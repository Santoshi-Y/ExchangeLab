#include <gtest/gtest.h>

#include "exchange/price_level.hpp"

namespace {

exchange::Order make_sell_order(
    exchange::OrderId id,
    exchange::Price price,
    exchange::Quantity quantity,
    exchange::Timestamp timestamp
) {
    return {
        .id = id,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = price,
        .initial_quantity = quantity,
        .remaining_quantity = quantity,
        .timestamp = timestamp
    };
}

}  // namespace

TEST(PriceLevelTest, StartsEmpty) {
    const exchange::PriceLevel level{101};

    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.price(), 101);
    EXPECT_EQ(level.order_count(), 0U);
    EXPECT_EQ(level.total_quantity(), 0);
}

TEST(PriceLevelTest, AddsOrderAndTracksQuantity) {
    exchange::PriceLevel level{101};

    level.add_order(make_sell_order(1, 101, 25, 1));

    EXPECT_FALSE(level.empty());
    EXPECT_EQ(level.order_count(), 1U);
    EXPECT_EQ(level.total_quantity(), 25);
    EXPECT_EQ(level.front().id, 1);
    EXPECT_EQ(level.front().remaining_quantity, 25);
}

TEST(PriceLevelTest, AddsMultipleOrdersInFifoOrder) {
    exchange::PriceLevel level{101};

    level.add_order(make_sell_order(1, 101, 10, 1));
    level.add_order(make_sell_order(2, 101, 20, 2));
    level.add_order(make_sell_order(3, 101, 30, 3));

    EXPECT_EQ(level.order_count(), 3U);
    EXPECT_EQ(level.total_quantity(), 60);
    EXPECT_EQ(level.front().id, 1);
}

TEST(PriceLevelTest, PartiallyFillsFrontOrder) {
    exchange::PriceLevel level{101};

    level.add_order(make_sell_order(1, 101, 20, 1));

    level.fill_front(7);

    EXPECT_FALSE(level.empty());
    EXPECT_EQ(level.order_count(), 1U);
    EXPECT_EQ(level.total_quantity(), 13);
    EXPECT_EQ(level.front().id, 1);
    EXPECT_EQ(level.front().remaining_quantity, 13);
}

TEST(PriceLevelTest, RemovesCompletelyFilledOrder) {
    exchange::PriceLevel level{101};

    level.add_order(make_sell_order(1, 101, 20, 1));

    level.fill_front(20);

    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.order_count(), 0U);
    EXPECT_EQ(level.total_quantity(), 0);
}

TEST(PriceLevelTest, AdvancesToNextOrderAfterFill) {
    exchange::PriceLevel level{101};

    level.add_order(make_sell_order(1, 101, 10, 1));
    level.add_order(make_sell_order(2, 101, 20, 2));

    level.fill_front(10);

    ASSERT_FALSE(level.empty());
    EXPECT_EQ(level.order_count(), 1U);
    EXPECT_EQ(level.total_quantity(), 20);
    EXPECT_EQ(level.front().id, 2);
}