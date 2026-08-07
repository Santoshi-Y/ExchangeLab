#include "exchange/multicast_publisher.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#ifdef _WIN32
#error "Windows support not implemented yet."
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace exchange {

struct MulticastPublisher::Destination {
    sockaddr_in address {};
};

MulticastPublisher::MulticastPublisher(
    MulticastConfig config
)
    : config_(std::move(config)) {}

MulticastPublisher::~MulticastPublisher() {
    stop();
}

bool MulticastPublisher::start() {
    if (socket_ >= 0) {
        return true;
    }

    socket_ = ::socket(
        AF_INET,
        SOCK_DGRAM,
        0
    );

    if (socket_ < 0) {
        return false;
    }

    const unsigned char ttl =
        static_cast<unsigned char>(config_.ttl);

    if (
        ::setsockopt(
            socket_,
            IPPROTO_IP,
            IP_MULTICAST_TTL,
            &ttl,
            sizeof(ttl)
        ) != 0
    ) {
        stop();
        return false;
    }

    const unsigned char loopback = 1;

    if (
        ::setsockopt(
            socket_,
            IPPROTO_IP,
            IP_MULTICAST_LOOP,
            &loopback,
            sizeof(loopback)
        ) != 0
    ) {
        stop();
        return false;
    }

    auto destination =
        std::make_unique<Destination>();

    destination->address.sin_family = AF_INET;
    destination->address.sin_port =
        htons(config_.port);

    if (
        ::inet_pton(
            AF_INET,
            config_.group.c_str(),
            &destination->address.sin_addr
        ) != 1
    ) {
        stop();
        return false;
    }

    destination_ = destination.release();
    return true;
}

void MulticastPublisher::stop() noexcept {
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }

    delete destination_;
    destination_ = nullptr;
}

bool MulticastPublisher::send(
    std::span<const std::byte> datagram
) const noexcept {
    if (
        socket_ < 0 ||
        destination_ == nullptr ||
        datagram.empty()
    ) {
        return false;
    }

    const auto sent = ::sendto(
        socket_,
        datagram.data(),
        datagram.size(),
        0,
        reinterpret_cast<const sockaddr*>(
            &destination_->address
        ),
        sizeof(destination_->address)
    );

    return sent ==
        static_cast<ssize_t>(datagram.size());
}

bool MulticastPublisher::is_open() const noexcept {
    return socket_ >= 0;
}

const MulticastConfig&
MulticastPublisher::config() const noexcept {
    return config_;
}

}  // namespace exchange