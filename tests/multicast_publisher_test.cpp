#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "exchange/multicast_publisher.hpp"

namespace {

using namespace std::chrono_literals;

TEST(MulticastPublisherTest, StartsAndStops) {
    exchange::MulticastPublisher publisher(
        exchange::MulticastConfig {
            .group = "127.0.0.1",
            .port = 19100,
            .ttl = 1
        }
    );

    ASSERT_TRUE(publisher.start());
    EXPECT_TRUE(publisher.is_open());

    publisher.stop();
    EXPECT_FALSE(publisher.is_open());
}

TEST(MulticastPublisherTest, QueuesDatagramForAsyncSend) {
    exchange::MulticastPublisher publisher(
        exchange::MulticastConfig {
            .group = "127.0.0.1",
            .port = 19101,
            .ttl = 1
        }
    );

    ASSERT_TRUE(publisher.start());

    const std::array<std::byte, 16> datagram {};
    EXPECT_TRUE(publisher.send(datagram));

    for (int attempt = 0; attempt < 100; ++attempt) {
        if (publisher.stats().sent >= 1U) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    const auto stats = publisher.stats();

    EXPECT_EQ(stats.enqueued, 1U);
    EXPECT_EQ(stats.sent, 1U);
    EXPECT_EQ(stats.dropped, 0U);
    EXPECT_EQ(stats.send_errors, 0U);
    EXPECT_EQ(stats.producer_shards_used, 1U);
    EXPECT_EQ(stats.producer_registration_failures, 0U);
    EXPECT_LE(
        stats.max_queue_depth,
        exchange::MulticastPublisher::queue_capacity()
    );

    publisher.stop();
}

TEST(MulticastPublisherTest, RejectsOversizedDatagram) {
    exchange::MulticastPublisher publisher(
        exchange::MulticastConfig {
            .group = "127.0.0.1",
            .port = 19102,
            .ttl = 1
        }
    );

    ASSERT_TRUE(publisher.start());

    std::array<
        std::byte,
        exchange::MulticastPublisher::maximum_datagram_size() + 1U
    > datagram {};

    EXPECT_FALSE(publisher.send(datagram));

    const auto stats = publisher.stats();
    EXPECT_EQ(stats.enqueued, 0U);
    EXPECT_EQ(stats.dropped, 1U);

    publisher.stop();
}

TEST(MulticastPublisherTest, AcceptsConcurrentProducerCalls) {
    exchange::MulticastPublisher publisher(
        exchange::MulticastConfig {
            .group = "127.0.0.1",
            .port = 19103,
            .ttl = 1
        }
    );

    ASSERT_TRUE(publisher.start());

    constexpr std::size_t producer_count = 4;
    constexpr std::size_t sends_per_producer = 16;

    std::array<std::thread, producer_count> producers;
    std::atomic<std::uint64_t> send_failures {0};

    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers[producer] = std::thread(
            [&publisher, &send_failures]() {
                const std::array<std::byte, 32> datagram {};

                for (
                    std::size_t index = 0;
                    index < sends_per_producer;
                    ++index
                ) {
                    if (!publisher.send(datagram)) {
                        send_failures.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    }
                }
            }
        );
    }

    for (auto& producer : producers) {
        producer.join();
    }

    publisher.stop();

    const auto stats = publisher.stats();
    const std::uint64_t expected =
        producer_count * sends_per_producer;

    EXPECT_EQ(send_failures.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(stats.enqueued, expected);
    EXPECT_EQ(stats.sent, expected);
    EXPECT_EQ(stats.dropped, 0U);
    EXPECT_EQ(stats.send_errors, 0U);
    EXPECT_EQ(stats.producer_shards_used, producer_count);
    EXPECT_EQ(stats.producer_registration_failures, 0U);
}

}  // namespace
