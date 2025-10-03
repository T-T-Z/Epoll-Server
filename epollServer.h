#pragma once
#include <iostream>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>
#include <functional>
#include <thread>
#include <csignal>
#include <atomic>
#include <mutex>
#include <random>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/eventfd.h>
#include <cstring>

class EpollServer {
public:
    using ConnectionHandler = std::function<void(int)>;
    using MessageHandler    = std::function<void(int,const std::string&,bool isUdp)>;

    EpollServer(int port, rlim_t max_fds = 65535, int workers = 1, int tcp_read_buffer_size = 1048, int udp_read_buffer_size = 1048)
        : port_(port), max_fds_(max_fds), num_workers_(workers), tcp_read_buffer_size(tcp_read_buffer_size), udp_read_buffer_size(udp_read_buffer_size)
    {
        std::signal(SIGINT,  [](int){ stop_requested_ = true; });
        std::signal(SIGTERM, [](int){ stop_requested_ = true; });
    }

    bool start() {
        bumpFdLimit(max_fds_);

        int sfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sfd < 0) { perror("tcp socket"); return false; }
        server_fd_.store(sfd);

        int opt = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        makeNonBlocking(sfd);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("tcp bind"); return false; }
        if (listen(sfd, SOMAXCONN) < 0) { perror("listen"); return false; }

        udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_fd_ < 0) { perror("udp socket"); return false; }
        setsockopt(udp_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(udp_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        makeNonBlocking(udp_fd_);
        if (bind(udp_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("udp bind"); return false; }

        event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (event_fd_ < 0) { perror("eventfd"); return false; }

        return true;
    }

    void run() {
        workers_.reserve(num_workers_);
        for (int i=0;i<num_workers_;++i) { workers_.emplace_back([this]{ loop(); }); }

        for (auto &t: workers_) { if (t.joinable()) {t.join(); } }
        workers_.clear();

        stop();
        cleanup();
    }

    void onConnect(ConnectionHandler cb)    { connect_cb_ = std::move(cb); }
    void onDisconnect(ConnectionHandler cb) { disconnect_cb_ = std::move(cb); }
    void onMessage(MessageHandler cb)       { message_cb_ = std::move(cb); }

    bool sendTcp(int fd,const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::write(fd, data.data() + off, data.size() - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            off += (size_t)n;
        }
        return true;
    }

    void sendUdp(int tcp_fd,const std::string& data) {
        std::lock_guard<std::mutex> lk(map_mtx_);
        auto it = udp_map_.find(tcp_fd);
        if (it==udp_map_.end()) return;
        const sockaddr_in &peer = it->second;
        ::sendto(udp_fd_, data.data(), data.size(), 0, (sockaddr*)&peer, sizeof(peer));
    }

    void broadcastTcp(const std::string& data,int exclude=-1) {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        for (int c: clients_) {if (c!=exclude) sendTcp(c,data);}
    }

    void broadcastUdp(const std::string& data,int exclude=-1) {
        std::lock_guard<std::mutex> lk(map_mtx_);
        for (auto &kv: udp_map_) {
            if (kv.first==exclude) continue;
            const sockaddr_in &peer = kv.second;
            ::sendto(udp_fd_, data.data(), data.size(), 0, (sockaddr*)&peer, sizeof(peer));
        }
    }

    void shutdown() {
        if (!stop_requested_.exchange(true)) {
            if (event_fd_ >= 0) {
                uint64_t one = 1;
                for (int i=0; i<num_workers_; ++i) {
                    int n = ::write(event_fd_, &one, sizeof(one));
                }
            }
        }
    }
    void pause()  { paused_ = true; }
    void resume() { paused_ = false; }

    void stop() {
        std::unordered_set<int> tmp;
        { std::lock_guard<std::mutex> lk(clients_mtx_); tmp.swap(clients_); }
        for (int fd: tmp) { ::close(fd); }
    }

    ~EpollServer() { shutdown(); stop(); cleanup(); }

private:
    int port_;
    rlim_t max_fds_;
    int num_workers_;
    int tcp_read_buffer_size;
    int udp_read_buffer_size;
    std::atomic<int> server_fd_{-1};
    int udp_fd_{-1};
    int event_fd_{-1};
    std::vector<std::thread> workers_;

    std::unordered_set<int> clients_;
    std::unordered_map<int,sockaddr_in> udp_map_;           // tcp_fd -> UDP endpoint
    std::unordered_map<std::string,int> endpoint_to_tcp_;   // "ip:port" -> tcp_fd

    std::unordered_map<int, uint64_t> tcp_to_sid_;          // tcp_fd -> sid
    std::unordered_map<uint64_t, int> sid_to_tcp_;          // sid -> tcp_fd
    std::mt19937_64 rng_{ std::random_device{}() };

    std::mutex clients_mtx_;
    std::mutex map_mtx_;

    ConnectionHandler connect_cb_;
    ConnectionHandler disconnect_cb_;
    MessageHandler message_cb_;

    static inline std::atomic<bool> stop_requested_{false};
    static inline std::atomic<bool> paused_{false};

    void cleanup() {
        int sfd = server_fd_.exchange(-1);
        if (sfd>=0) ::close(sfd);
        if (udp_fd_>=0) ::close(udp_fd_); udp_fd_=-1;
        if (event_fd_>=0) ::close(event_fd_); event_fd_=-1;
    }

    static void makeNonBlocking(int fd) {
        int flags = fcntl(fd,F_GETFL,0);
        if (flags<0) flags=0;
        fcntl(fd,F_SETFL,flags|O_NONBLOCK);
    }

    static void bumpFdLimit(rlim_t max) {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE,&rl)==0) {
            rl.rlim_cur = std::min(max, rl.rlim_max);
            setrlimit(RLIMIT_NOFILE,&rl);
        }
    }

    static std::string endpointKey(const sockaddr_in& sa) {
        char ip[INET_ADDRSTRLEN]{0};
        ::inet_ntop(AF_INET, (void*)&sa.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(sa.sin_port));
    }

    void loop() {
        int ep = epoll_create1(EPOLL_CLOEXEC);
        if (ep<0) { perror("epoll_create1"); return; }

        epoll_event ev{};
        ev.events=EPOLLIN; ev.data.fd=event_fd_;
        epoll_ctl(ep,EPOLL_CTL_ADD,event_fd_,&ev);

        int sfd = server_fd_.load();
        if (sfd>=0) {
            ev.events=EPOLLIN|EPOLLEXCLUSIVE; ev.data.fd=sfd;
            epoll_ctl(ep,EPOLL_CTL_ADD,sfd,&ev);
        }
        if (udp_fd_>=0) {
            ev.events=EPOLLIN; ev.data.fd=udp_fd_;
            epoll_ctl(ep,EPOLL_CTL_ADD,udp_fd_,&ev);
        }

        std::vector<epoll_event> events(64);

        while (!stop_requested_) {
            int n = epoll_wait(ep,events.data(),(int)events.size(),200);
            if (n <= 0) continue;

            for (int i=0;i<n;++i) {
                int fd = events[i].data.fd;
                if (fd == event_fd_) {
                    uint64_t v; int n = ::read(event_fd_, &v, sizeof(v));
                } else if (fd==sfd) {
                    if (!paused_) acceptClients(ep);
                    else drainAccept();
                } else if (fd==udp_fd_) {
                    if (!paused_) handleUdp();
                } else {
                    if (!paused_) handleTcp(fd);
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
            epoll_ctl(ep,EPOLL_CTL_ADD,cfd,&ev);

            {
                std::lock_guard<std::mutex> lk(clients_mtx_);
                clients_.insert(cfd);
            }

            uint64_t sid = rng_();
            {
                std::lock_guard<std::mutex> lk(map_mtx_);
                tcp_to_sid_[cfd] = sid;
                sid_to_tcp_[sid] = cfd;
            }

            sendTcp(cfd, std::to_string(sid));

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

    void handleTcp(int fd) {
        char buf[tcp_read_buffer_size];
        int n = ::read(fd,buf,sizeof(buf));
        if (n<=0) {
            ::close(fd);
            {
                std::lock_guard<std::mutex> lk(clients_mtx_);
                clients_.erase(fd);
            }
            {
                std::lock_guard<std::mutex> lk(map_mtx_);
                auto it = udp_map_.find(fd);
                if (it != udp_map_.end()) {
                    endpoint_to_tcp_.erase(endpointKey(it->second));
                    udp_map_.erase(it);
                }
                auto sit = tcp_to_sid_.find(fd);
                if (sit != tcp_to_sid_.end()) {
                    sid_to_tcp_.erase(sit->second);
                    tcp_to_sid_.erase(sit);
                }
            }
            if (disconnect_cb_) disconnect_cb_(fd);
        } else {
            if (message_cb_) message_cb_(fd,std::string(buf,n),false);
        }
    }

    void handleUdp() {
        char buf[udp_read_buffer_size];
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int n = ::recvfrom(udp_fd_, buf, sizeof(buf), 0, (sockaddr*)&peer, &plen);
        if (n <= 0) return;

        std::string payload(buf, n);
        int tcp_fd = -1;
        std::string key = endpointKey(peer);

        {
            std::lock_guard<std::mutex> lk(map_mtx_);

            auto it = endpoint_to_tcp_.find(key);
            if (it != endpoint_to_tcp_.end()) {
                tcp_fd = it->second;
            } else {
                uint64_t sid = 0;
                try {
                    sid = std::stoull(payload);
                } catch (...) { sid = 0; }

                if (sid) {
                    auto sit = sid_to_tcp_.find(sid);
                    if (sit != sid_to_tcp_.end()) {
                        tcp_fd = sit->second;
                        udp_map_[tcp_fd] = peer;
                        endpoint_to_tcp_[key] = tcp_fd;
                        return;
                    }
                }
            }
        }

        if (tcp_fd != -1) {
            if (message_cb_) message_cb_(tcp_fd, payload, true);
        }
    }
};
