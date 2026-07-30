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

    // Original single-client interface.
    // Kept so existing tests continue working.
    bool accept_client();

    std::vector<std::byte> receive();

    bool send(std::span<const std::byte> data);

    // New multi-client interface.
    int accept_connection();

    static std::vector<std::byte> receive_from(
        int client_socket
    );

    static bool send_to(
        int client_socket,
        std::span<const std::byte> data
    );

    static void close_connection(int client_socket);

private:
    std::uint16_t port_;

    int listen_socket_;
    int client_socket_;
};

}  // namespace exchange