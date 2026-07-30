#include "exchange/tcp_server.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#ifdef _WIN32
#error "Windows support not implemented yet."
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace exchange {

namespace {

constexpr std::size_t kBufferSize = 4096;
constexpr int kListenBacklog = 16;

bool send_all(
    int socket,
    std::span<const std::byte> data
) {
    std::size_t total_sent = 0;

    while (total_sent < data.size()) {
        const auto sent = ::send(
            socket,
            data.data() + total_sent,
            data.size() - total_sent,
            0
        );

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (sent == 0) {
            return false;
        }

        total_sent += static_cast<std::size_t>(sent);
    }

    return true;
}

void configure_client_socket(int socket) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;

    ::setsockopt(
        socket,
        SOL_SOCKET,
        SO_NOSIGPIPE,
        &enabled,
        sizeof(enabled)
    );
#else
    static_cast<void>(socket);
#endif
}

}  // namespace

TcpServer::TcpServer(std::uint16_t port)
    : port_(port),
      listen_socket_(-1),
      client_socket_(-1) {}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start() {
    if (listen_socket_ >= 0) {
        return true;
    }

    listen_socket_ = ::socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (listen_socket_ < 0) {
        return false;
    }

    int enable = 1;

    if (::setsockopt(
            listen_socket_,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enable,
            sizeof(enable)
        ) < 0) {

        stop();
        return false;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port_);

    if (::bind(
            listen_socket_,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) < 0) {

        stop();
        return false;
    }

    if (::listen(
            listen_socket_,
            kListenBacklog
        ) < 0) {

        stop();
        return false;
    }

    return true;
}

void TcpServer::stop() {
    if (client_socket_ >= 0) {
        ::shutdown(client_socket_, SHUT_RDWR);
        ::close(client_socket_);
        client_socket_ = -1;
    }

    if (listen_socket_ >= 0) {
        ::shutdown(listen_socket_, SHUT_RDWR);
        ::close(listen_socket_);
        listen_socket_ = -1;
    }
}

bool TcpServer::accept_client() {
    client_socket_ = accept_connection();
    return client_socket_ >= 0;
}

std::vector<std::byte> TcpServer::receive() {
    return receive_from(client_socket_);
}

bool TcpServer::send(
    std::span<const std::byte> data
) {
    return send_to(client_socket_, data);
}

int TcpServer::accept_connection() {
    if (listen_socket_ < 0) {
        return -1;
    }

    sockaddr_in client_address {};
    socklen_t client_address_length =
        sizeof(client_address);

    int client_socket = -1;

    while (client_socket < 0) {
        client_socket = ::accept(
            listen_socket_,
            reinterpret_cast<sockaddr*>(
                &client_address
            ),
            &client_address_length
        );

        if (client_socket < 0 &&
            errno == EINTR) {

            continue;
        }

        break;
    }

    if (client_socket >= 0) {
        configure_client_socket(client_socket);
    }

    return client_socket;
}

std::vector<std::byte> TcpServer::receive_from(
    int client_socket
) {
    if (client_socket < 0) {
        return {};
    }

    std::vector<std::byte> buffer(kBufferSize);

    while (true) {
        const auto received = ::recv(
            client_socket,
            buffer.data(),
            buffer.size(),
            0
        );

        if (received < 0 && errno == EINTR) {
            continue;
        }

        if (received <= 0) {
            return {};
        }

        buffer.resize(
            static_cast<std::size_t>(received)
        );

        return buffer;
    }
}

bool TcpServer::send_to(
    int client_socket,
    std::span<const std::byte> data
) {
    if (client_socket < 0) {
        return false;
    }

    return send_all(client_socket, data);
}

void TcpServer::close_connection(
    int client_socket
) {
    if (client_socket < 0) {
        return;
    }

    ::shutdown(client_socket, SHUT_RDWR);
    ::close(client_socket);
}

}  // namespace exchange