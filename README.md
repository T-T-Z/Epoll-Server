# Epoll Server

A simple, self contained, header‑only C++ server that uses Linux epoll and threaded workers. Designed for when you want to handle many concurrent connections but don't want to think too hard about it and just want it to work.

# What it has:

  epoll for scalable, non‑blocking I/O.
    
  hooks for onConnect, onDisconnect, and onMessage.

  broadcast & send helper functions to forward messages to 1 or all clients.

  handles tcp and udp, pairs them up with handshake on connection.

  pause and resume accepting new connections without killing the server.

# Why it good:

  handles thousands of concurrent connections without the one thread per client overhead.

  doesn't just implode when stopped.

  it's just a header file, very easy, very clean.

# How to use:
Example basic message server
```cpp
#include "epollServer.hpp"

int main() {
    EpollServer server(9000, 20000, 4, 1048, 1048); // port, max_fds, workers, TCP_read_buffer_size, UDP_read_buffer_size

    if (!server.start()) {
        std::cerr << "Failed to start server\n";
        return 1;
    }

    server.onConnect([](int fd) {
        std::cout << "[+] Client connected: " << fd << "\n";
    });

    server.onDisconnect([](int fd) {
        std::cout << "[-] Client disconnected: " << fd << "\n";
    });

    server.onMessage([&server](int fd, const std::string& msg, bool isudp) {
        //std::cout << "[msg] From " << fd << ": " << msg;
        if(isudp){
            server.broadcastUdp("Echo: " + msg, fd);
        }else{
            server.broadcastTcp("Echo: " + msg, fd);
        }
    });

    server.run();
}
```

Compile with
```bash
g++ -o server server.cpp -pthread 
```

Example Client
```cpp
#include "epollClient.hpp"

int main() {
    EpollClient client("127.0.0.1", 9000, 2000, 1048, 1048); // host, port, reconnect_delay_in_ms, workers, TCP_read_buffer_size, UDP_read_buffer_size

    client.onMessage([](const std::string& msg, bool isUdp){
        std::cout << (isUdp ? "[UDP]" : "[TCP]") << " " << msg << "\n";
    });

    client.connectToServer();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line=="quit") break;

        if (line.rfind("t:",0)==0) {
            std::string msg = line.substr(2);
            client.sendTcp(msg);
        } else if (line.rfind("u:",0)==0) {
            std::string msg = line.substr(2);
            client.sendUdp(msg);
        }
    }

    client.disconnect();
}

```

Compile with
```bash
g++ -o client client.cpp -pthread 
```
