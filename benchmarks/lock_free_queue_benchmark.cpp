#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "exchange/lock_free_ring.hpp"

namespace {

constexpr std::size_t total_queue_capacity = 65'536;
constexpr std::size_t items_per_producer = 500'000;

class MutexQueue {
public:
    [[nodiscard]] bool try_push(std::uint64_t value) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (count_ == total_queue_capacity) {
            return false;
        }

        values_[write_index_] = value;
        write_index_ = (write_index_ + 1U) % total_queue_capacity;
        ++count_;
        return true;
    }

    [[nodiscard]] bool try_pop(std::uint64_t& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (count_ == 0) {
            return false;
        }

        value = values_[read_index_];
        read_index_ = (read_index_ + 1U) % total_queue_capacity;
        --count_;
        return true;
    }

private:
    std::array<std::uint64_t, total_queue_capacity> values_ {};
    std::size_t read_index_ {0};
    std::size_t write_index_ {0};
    std::size_t count_ {0};
    std::mutex mutex_;
};

template <std::size_t ProducerCount>
double run_sharded_spsc_case() {
    static_assert(
        total_queue_capacity % ProducerCount == 0,
        "Total queue capacity must divide evenly across producer shards"
    );

    constexpr std::size_t capacity_per_shard =
        total_queue_capacity / ProducerCount;

    using Queue = exchange::SpscRing<
        std::uint64_t,
        capacity_per_shard
    >;

    std::array<Queue, ProducerCount> queues;
    const std::size_t total_items =
        ProducerCount * items_per_producer;

    std::atomic<bool> start {false};
    std::atomic<std::uint64_t> checksum {0};
    std::vector<std::thread> producers;
    producers.reserve(ProducerCount);

    for (std::size_t producer = 0; producer < ProducerCount; ++producer) {
        producers.emplace_back(
            [producer, &queues, &start]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                const std::uint64_t base =
                    static_cast<std::uint64_t>(
                        producer * items_per_producer
                    );

                for (std::size_t index = 0; index < items_per_producer; ++index) {
                    const std::uint64_t value =
                        base + static_cast<std::uint64_t>(index);

                    while (!queues[producer].try_push(value)) {
                        std::this_thread::yield();
                    }
                }
            }
        );
    }

    std::thread consumer(
        [&queues, &start, &checksum, total_items]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::uint64_t local_checksum = 0;
            std::size_t consumed = 0;
            std::size_t cursor = 0;

            while (consumed < total_items) {
                bool made_progress = false;

                for (std::size_t offset = 0; offset < ProducerCount; ++offset) {
                    const std::size_t shard =
                        (cursor + offset) % ProducerCount;
                    std::uint64_t value = 0;

                    if (queues[shard].try_pop(value)) {
                        local_checksum += value;
                        ++consumed;
                        cursor = (shard + 1U) % ProducerCount;
                        made_progress = true;
                        break;
                    }
                }

                if (!made_progress) {
                    std::this_thread::yield();
                }
            }

            checksum.store(local_checksum, std::memory_order_relaxed);
        }
    );

    const auto started = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();

    const auto finished = std::chrono::steady_clock::now();

    if (checksum.load(std::memory_order_relaxed) == 0) {
        std::cerr << "Unexpected zero checksum\n";
    }

    const double seconds =
        std::chrono::duration<double>(finished - started).count();

    return static_cast<double>(total_items) / seconds;
}

double run_mutex_case(std::size_t producer_count) {
    MutexQueue queue;
    const std::size_t total_items =
        producer_count * items_per_producer;

    std::atomic<bool> start {false};
    std::atomic<std::uint64_t> checksum {0};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);

    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back(
            [producer, &queue, &start]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                const std::uint64_t base =
                    static_cast<std::uint64_t>(
                        producer * items_per_producer
                    );

                for (std::size_t index = 0; index < items_per_producer; ++index) {
                    const std::uint64_t value =
                        base + static_cast<std::uint64_t>(index);

                    while (!queue.try_push(value)) {
                        std::this_thread::yield();
                    }
                }
            }
        );
    }

    std::thread consumer(
        [&queue, &start, &checksum, total_items]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::uint64_t local_checksum = 0;
            std::size_t consumed = 0;

            while (consumed < total_items) {
                std::uint64_t value = 0;

                if (!queue.try_pop(value)) {
                    std::this_thread::yield();
                    continue;
                }

                local_checksum += value;
                ++consumed;
            }

            checksum.store(local_checksum, std::memory_order_relaxed);
        }
    );

    const auto started = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();

    const auto finished = std::chrono::steady_clock::now();

    if (checksum.load(std::memory_order_relaxed) == 0) {
        std::cerr << "Unexpected zero checksum\n";
    }

    const double seconds =
        std::chrono::duration<double>(finished - started).count();

    return static_cast<double>(total_items) / seconds;
}

template <std::size_t ProducerCount>
void print_case(const char* label) {
    const double spsc_rate = run_sharded_spsc_case<ProducerCount>();
    const double mutex_rate = run_mutex_case(ProducerCount);

    std::cout
        << label << '\n'
        << "  Sharded SPSC: "
        << std::fixed << std::setprecision(2)
        << spsc_rate
        << " messages/sec\n"
        << "  Mutex queue:  "
        << mutex_rate
        << " messages/sec\n"
        << "  Ratio:        "
        << (spsc_rate / mutex_rate)
        << "x\n\n";
}

}  // namespace

int main() {
    std::cout
        << "ExchangeLab Sharded SPSC Queue Benchmark\n"
        << "========================================\n"
        << "Items per producer: "
        << items_per_producer
        << "\nTotal queue capacity: "
        << total_queue_capacity
        << "\nIndex atomics always lock-free: "
        << (
            exchange::SpscRing<
                std::uint64_t,
                total_queue_capacity
            >::index_atomics_always_lock_free()
                ? "yes"
                : "no"
        )
        << "\n\n";

    print_case<1>("1 producer / 1 consumer");
    print_case<4>("4 producers / 1 consumer");

    return 0;
}