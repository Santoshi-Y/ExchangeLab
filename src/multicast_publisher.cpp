#include "exchange/multicast_publisher.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

namespace {

std::atomic<std::uint64_t> next_publisher_generation {1};

struct ThreadProducerRegistration {
    const MulticastPublisher* publisher {nullptr};
    std::uint64_t generation {0};
    std::size_t shard {0};
};

thread_local ThreadProducerRegistration producer_registration;

}  // namespace

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
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
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
        ::close(socket_);
        socket_ = -1;
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
        ::close(socket_);
        socket_ = -1;
        return false;
    }

    auto destination = std::make_unique<Destination>();
    destination->address.sin_family = AF_INET;
    destination->address.sin_port = htons(config_.port);

    if (
        ::inet_pton(
            AF_INET,
            config_.group.c_str(),
            &destination->address.sin_addr
        ) != 1
    ) {
        ::close(socket_);
        socket_ = -1;
        return false;
    }

    destination_ = destination.release();

    for (ProducerShard& shard : shards_) {
        shard.queue.reset();
        shard.active_sender.store(false, std::memory_order_relaxed);
        shard.enqueued.store(0, std::memory_order_relaxed);
        shard.dropped.store(0, std::memory_order_relaxed);
        shard.max_depth.store(0, std::memory_order_relaxed);
    }

    next_producer_shard_.store(0, std::memory_order_relaxed);
    sent_.store(0, std::memory_order_relaxed);
    send_errors_.store(0, std::memory_order_relaxed);
    administrative_dropped_.store(0, std::memory_order_relaxed);
    producer_registration_failures_.store(0, std::memory_order_relaxed);

    wake_requested_.clear(std::memory_order_relaxed);

    const std::uint64_t generation =
        next_publisher_generation.fetch_add(
            1,
            std::memory_order_relaxed
        );
    generation_.store(generation, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    try {
        publisher_thread_ = std::thread(
            &MulticastPublisher::publisher_loop,
            this
        );
    } catch (...) {
        running_.store(false, std::memory_order_release);

        delete destination_;
        destination_ = nullptr;

        ::close(socket_);
        socket_ = -1;
        return false;
    }

    return true;
}

void MulticastPublisher::stop() noexcept {
    running_.store(false, std::memory_order_release);

    wake_requested_.test_and_set(std::memory_order_release);
    wake_requested_.notify_all();

    if (publisher_thread_.joinable()) {
        publisher_thread_.join();
    }

    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }

    delete destination_;
    destination_ = nullptr;
}

bool MulticastPublisher::send(
    std::span<const std::byte> datagram
) noexcept {
    if (
        !running_.load(std::memory_order_acquire) ||
        datagram.empty() ||
        datagram.size() > kMaximumDatagramSize
    ) {
        administrative_dropped_.fetch_add(
            1,
            std::memory_order_relaxed
        );
        return false;
    }

    const std::size_t shard_index =
        producer_shard_for_current_thread();

    if (shard_index == kInvalidShard) {
        administrative_dropped_.fetch_add(
            1,
            std::memory_order_relaxed
        );
        return false;
    }

    ProducerShard& shard = shards_[shard_index];

    // Per-shard activity tracking avoids one contended global in-flight
    // producer counter while still making shutdown/drain safe.
    shard.active_sender.store(true, std::memory_order_release);

    if (!running_.load(std::memory_order_acquire)) {
        shard.active_sender.store(false, std::memory_order_release);
        administrative_dropped_.fetch_add(
            1,
            std::memory_order_relaxed
        );
        return false;
    }

    const bool queued = shard.queue.try_emplace(
        [datagram](Slot& slot) noexcept {
            std::memcpy(
                slot.bytes.data(),
                datagram.data(),
                datagram.size()
            );
            slot.size = datagram.size();
        }
    );

    shard.active_sender.store(false, std::memory_order_release);

    if (!queued) {
        shard.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    shard.enqueued.fetch_add(1, std::memory_order_relaxed);
    update_max_depth(shard, shard.queue.size_approx());
    signal_consumer();
    return true;
}

bool MulticastPublisher::is_open() const noexcept {
    return running_.load(std::memory_order_acquire) &&
        socket_ >= 0 &&
        destination_ != nullptr;
}

const MulticastConfig&
MulticastPublisher::config() const noexcept {
    return config_;
}

MulticastPublisherStats
MulticastPublisher::stats() const noexcept {
    std::uint64_t enqueued = 0;
    std::uint64_t dropped = administrative_dropped_.load(
        std::memory_order_relaxed
    );
    std::uint64_t max_queue_depth = 0;

    for (const ProducerShard& shard : shards_) {
        enqueued += shard.enqueued.load(std::memory_order_relaxed);
        dropped += shard.dropped.load(std::memory_order_relaxed);
        max_queue_depth += shard.max_depth.load(
            std::memory_order_relaxed
        );
    }

    const std::size_t used = std::min(
        next_producer_shard_.load(std::memory_order_relaxed),
        kProducerShardCount
    );

    return {
        .enqueued = enqueued,
        .sent = sent_.load(std::memory_order_relaxed),
        .dropped = dropped,
        .send_errors = send_errors_.load(std::memory_order_relaxed),
        .queue_depth = aggregate_queue_depth(),
        // Sum of per-shard high-water marks. This is an upper bound on
        // simultaneous aggregate depth, but never exceeds total capacity.
        .max_queue_depth = max_queue_depth,
        .producer_shards_used = static_cast<std::uint64_t>(used),
        .producer_registration_failures =
            producer_registration_failures_.load(
                std::memory_order_relaxed
            )
    };
}

void MulticastPublisher::publisher_loop() noexcept {
    std::size_t cursor = 0;

    while (true) {
        bool consumed = false;

        for (
            std::size_t offset = 0;
            offset < kProducerShardCount;
            ++offset
        ) {
            const std::size_t shard_index =
                (cursor + offset) % kProducerShardCount;

            ProducerShard& shard = shards_[shard_index];

            if (
                shard.queue.try_consume(
                    [this](const Slot& slot) noexcept {
                        const std::span<const std::byte> datagram {
                            slot.bytes.data(),
                            slot.size
                        };

                        if (send_datagram(datagram)) {
                            sent_.fetch_add(
                                1,
                                std::memory_order_relaxed
                            );
                        } else {
                            send_errors_.fetch_add(
                                1,
                                std::memory_order_relaxed
                            );
                        }
                    }
                )
            ) {
                cursor = (shard_index + 1U) % kProducerShardCount;
                consumed = true;
                break;
            }
        }

        if (consumed) {
            continue;
        }

        if (!running_.load(std::memory_order_acquire)) {
            if (all_queues_empty() && !any_active_senders()) {
                break;
            }

            std::this_thread::yield();
            continue;
        }

        // Arm the sleep state, then rescan. A producer arriving after the
        // clear sets the flag and notifies; a producer arriving before the
        // rescan is observed directly. This prevents missed wake-ups.
        wake_requested_.clear(std::memory_order_release);

        if (!all_queues_empty()) {
            continue;
        }

        if (!running_.load(std::memory_order_acquire)) {
            continue;
        }

        wake_requested_.wait(false, std::memory_order_acquire);
    }
}

bool MulticastPublisher::send_datagram(
    std::span<const std::byte> datagram
) noexcept {
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

    return sent == static_cast<ssize_t>(datagram.size());
}

std::size_t
MulticastPublisher::producer_shard_for_current_thread() noexcept {
    const std::uint64_t generation = generation_.load(
        std::memory_order_acquire
    );

    if (
        producer_registration.publisher == this &&
        producer_registration.generation == generation &&
        producer_registration.shard < kProducerShardCount
    ) {
        return producer_registration.shard;
    }

    const std::size_t shard = next_producer_shard_.fetch_add(
        1,
        std::memory_order_relaxed
    );

    if (shard >= kProducerShardCount) {
        producer_registration_failures_.fetch_add(
            1,
            std::memory_order_relaxed
        );
        return kInvalidShard;
    }

    producer_registration = {
        .publisher = this,
        .generation = generation,
        .shard = shard
    };

    return shard;
}

bool MulticastPublisher::all_queues_empty() const noexcept {
    for (const ProducerShard& shard : shards_) {
        if (!shard.queue.empty_approx()) {
            return false;
        }
    }

    return true;
}

bool MulticastPublisher::any_active_senders() const noexcept {
    for (const ProducerShard& shard : shards_) {
        if (shard.active_sender.load(std::memory_order_acquire)) {
            return true;
        }
    }

    return false;
}

std::uint64_t
MulticastPublisher::aggregate_queue_depth() const noexcept {
    std::uint64_t depth = 0;

    for (const ProducerShard& shard : shards_) {
        depth += static_cast<std::uint64_t>(
            shard.queue.size_approx()
        );
    }

    return depth;
}

void MulticastPublisher::signal_consumer() noexcept {
    // During a burst the flag remains set, so producers only perform a
    // shared read. The first producer after the consumer arms its idle
    // state performs the test-and-set and wake-up.
    if (!wake_requested_.test(std::memory_order_relaxed)) {
        if (!wake_requested_.test_and_set(std::memory_order_release)) {
            wake_requested_.notify_one();
        }
    }
}

void MulticastPublisher::update_max_depth(
    ProducerShard& shard,
    std::size_t depth
) noexcept {
    const std::uint64_t candidate =
        static_cast<std::uint64_t>(depth);
    std::uint64_t observed = shard.max_depth.load(
        std::memory_order_relaxed
    );

    while (
        observed < candidate &&
        !shard.max_depth.compare_exchange_weak(
            observed,
            candidate,
            std::memory_order_relaxed,
            std::memory_order_relaxed
        )
    ) {
    }
}

}  // namespace exchange