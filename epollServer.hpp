#pragma once

#include <iostream>
#include <unordered_set>
#include <vector>
#include <string>
#include <functional>
#include <thread>
#include <csignal>
#include <atomic>
#include <mutex>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <cstring>

class EpollServer {
public:
    using ConnectionHandler = std::function<void(int)>;
    using MessageHandler    = std::function<void(int,const std::string&)>;

    EpollServer(int port, rlim_t max_fds = 65535, int workers = 1)
        : port_(port), max_fds_(max_fds), num_workers_(workers)
    {
        std::signal(SIGINT,  [](int){ stop_requested_ = true; });
        std::signal(SIGTERM, [](int){ stop_requested_ = true; });
    }

    bool start() {
        bumpFdLimit(max_fds_);

        int sfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sfd < 0) { perror("socket"); return false; }
        server_fd_.store(sfd);

        int opt = 1;
        (void)setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        (void)setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        makeNonBlocking(sfd);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return false; }
        if (listen(sfd, SOMAXCONN) < 0) { perror("listen"); return false; }

        event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (event_fd_ < 0) { perror("eventfd"); return false; }

        return true;
    }

    void run() {
        std::cout << "Listening on " << port_ << " with " << num_workers_ << " worker(s)\n";

        workers_.reserve(num_workers_);
        for (int i=0;i<num_workers_;++i) {
            workers_.emplace_back([this]{ loop(); });
        }

        for (auto &t: workers_) if (t.joinable()) t.join();
        workers_.clear();

        closeListeningSocket();
        if (event_fd_>=0) { ::close(event_fd_); event_fd_=-1; }

        stop();
    }

    void onConnect(ConnectionHandler cb)    { connect_cb_ = std::move(cb); }
    void onDisconnect(ConnectionHandler cb) { disconnect_cb_ = std::move(cb); }
    void onMessage(MessageHandler cb)       { message_cb_ = std::move(cb); }

    void send(int fd,const std::string& data) { writeAll(fd,data); }

    void broadcast(const std::string& data,int exclude=-1) {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        for (int c: clients_) if (c!=exclude) writeAll(c,data);
    }

    void shutdown() {
        if (!stop_requested_.exchange(true)) {
            if (event_fd_ >= 0) {
                uint64_t one = 1;
                for (int i=0; i<num_workers_; ++i) {
                    ssize_t n = ::write(event_fd_, &one, sizeof(one));
                }
            }
        }
    }

    void pause()  { paused_=true; }
    void resume() { paused_=false; }

    void stop() {
        std::unordered_set<int> tmp;
        { std::lock_guard<std::mutex> lk(clients_mtx_); tmp.swap(clients_); }
        for (int fd: tmp) ::close(fd);
    }

    ~EpollServer() {
        shutdown();
        stop();
        closeListeningSocket();
        if (event_fd_>=0) { ::close(event_fd_); event_fd_=-1; }
    }

private:
    int port_;
    rlim_t max_fds_;
    int num_workers_;
    std::atomic<int> server_fd_{-1};
    int event_fd_{-1};
    std::vector<std::thread> workers_;

    std::unordered_set<int> clients_;
    std::mutex clients_mtx_;

    ConnectionHandler connect_cb_;
    ConnectionHandler disconnect_cb_;
    MessageHandler message_cb_;

    static inline std::atomic<bool> stop_requested_{false};
    static inline std::atomic<bool> paused_{false};

    static void makeNonBlocking(int fd) {
        int flags = fcntl(fd,F_GETFL,0);
        if (flags<0) flags=0;
        fcntl(fd,F_SETFL,flags|O_NONBLOCK);
    }

    static void bumpFdLimit(rlim_t max) {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE,&rl)==0) {
            rl.rlim_cur = std::min(max, rl.rlim_max);
            (void)setrlimit(RLIMIT_NOFILE,&rl);
        }
    }

    void closeListeningSocket() {
        int sfd = server_fd_.exchange(-1);
        if (sfd>=0) ::close(sfd);
    }

    void loop() {
        int ep = epoll_create1(EPOLL_CLOEXEC);
        if (ep<0) { perror("epoll_create1"); return; }

        epoll_event ev{};
        ev.events=EPOLLIN; ev.data.fd=event_fd_;
        (void)epoll_ctl(ep,EPOLL_CTL_ADD,event_fd_,&ev);

        int sfd = server_fd_.load();
        if (sfd>=0) {
            ev.events=EPOLLIN|EPOLLEXCLUSIVE; ev.data.fd=sfd;
            (void)epoll_ctl(ep,EPOLL_CTL_ADD,sfd,&ev);
        }

        std::vector<epoll_event> events(64);

        while (!stop_requested_) {
            int n = epoll_wait(ep,events.data(),(int)events.size(),200);
            if (n < 0) {
                if (errno != EINTR) break;
            }
            if (n == 0) continue;

            for (int i=0;i<n;++i) {
                int fd = events[i].data.fd;
                if (fd == event_fd_) {
                    uint64_t v = 0;
                    ssize_t r = ::read(event_fd_, &v, sizeof(v));
                    if (r < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            perror("read eventfd");
                        }
                    }
                    continue;
                }
                if (fd==sfd) {
                    if (!paused_) acceptClients(ep);
                    else drainAccept();
                } else {
                    handleClient(fd);
                }
            }
        }
        ::close(ep);
    }

    void acceptClients(int ep) {
        int sfd = server_fd_.load();
        while (true) {
            int cfd = accept4(sfd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if (cfd<0) break;
            epoll_event ev{}; ev.events=EPOLLIN; ev.data.fd=cfd;
            (void)epoll_ctl(ep,EPOLL_CTL_ADD,cfd,&ev);
            { std::lock_guard<std::mutex> lk(clients_mtx_); clients_.insert(cfd); }
            if (connect_cb_) connect_cb_(cfd);
        }
    }

    void drainAccept() {
        int sfd = server_fd_.load();
        while (true) {
            int cfd = accept4(sfd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if (cfd<0) break;
            ::close(cfd);
        }
    }

    void handleClient(int fd) {
        char buf[1024];
        int n = ::read(fd,buf,sizeof(buf));
        if (n<=0) {
            ::close(fd);
            { std::lock_guard<std::mutex> lk(clients_mtx_); clients_.erase(fd); }
            if (disconnect_cb_) disconnect_cb_(fd);
        } else {
            if (message_cb_) message_cb_(fd,std::string(buf,n));
        }
    }

    static bool writeAll(int fd,const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::write(fd, data.data() + off, data.size() - off);
            if (n < 0) {
                if (errno != EINTR) return false;
            }
            off += (size_t)n;
        }
        return true;
    }
};
