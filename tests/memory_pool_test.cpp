#include <cstdint>
#include <list>
#include <memory_resource>

#include <gtest/gtest.h>

#include "exchange/memory_pool.hpp"
#include "exchange/order_book.hpp"

namespace {

exchange::Order make_limit_order(
    exchange::OrderId id,
    exchange::Side side,
    exchange::Price price,
    exchange::Quantity quantity
) {
    return {
        .id = id,
        .side = side,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = price,
        .initial_quantity = quantity,
        .remaining_quantity = quantity,
        .timestamp = id
    };
}

}  // namespace

TEST(MemoryPoolTest, ConstructionStatsAreInternallyConsistent) {
    exchange::MemoryPool pool;
    const auto stats = pool.stats();

    // The C++ standard does not require unsynchronized_pool_resource
    // to defer all upstream allocations until the first user request.
    // libc++ currently does; libstdc++ may allocate bookkeeping storage
    // during construction. Test portable invariants instead of a
    // standard-library implementation detail.
    EXPECT_NE(pool.resource(), nullptr);

    EXPECT_EQ(
        stats.upstream_allocations == 0U,
        stats.upstream_bytes_allocated == 0U
    );

    EXPECT_LE(
        stats.upstream_deallocations,
        stats.upstream_allocations
    );

    EXPECT_LE(
        stats.upstream_bytes_deallocated,
        stats.upstream_bytes_allocated
    );
}

TEST(MemoryPoolTest, ReusesFreedListNodes) {
    exchange::MemoryPool pool;
    std::pmr::list<std::uint64_t> values(pool.resource());

    for (std::uint64_t value = 0; value < 512; ++value) {
        values.push_back(value);
    }

    values.clear();
    const auto warmed = pool.stats();

    ASSERT_GT(warmed.upstream_allocations, 0U);

    for (std::uint64_t value = 0; value < 512; ++value) {
        values.push_back(value);
    }

    const auto reused = pool.stats();

    EXPECT_EQ(
        reused.upstream_allocations,
        warmed.upstream_allocations
    );
    EXPECT_EQ(
        reused.upstream_bytes_allocated,
        warmed.upstream_bytes_allocated
    );
}

TEST(MemoryPoolTest, OrderBookUsesPooledStorage) {
    exchange::OrderBook book;

    book.add_order(
        make_limit_order(
            1,
            exchange::Side::Buy,
            100,
            10
        )
    );

    const auto stats = book.memory_pool_stats();

    EXPECT_GT(stats.upstream_allocations, 0U);
    EXPECT_GT(stats.upstream_bytes_allocated, 0U);
    EXPECT_TRUE(book.contains(1));
}

TEST(MemoryPoolTest, OrderBookReusesStorageDuringChurn) {
    exchange::OrderBook book;
    constexpr std::uint64_t order_count = 512;

    for (std::uint64_t id = 1; id <= order_count; ++id) {
        book.add_order(
            make_limit_order(
                id,
                exchange::Side::Buy,
                100,
                1
            )
        );
    }

    for (std::uint64_t id = 1; id <= order_count; ++id) {
        ASSERT_TRUE(book.cancel_order(id));
    }

    const auto warmed = book.memory_pool_stats();

    for (
        std::uint64_t id = order_count + 1;
        id <= order_count * 2;
        ++id
    ) {
        book.add_order(
            make_limit_order(
                id,
                exchange::Side::Buy,
                100,
                1
            )
        );
    }

    const auto reused = book.memory_pool_stats();

    EXPECT_EQ(
        reused.upstream_allocations,
        warmed.upstream_allocations
    );
    EXPECT_EQ(
        reused.upstream_bytes_allocated,
        warmed.upstream_bytes_allocated
    );
    EXPECT_EQ(book.order_count(), order_count);
}

TEST(MemoryPoolTest, PooledPriceLevelPreservesFifoAfterMiddleCancel) {
    exchange::OrderBook book;

    book.add_order(
        make_limit_order(
            1,
            exchange::Side::Sell,
            101,
            10
        )
    );
    book.add_order(
        make_limit_order(
            2,
            exchange::Side::Sell,
            101,
            20
        )
    );
    book.add_order(
        make_limit_order(
            3,
            exchange::Side::Sell,
            101,
            30
        )
    );

    ASSERT_TRUE(book.cancel_order(2));

    EXPECT_EQ(book.best_ask_level().front().id, 1U);
    EXPECT_EQ(book.best_ask_level().total_quantity(), 40U);

    book.best_ask_level().fill_front(10);
    book.remove_from_index(1);

    EXPECT_EQ(book.best_ask_level().front().id, 3U);
}

TEST(MemoryPoolTest, SwapMovesPoolAndIndexedOrdersTogether) {
    exchange::OrderBook first;
    exchange::OrderBook second;

    first.add_order(
        make_limit_order(
            11,
            exchange::Side::Buy,
            99,
            7
        )
    );

    second.add_order(
        make_limit_order(
            22,
            exchange::Side::Sell,
            105,
            9
        )
    );

    const auto first_stats = first.memory_pool_stats();
    const auto second_stats = second.memory_pool_stats();

    first.swap(second);

    EXPECT_TRUE(first.contains(22));
    EXPECT_FALSE(first.contains(11));
    EXPECT_EQ(first.best_ask(), 105);

    EXPECT_TRUE(second.contains(11));
    EXPECT_FALSE(second.contains(22));
    EXPECT_EQ(second.best_bid(), 99);

    EXPECT_EQ(
        first.memory_pool_stats().upstream_allocations,
        second_stats.upstream_allocations
    );
    EXPECT_EQ(
        second.memory_pool_stats().upstream_allocations,
        first_stats.upstream_allocations
    );
}