#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>

#include "exchange/lock_free_ring.hpp"

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
    std::uint64_t queue_depth {0};
    std::uint64_t max_queue_depth {0};
    std::uint64_t producer_shards_used {0};
    std::uint64_t producer_registration_failures {0};
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
     * Each producer thread is assigned one private SPSC queue the first
     * time it calls send() during a publisher generation. Producers never
     * contend on one shared enqueue index; the publisher thread drains the
     * shards round-robin and owns sendto().
     *
     * Returns false when the publisher is stopped, the datagram is invalid,
     * the producer shard is full, or the fixed producer-shard budget is
     * exhausted. Market data is dropped rather than blocking order entry.
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
    producer_shard_count() noexcept {
        return kProducerShardCount;
    }

    static constexpr std::size_t
    queue_capacity_per_producer() noexcept {
        return kQueueCapacityPerProducer;
    }

    static constexpr std::size_t
    queue_capacity() noexcept {
        return kProducerShardCount * kQueueCapacityPerProducer;
    }

    static constexpr std::size_t
    maximum_datagram_size() noexcept {
        return kMaximumDatagramSize;
    }

    static constexpr bool
    queue_indices_always_lock_free() noexcept {
        return Queue::index_atomics_always_lock_free();
    }

private:
    static constexpr std::size_t kProducerShardCount = 256;
    static constexpr std::size_t kQueueCapacityPerProducer = 32;
    static constexpr std::size_t kMaximumDatagramSize = 512;
    static constexpr std::size_t kInvalidShard = kProducerShardCount;

    struct Slot {
        std::array<std::byte, kMaximumDatagramSize> bytes {};
        std::size_t size {0};
    };

    using Queue = SpscRing<Slot, kQueueCapacityPerProducer>;

    struct ProducerShard {
        Queue queue {};
        std::atomic<bool> active_sender {false};
        std::atomic<std::uint64_t> enqueued {0};
        std::atomic<std::uint64_t> dropped {0};
        std::atomic<std::uint64_t> max_depth {0};
    };

    struct Destination;

    void publisher_loop() noexcept;

    [[nodiscard]] bool send_datagram(
        std::span<const std::byte> datagram
    ) noexcept;

    [[nodiscard]] std::size_t
    producer_shard_for_current_thread() noexcept;

    [[nodiscard]] bool all_queues_empty() const noexcept;
    [[nodiscard]] bool any_active_senders() const noexcept;
    [[nodiscard]] std::uint64_t aggregate_queue_depth() const noexcept;

    void signal_consumer() noexcept;
    static void update_max_depth(
        ProducerShard& shard,
        std::size_t depth
    ) noexcept;

    MulticastConfig config_;

    int socket_ {-1};
    Destination* destination_ {nullptr};

    std::array<ProducerShard, kProducerShardCount> shards_ {};
    std::thread publisher_thread_;

    std::atomic<bool> running_ {false};
    std::atomic<std::uint64_t> generation_ {0};
    std::atomic<std::size_t> next_producer_shard_ {0};

    // Set while work may exist. Producers normally only read this flag;
    // the first producer after an idle period performs the RMW + notify.
    std::atomic_flag wake_requested_ = ATOMIC_FLAG_INIT;

    std::atomic<std::uint64_t> sent_ {0};
    std::atomic<std::uint64_t> send_errors_ {0};
    std::atomic<std::uint64_t> administrative_dropped_ {0};
    std::atomic<std::uint64_t> producer_registration_failures_ {0};
};

}  // namespace exchange