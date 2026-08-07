#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "exchange/lock_free_ring.hpp"

namespace {

TEST(SpscRingTest, StartsEmpty) {
    exchange::SpscRing<int, 8> queue;

    EXPECT_TRUE(queue.empty_approx());
    EXPECT_EQ(queue.size_approx(), 0U);
    EXPECT_EQ(queue.capacity(), 8U);
}

TEST(SpscRingTest, PushesAndPopsInFifoOrder) {
    exchange::SpscRing<int, 8> queue;

    ASSERT_TRUE(queue.try_push(10));
    ASSERT_TRUE(queue.try_push(20));
    ASSERT_TRUE(queue.try_push(30));

    int value = 0;

    ASSERT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 10);
    ASSERT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 20);
    ASSERT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, 30);
    EXPECT_FALSE(queue.try_pop(value));
}

TEST(SpscRingTest, RejectsPushWhenFull) {
    exchange::SpscRing<int, 4> queue;

    EXPECT_TRUE(queue.try_push(1));
    EXPECT_TRUE(queue.try_push(2));
    EXPECT_TRUE(queue.try_push(3));
    EXPECT_TRUE(queue.try_push(4));
    EXPECT_FALSE(queue.try_push(5));
    EXPECT_EQ(queue.size_approx(), 4U);
}

TEST(SpscRingTest, ReusesSlotsAfterWraparound) {
    exchange::SpscRing<int, 4> queue;

    for (int cycle = 0; cycle < 100; ++cycle) {
        for (int offset = 0; offset < 4; ++offset) {
            ASSERT_TRUE(queue.try_push(cycle * 4 + offset));
        }

        for (int offset = 0; offset < 4; ++offset) {
            int value = -1;
            ASSERT_TRUE(queue.try_pop(value));
            EXPECT_EQ(value, cycle * 4 + offset);
        }
    }

    EXPECT_TRUE(queue.empty_approx());
}

TEST(SpscRingTest, TransfersAllItemsBetweenProducerAndConsumer) {
    constexpr std::size_t item_count = 250'000;

    exchange::SpscRing<std::uint64_t, 4096> queue;
    std::atomic<bool> start {false};
    std::atomic<std::uint64_t> checksum {0};

    std::thread producer(
        [&queue, &start]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (std::size_t index = 0; index < item_count; ++index) {
                const auto value = static_cast<std::uint64_t>(index + 1U);
                while (!queue.try_push(value)) {
                    std::this_thread::yield();
                }
            }
        }
    );

    std::thread consumer(
        [&queue, &start, &checksum]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::uint64_t local_checksum = 0;
            std::size_t consumed = 0;

            while (consumed < item_count) {
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

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    const std::uint64_t expected =
        (static_cast<std::uint64_t>(item_count) *
         static_cast<std::uint64_t>(item_count + 1U)) /
        2U;

    EXPECT_EQ(checksum.load(std::memory_order_relaxed), expected);
    EXPECT_TRUE(queue.empty_approx());
}

}  // namespace