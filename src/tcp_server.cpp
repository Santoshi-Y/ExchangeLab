#include "exchange/tcp_server.hpp"

#include <cerrno>
#include <cstring>

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

}

TcpServer::TcpServer(std::uint16_t port)
    : port_(port),
      listen_socket_(-1),
      client_socket_(-1) {}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start() {
    listen_socket_ = ::socket(AF_INET, SOCK_STREAM, 0);

    if (listen_socket_ < 0) {
        return false;
    }

    int enable = 1;
    ::setsockopt(
        listen_socket_,
        SOL_SOCKET,
        SO_REUSEADDR,
        &enable,
        sizeof(enable));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port_);

    if (::bind(
            listen_socket_,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)) < 0) {
        stop();
        return false;
    }

    if (::listen(listen_socket_, 16) < 0) {
        stop();
        return false;
    }

    return true;
}

void TcpServer::stop() {

    if (client_socket_ >= 0) {
        ::close(client_socket_);
        client_socket_ = -1;
    }

    if (listen_socket_ >= 0) {
        ::close(listen_socket_);
        listen_socket_ = -1;
    }
}

bool TcpServer::accept_client() {

    sockaddr_in client{};
    socklen_t length = sizeof(client);

    client_socket_ =
        ::accept(
            listen_socket_,
            reinterpret_cast<sockaddr*>(&client),
            &length);

    return client_socket_ >= 0;
}

std::vector<std::byte> TcpServer::receive() {

    std::vector<std::byte> buffer(kBufferSize);

    const auto received =
        ::recv(
            client_socket_,
            buffer.data(),
            buffer.size(),
            0);

    if (received <= 0) {
        return {};
    }

    buffer.resize(static_cast<std::size_t>(received));

    return buffer;
}

bool TcpServer::send(std::span<const std::byte> data) {

    const auto sent =
        ::send(
            client_socket_,
            data.data(),
            data.size(),
            0);

    return sent ==
           static_cast<ssize_t>(data.size());
}

}  // namespace exchange