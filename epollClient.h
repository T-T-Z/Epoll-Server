#pragma once
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <chrono>
#include <random>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cstring>

class EpollClient {
public:
    using MessageHandler = std::function<void(const std::string&, bool isUdp)>;

    EpollClient(const std::string& host, int port, int reconnectDelayMs = 2000, int tcp_read_buffer_size = 1048, int udp_read_buffer_size = 1048)
    : host_(host), port_(port), reconnectDelayMs_(reconnectDelayMs), tcp_read_buffer_size(tcp_read_buffer_size), udp_read_buffer_size(udp_read_buffer_size) {}

    ~EpollClient() { disconnect(); }

    void onMessage(MessageHandler cb) { msg_cb_ = std::move(cb); }

    bool connectToServer() {
        disconnect();
        running_ = true;
        worker_thread_ = std::thread([this]{ mainLoop(); });
        return true;
    }

    void disconnect() {
        running_ = false;
        connected_ = false;
        if (tcp_fd_>=0) { ::close(tcp_fd_); tcp_fd_=-1; }
        if (udp_fd_>=0) { ::close(udp_fd_); udp_fd_=-1; }
        if (worker_thread_.joinable()) worker_thread_.join();
    }

    void sendTcp(const std::string& msg) { if (connected_ && tcp_fd_>=0) int n = ::write(tcp_fd_, msg.data(), msg.size()); }

    void sendUdp(const std::string& msg) { if (connected_ && udp_fd_>=0) ::sendto(udp_fd_, msg.data(), msg.size(), 0, (sockaddr*)&udp_serv_, sizeof(udp_serv_)); }

private:
    std::string host_;
    int port_;
    int reconnectDelayMs_;
    int tcp_read_buffer_size;
    int udp_read_buffer_size;
    int tcp_fd_{-1};
    int udp_fd_{-1};
    sockaddr_in udp_serv_{};
    uint64_t sid_{0};

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread worker_thread_;

    MessageHandler msg_cb_;

    void mainLoop() {
        while (running_) {
            if (!establishConnection()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(reconnectDelayMs_));
                continue;
            }
            connected_ = true;
            std::cout << "Connected to server\n";

            std::thread tcp_thread([this]{ tcpLoop(); });
            std::thread udp_thread([this]{ udpLoop(); });

            while (running_ && connected_) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }

            if (tcp_thread.joinable()) tcp_thread.join();
            if (udp_thread.joinable()) udp_thread.join();

            if (tcp_fd_>=0) ::close(tcp_fd_); tcp_fd_=-1;
            if (udp_fd_>=0) ::close(udp_fd_); udp_fd_=-1;

            if (running_) {
                std::cout << "Reconnecting in " << reconnectDelayMs_ << "ms\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(reconnectDelayMs_));
            }
        }
    }

    bool establishConnection() {
        tcp_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (tcp_fd_ < 0) { perror("tcp socket"); return false; }

        sockaddr_in serv{};
        serv.sin_family = AF_INET;
        serv.sin_port = htons(port_);
        if (::inet_pton(AF_INET, host_.c_str(), &serv.sin_addr) <= 0) { perror("inet_pton"); return false; }
        if (::connect(tcp_fd_, (sockaddr*)&serv, sizeof(serv)) < 0) { perror("connect"); return false; }

        char buf[256];
        int n = ::read(tcp_fd_, buf, sizeof(buf)-1);
        if (n <= 0) { std::cerr << "Failed to read SID\n"; return false; }
        buf[n] = '\0';
        std::string sid_msg(buf);
        sid_ = std::stoull(sid_msg);

        udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_fd_ < 0) { perror("udp socket"); return false; }
        udp_serv_ = serv;

        if (::sendto(udp_fd_, sid_msg.data(), sid_msg.size(), 0, (sockaddr*)&udp_serv_, sizeof(udp_serv_)) < 0) { perror("sendto"); return false; }
        return true;
    }

    void tcpLoop() {
        char buf[tcp_read_buffer_size];
        while (running_ && connected_) {
            int n = ::read(tcp_fd_, buf, sizeof(buf)-1);
            if (n <= 0) { connected_ = false; break; }
            buf[n] = '\0';
            if (msg_cb_) msg_cb_(std::string(buf,n), false);
        }
    }

    void udpLoop() {
        char buf[udp_read_buffer_size];
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        while (running_ && connected_) {
            int n = ::recvfrom(udp_fd_, buf, sizeof(buf)-1, 0, (sockaddr*)&from, &flen);
            if (n <= 0) { connected_ = false; break; }
            buf[n] = '\0';
            if (msg_cb_) msg_cb_(std::string(buf,n), true);
        }
    }
};
