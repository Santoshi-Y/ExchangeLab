#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <thread>

#include "exchange/multicast_publisher.hpp"

namespace {

using namespace std::chrono_literals;

TEST(
    MulticastPublisherTest,
    StartsAndStops
) {
    exchange::MulticastPublisher publisher(
        exchange::MulticastConfig {
            .group = "239.255.0.1",
            .port = 19100,
            .ttl = 1
        }
    );

    ASSERT_TRUE(publisher.start());
    EXPECT_TRUE(publisher.is_open());

    publisher.stop();

    EXPECT_FALSE(publisher.is_open());
}

TEST(
    MulticastPublisherTest,
    QueuesDatagramForAsyncSend
) {
    exchange::MulticastPublisher publisher(
        exchange::MulticastConfig {
            .group = "239.255.0.1",
            .port = 19101,
            .ttl = 1
        }
    );

    ASSERT_TRUE(publisher.start());

    const std::array<std::byte, 16>
        datagram {};

    EXPECT_TRUE(
        publisher.send(datagram)
    );

    for (
        int attempt = 0;
        attempt < 100;
        ++attempt
    ) {
        const auto stats =
            publisher.stats();

        if (stats.sent >= 1U) {
            break;
        }

        std::this_thread::sleep_for(
            1ms
        );
    }

    const auto stats =
        publisher.stats();

    EXPECT_EQ(stats.enqueued, 1U);
    EXPECT_EQ(stats.sent, 1U);
    EXPECT_EQ(stats.dropped, 0U);
    EXPECT_EQ(stats.send_errors, 0U);

    publisher.stop();
}

TEST(
    MulticastPublisherTest,
    RejectsOversizedDatagram
) {
    exchange::MulticastPublisher publisher(
        exchange::MulticastConfig {
            .group = "239.255.0.1",
            .port = 19102,
            .ttl = 1
        }
    );

    ASSERT_TRUE(publisher.start());

    std::array<
        std::byte,
        exchange::MulticastPublisher::
            maximum_datagram_size() + 1U
    > datagram {};

    EXPECT_FALSE(
        publisher.send(datagram)
    );

    const auto stats =
        publisher.stats();

    EXPECT_EQ(stats.enqueued, 0U);
    EXPECT_EQ(stats.dropped, 1U);

    publisher.stop();
}

}  // namespace