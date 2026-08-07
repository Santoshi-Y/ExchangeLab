#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace exchange {

struct MulticastConfig {
    std::string group {"239.255.0.1"};
    std::uint16_t port {9100};
    std::uint8_t ttl {1};
};

class MulticastPublisher {
public:
    explicit MulticastPublisher(MulticastConfig config);

    ~MulticastPublisher();

    MulticastPublisher(const MulticastPublisher&) = delete;
    MulticastPublisher& operator=(const MulticastPublisher&) = delete;
    MulticastPublisher(MulticastPublisher&&) = delete;
    MulticastPublisher& operator=(MulticastPublisher&&) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;

    [[nodiscard]] bool send(
        std::span<const std::byte> datagram
    ) const noexcept;

    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] const MulticastConfig&
    config() const noexcept;

private:
    MulticastConfig config_;
    int socket_ {-1};

    struct Destination;
    Destination* destination_ {nullptr};
};

}  // namespace exchange