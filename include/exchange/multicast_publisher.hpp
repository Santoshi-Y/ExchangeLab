#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace exchange {

struct MulticastConfig {
    std::string group {"239.255.0.1"};
    std::uint16_t port {9100};
    std::uint8_t ttl {1};
};

struct MulticastPublisherStats {
    std::uint64_t enqueued {0};
    std::uint64_t sent {0};
    std::uint64_t dropped {0};
    std::uint64_t send_errors {0};
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

    /*
     * Non-blocking with respect to the UDP syscall.
     *
     * The producer copies the datagram into a fixed-capacity
     * ring buffer. A dedicated publisher thread performs sendto().
     *
     * Returns false when:
     *  - the publisher is not running,
     *  - the datagram is too large, or
     *  - the ring buffer is full.
     *
     * UDP market data is intentionally allowed to drop rather
     * than blocking the order-processing path.
     */
    [[nodiscard]] bool send(
        std::span<const std::byte> datagram
    ) noexcept;

    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] const MulticastConfig&
    config() const noexcept;

    [[nodiscard]] MulticastPublisherStats
    stats() const noexcept;

    static constexpr std::size_t
    queue_capacity() noexcept {
        return kQueueCapacity;
    }

    static constexpr std::size_t
    maximum_datagram_size() noexcept {
        return kMaximumDatagramSize;
    }

private:
    static constexpr std::size_t kQueueCapacity = 4096;
    static constexpr std::size_t kMaximumDatagramSize = 512;

    struct Slot {
        std::array<std::byte, kMaximumDatagramSize> bytes {};
        std::size_t size {0};
    };

    struct Destination;

    void publisher_loop() noexcept;

    [[nodiscard]] bool send_datagram(
        std::span<const std::byte> datagram
    ) noexcept;

    MulticastConfig config_;

    int socket_ {-1};
    Destination* destination_ {nullptr};

    std::array<Slot, kQueueCapacity> queue_ {};
    std::size_t read_index_ {0};
    std::size_t write_index_ {0};
    std::size_t queued_count_ {0};

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread publisher_thread_;

    std::atomic<bool> running_ {false};

    std::atomic<std::uint64_t> enqueued_ {0};
    std::atomic<std::uint64_t> sent_ {0};
    std::atomic<std::uint64_t> dropped_ {0};
    std::atomic<std::uint64_t> send_errors_ {0};
};

}  // namespace exchange