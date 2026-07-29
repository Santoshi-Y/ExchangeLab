#include <stdexcept>

#include <gtest/gtest.h>

#include "exchange/matching_engine.hpp"

namespace {

exchange::Order make_order(
    exchange::OrderId id,
    exchange::Side side,
    exchange::Price price,
    exchange::Quantity quantity,
    exchange::Timestamp timestamp,
    exchange::OrderType type =
        exchange::OrderType::Limit
) {
    return {
        .id = id,
        .side = side,
        .type = type,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = price,
        .initial_quantity = quantity,
        .remaining_quantity = quantity,
        .timestamp = timestamp
    };
}

}  // namespace

TEST(OrderReplaceTest, UnknownOrderReturnsFalse) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    const auto result = engine.replace_order(
        book,
        999,
        105,
        20,
        10
    );

    EXPECT_FALSE(result.replaced);
    EXPECT_TRUE(result.trades.empty());
    EXPECT_TRUE(book.empty());
}

TEST(OrderReplaceTest, ReplacesPriceAndQuantity) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Buy,
            100,
            10,
            1
        )
    );

    const auto result = engine.replace_order(
        book,
        1,
        102,
        25,
        2
    );

    ASSERT_TRUE(result.replaced);
    EXPECT_TRUE(result.trades.empty());

    ASSERT_TRUE(book.contains(1));
    ASSERT_TRUE(book.has_bids());

    EXPECT_EQ(book.best_bid(), 102);
    EXPECT_EQ(book.order_count(), 1U);

    const exchange::Order* replaced =
        book.find_order(1);

    ASSERT_NE(replaced, nullptr);
    EXPECT_EQ(replaced->id, 1);
    EXPECT_EQ(replaced->side, exchange::Side::Buy);
    EXPECT_EQ(replaced->price, 102);
    EXPECT_EQ(replaced->initial_quantity, 25);
    EXPECT_EQ(replaced->remaining_quantity, 25);
    EXPECT_EQ(replaced->timestamp, 2);
}

TEST(OrderReplaceTest, ReplacementLosesFifoPriority) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Sell,
            101,
            10,
            1
        )
    );

    engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Sell,
            101,
            20,
            2
        )
    );

    const auto result = engine.replace_order(
        book,
        1,
        101,
        10,
        3
    );

    ASSERT_TRUE(result.replaced);
    EXPECT_TRUE(result.trades.empty());

    ASSERT_TRUE(book.has_asks());

    // Order 1 was cancelled and reinserted, so order 2
    // now has priority at the same price.
    EXPECT_EQ(
        book.best_ask_level().front().id,
        2
    );

    EXPECT_EQ(
        book.best_ask_level().order_count(),
        2U
    );

    EXPECT_EQ(
        book.best_ask_level().total_quantity(),
        30
    );
}

TEST(OrderReplaceTest, AggressiveReplacementExecutesImmediately) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Buy,
            100,
            20,
            1
        )
    );

    engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Sell,
            105,
            15,
            2
        )
    );

    const auto result = engine.replace_order(
        book,
        1,
        105,
        20,
        3
    );

    ASSERT_TRUE(result.replaced);
    ASSERT_EQ(result.trades.size(), 1U);

    EXPECT_EQ(result.trades[0].buy_order_id, 1);
    EXPECT_EQ(result.trades[0].sell_order_id, 2);
    EXPECT_EQ(result.trades[0].price, 105);
    EXPECT_EQ(result.trades[0].quantity, 15);

    EXPECT_FALSE(book.contains(2));

    // Five units from the replaced buy order remain.
    ASSERT_TRUE(book.contains(1));
    ASSERT_TRUE(book.has_bids());

    EXPECT_EQ(book.best_bid(), 105);

    const exchange::Order* remaining =
        book.find_order(1);

    ASSERT_NE(remaining, nullptr);
    EXPECT_EQ(remaining->remaining_quantity, 5);
    EXPECT_EQ(remaining->initial_quantity, 20);
    EXPECT_EQ(remaining->timestamp, 3);
}

TEST(OrderReplaceTest, FullyExecutedReplacementDoesNotRest) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Buy,
            100,
            10,
            1
        )
    );

    engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Sell,
            105,
            10,
            2
        )
    );

    const auto result = engine.replace_order(
        book,
        1,
        105,
        10,
        3
    );

    ASSERT_TRUE(result.replaced);
    ASSERT_EQ(result.trades.size(), 1U);

    EXPECT_EQ(result.trades[0].quantity, 10);
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
    EXPECT_TRUE(book.empty());
}

TEST(OrderReplaceTest, PreservesOriginalSide) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Sell,
            110,
            20,
            1
        )
    );

    const auto result = engine.replace_order(
        book,
        1,
        108,
        30,
        2
    );

    ASSERT_TRUE(result.replaced);
    EXPECT_TRUE(result.trades.empty());

    const exchange::Order* replaced =
        book.find_order(1);

    ASSERT_NE(replaced, nullptr);
    EXPECT_EQ(replaced->side, exchange::Side::Sell);
    EXPECT_EQ(replaced->price, 108);
    EXPECT_EQ(replaced->remaining_quantity, 30);

    EXPECT_FALSE(book.has_bids());
    EXPECT_TRUE(book.has_asks());
    EXPECT_EQ(book.best_ask(), 108);
}

TEST(OrderReplaceTest, RejectsNonPositiveQuantity) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Buy,
            100,
            10,
            1
        )
    );

    EXPECT_THROW(
        engine.replace_order(
            book,
            1,
            101,
            0,
            2
        ),
        std::invalid_argument
    );

    // Validation occurs before cancellation, so the
    // original order remains unchanged.
    ASSERT_TRUE(book.contains(1));

    const exchange::Order* original =
        book.find_order(1);

    ASSERT_NE(original, nullptr);
    EXPECT_EQ(original->price, 100);
    EXPECT_EQ(original->remaining_quantity, 10);
}