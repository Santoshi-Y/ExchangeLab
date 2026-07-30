#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace exchange {

class TcpServer {
public:
    explicit TcpServer(std::uint16_t port);

    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    bool start();

    void stop();

    bool accept_client();

    std::vector<std::byte> receive();

    bool send(std::span<const std::byte> data);

private:
    std::uint16_t port_;

    int listen_socket_;
    int client_socket_;
};

}  // namespace exchange