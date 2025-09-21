# Epoll Server

A simple, self contained, header‑only C++ server that uses Linux epoll and threaded workers. Designed for when you want to handle many concurrent connections but don't want to think too hard about it and just want it to work.

# What it has:

  epoll for scalable, non‑blocking I/O.
    
  hooks for onConnect, onDisconnect, and onMessage.

  broadcast & send helper functions to forward messages to 1 or all clients.

  pause and resume accepting new connections without killing the server.

# Why it good:

  handles thousands of concurrent connections without the one thread per client overhead.

  doesn't just implode when stopped.

  it's just a single header file, very easy, very clean.

# How to use:
Example basic message server
```cpp
#include "epollServer.hpp"

int main() {
    EpollServer server(5001, 20000, 4, 128); // port, max_fds, workers, read_buffer_size

    if (!server.start()) {
        std::cerr << "Failed to start server\n";
        return 1;
    }

    server.onConnect([](int fd) {
        std::cout << "Client connected: " << fd << "\n";
    });

    server.onDisconnect([](int fd) {
        std::cout << "Client disconnected: " << fd << "\n";
    });

    server.onMessage([&server](int fd, const std::string& msg) {
        std::cout << fd << ": " << msg;
        server.broadcast("Echo: " + msg, fd);
    });

    server.run(); 
}
```

Compile with
```bash
g++ -o server main.cpp -pthread 
```
