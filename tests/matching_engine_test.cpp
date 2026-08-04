#include <gtest/gtest.h>

#include "exchange/matching_engine.hpp"

namespace {

exchange::Order make_order(
    exchange::OrderId id,
    exchange::Side side,
    exchange::Price price,
    exchange::Quantity quantity,
    exchange::Timestamp timestamp,
    exchange::OrderType type = exchange::OrderType::Limit,
    exchange::TimeInForce time_in_force =
        exchange::TimeInForce::GoodTillCancel
) {
    return {
        .id = id,
        .side = side,
        .type = type,
        .time_in_force = time_in_force,
        .price = price,
        .initial_quantity = quantity,
        .remaining_quantity = quantity,
        .timestamp = timestamp
    };
}

}  // namespace

TEST(MatchingEngineTest, FullyFillsIncomingBuy) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            101,
            40,
            1
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            101,
            40,
            2
        )
    );

    ASSERT_EQ(trades.size(), 1U);

    EXPECT_EQ(trades[0].buy_order_id, 2);
    EXPECT_EQ(trades[0].sell_order_id, 1);
    EXPECT_EQ(trades[0].price, 101);
    EXPECT_EQ(trades[0].quantity, 40);

    EXPECT_FALSE(book.has_bids());
    EXPECT_FALSE(book.has_asks());
}

TEST(MatchingEngineTest, PartiallyFillsRestingSell) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            101,
            40,
            1
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            105,
            25,
            2
        )
    );

    ASSERT_EQ(trades.size(), 1U);

    EXPECT_EQ(trades[0].price, 101);
    EXPECT_EQ(trades[0].quantity, 25);

    ASSERT_TRUE(book.has_asks());
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(
        book.best_ask_level().total_quantity(),
        15
    );
    EXPECT_EQ(
        book.best_ask_level().front().remaining_quantity,
        15
    );

    EXPECT_FALSE(book.has_bids());
}

TEST(MatchingEngineTest, RestsUnfilledIncomingBuy) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            101,
            20,
            1
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            101,
            50,
            2
        )
    );

    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0].quantity, 20);

    EXPECT_FALSE(book.has_asks());

    ASSERT_TRUE(book.has_bids());
    EXPECT_EQ(book.best_bid(), 101);
    EXPECT_EQ(
        book.best_bid_level().total_quantity(),
        30
    );
    EXPECT_EQ(book.best_bid_level().front().id, 2);
    EXPECT_EQ(
        book.best_bid_level().front().remaining_quantity,
        30
    );
}

TEST(MatchingEngineTest, MatchesBuyAcrossMultipleLevels) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            101,
            20,
            1
        )
    );

    book.add_order(
        make_order(
            2,
            exchange::Side::Sell,
            102,
            30,
            2
        )
    );

    book.add_order(
        make_order(
            3,
            exchange::Side::Sell,
            103,
            50,
            3
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            10,
            exchange::Side::Buy,
            103,
            60,
            10
        )
    );

    ASSERT_EQ(trades.size(), 3U);

    EXPECT_EQ(trades[0].sell_order_id, 1);
    EXPECT_EQ(trades[0].price, 101);
    EXPECT_EQ(trades[0].quantity, 20);

    EXPECT_EQ(trades[1].sell_order_id, 2);
    EXPECT_EQ(trades[1].price, 102);
    EXPECT_EQ(trades[1].quantity, 30);

    EXPECT_EQ(trades[2].sell_order_id, 3);
    EXPECT_EQ(trades[2].price, 103);
    EXPECT_EQ(trades[2].quantity, 10);

    ASSERT_TRUE(book.has_asks());
    EXPECT_EQ(book.best_ask(), 103);
    EXPECT_EQ(
        book.best_ask_level().total_quantity(),
        40
    );

    EXPECT_FALSE(book.has_bids());
}

TEST(MatchingEngineTest, PreservesFifoAtSamePrice) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            101,
            10,
            1
        )
    );

    book.add_order(
        make_order(
            2,
            exchange::Side::Sell,
            101,
            20,
            2
        )
    );

    book.add_order(
        make_order(
            3,
            exchange::Side::Sell,
            101,
            30,
            3
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            10,
            exchange::Side::Buy,
            101,
            35,
            10
        )
    );

    ASSERT_EQ(trades.size(), 3U);

    EXPECT_EQ(trades[0].sell_order_id, 1);
    EXPECT_EQ(trades[0].quantity, 10);

    EXPECT_EQ(trades[1].sell_order_id, 2);
    EXPECT_EQ(trades[1].quantity, 20);

    EXPECT_EQ(trades[2].sell_order_id, 3);
    EXPECT_EQ(trades[2].quantity, 5);

    ASSERT_TRUE(book.has_asks());
    EXPECT_EQ(
        book.best_ask_level().order_count(),
        1U
    );
    EXPECT_EQ(book.best_ask_level().front().id, 3);
    EXPECT_EQ(
        book.best_ask_level().front().remaining_quantity,
        25
    );
}

TEST(MatchingEngineTest, MatchesIncomingSellAgainstBestBid) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Buy,
            103,
            20,
            1
        )
    );

    book.add_order(
        make_order(
            2,
            exchange::Side::Buy,
            102,
            30,
            2
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            10,
            exchange::Side::Sell,
            102,
            25,
            10
        )
    );

    ASSERT_EQ(trades.size(), 2U);

    EXPECT_EQ(trades[0].buy_order_id, 1);
    EXPECT_EQ(trades[0].sell_order_id, 10);
    EXPECT_EQ(trades[0].price, 103);
    EXPECT_EQ(trades[0].quantity, 20);

    EXPECT_EQ(trades[1].buy_order_id, 2);
    EXPECT_EQ(trades[1].sell_order_id, 10);
    EXPECT_EQ(trades[1].price, 102);
    EXPECT_EQ(trades[1].quantity, 5);

    ASSERT_TRUE(book.has_bids());
    EXPECT_EQ(book.best_bid(), 102);
    EXPECT_EQ(
        book.best_bid_level().total_quantity(),
        25
    );

    EXPECT_FALSE(book.has_asks());
}

TEST(MatchingEngineTest, DoesNotMatchNonCrossingBuy) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            105,
            20,
            1
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            104,
            15,
            2
        )
    );

    EXPECT_TRUE(trades.empty());

    ASSERT_TRUE(book.has_bids());
    ASSERT_TRUE(book.has_asks());

    EXPECT_EQ(book.best_bid(), 104);
    EXPECT_EQ(book.best_ask(), 105);
    EXPECT_EQ(
        book.best_bid_level().total_quantity(),
        15
    );
    EXPECT_EQ(
        book.best_ask_level().total_quantity(),
        20
    );
}

TEST(MatchingEngineTest, DoesNotMatchNonCrossingSell) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Buy,
            100,
            20,
            1
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Sell,
            101,
            15,
            2
        )
    );

    EXPECT_TRUE(trades.empty());

    ASSERT_TRUE(book.has_bids());
    ASSERT_TRUE(book.has_asks());

    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(
        book.best_bid_level().total_quantity(),
        20
    );
    EXPECT_EQ(
        book.best_ask_level().total_quantity(),
        15
    );
}

TEST(MatchingEngineTest, UsesRestingOrderPrice) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            101,
            10,
            1
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            110,
            10,
            2
        )
    );

    ASSERT_EQ(trades.size(), 1U);

    // Execution occurs at the resting order's price,
    // not at the incoming buy's limit price.
    EXPECT_EQ(trades[0].price, 101);
}

TEST(MatchingEngineTest, EmptyBookCausesOrderToRest) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    const auto trades = engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Buy,
            100,
            25,
            1
        )
    );

    EXPECT_TRUE(trades.empty());

    ASSERT_TRUE(book.has_bids());
    EXPECT_FALSE(book.has_asks());

    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(
        book.best_bid_level().total_quantity(),
        25
    );
}

TEST(MatchingEngineTest, MarketBuyIgnoresLimitPrice) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            105,
            20,
            1
        )
    );

    // The price field is deliberately below the best ask.
    // A limit buy at 100 would not match an ask at 105.
    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            100,
            20,
            2,
            exchange::OrderType::Market
        )
    );

    ASSERT_EQ(trades.size(), 1U);

    EXPECT_EQ(trades[0].buy_order_id, 2);
    EXPECT_EQ(trades[0].sell_order_id, 1);
    EXPECT_EQ(trades[0].price, 105);
    EXPECT_EQ(trades[0].quantity, 20);

    EXPECT_FALSE(book.has_bids());
    EXPECT_FALSE(book.has_asks());
}

TEST(MatchingEngineTest, MarketSellIgnoresLimitPrice) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Buy,
            95,
            20,
            1
        )
    );

    // The price field is deliberately above the best bid.
    // A limit sell at 100 would not match a bid at 95.
    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Sell,
            100,
            20,
            2,
            exchange::OrderType::Market
        )
    );

    ASSERT_EQ(trades.size(), 1U);

    EXPECT_EQ(trades[0].buy_order_id, 1);
    EXPECT_EQ(trades[0].sell_order_id, 2);
    EXPECT_EQ(trades[0].price, 95);
    EXPECT_EQ(trades[0].quantity, 20);

    EXPECT_FALSE(book.has_bids());
    EXPECT_FALSE(book.has_asks());
}

TEST(MatchingEngineTest, MarketBuyConsumesMultipleAskLevels) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            101,
            10,
            1
        )
    );

    book.add_order(
        make_order(
            2,
            exchange::Side::Sell,
            102,
            20,
            2
        )
    );

    book.add_order(
        make_order(
            3,
            exchange::Side::Sell,
            104,
            30,
            3
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            10,
            exchange::Side::Buy,
            0,
            45,
            10,
            exchange::OrderType::Market
        )
    );

    ASSERT_EQ(trades.size(), 3U);

    EXPECT_EQ(trades[0].sell_order_id, 1);
    EXPECT_EQ(trades[0].price, 101);
    EXPECT_EQ(trades[0].quantity, 10);

    EXPECT_EQ(trades[1].sell_order_id, 2);
    EXPECT_EQ(trades[1].price, 102);
    EXPECT_EQ(trades[1].quantity, 20);

    EXPECT_EQ(trades[2].sell_order_id, 3);
    EXPECT_EQ(trades[2].price, 104);
    EXPECT_EQ(trades[2].quantity, 15);

    ASSERT_TRUE(book.has_asks());
    EXPECT_EQ(book.best_ask(), 104);
    EXPECT_EQ(
        book.best_ask_level().total_quantity(),
        15
    );

    EXPECT_FALSE(book.has_bids());
}

TEST(MatchingEngineTest, UnfilledMarketBuyDoesNotRest) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            101,
            10,
            1
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            0,
            25,
            2,
            exchange::OrderType::Market
        )
    );

    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0].quantity, 10);

    // The remaining 15 units are discarded rather than
    // placed on the bid side.
    EXPECT_FALSE(book.has_bids());
    EXPECT_FALSE(book.has_asks());
}

TEST(MatchingEngineTest, UnfilledMarketSellDoesNotRest) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order(
        make_order(
            1,
            exchange::Side::Buy,
            100,
            10,
            1
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Sell,
            0,
            25,
            2,
            exchange::OrderType::Market
        )
    );

    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0].quantity, 10);

    EXPECT_FALSE(book.has_bids());
    EXPECT_FALSE(book.has_asks());
}

TEST(MatchingEngineTest, MarketOrderOnEmptyBookDoesNotRest) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    const auto trades = engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Buy,
            0,
            25,
            1,
            exchange::OrderType::Market
        )
    );

    EXPECT_TRUE(trades.empty());
    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.has_bids());
    EXPECT_FALSE(book.has_asks());
}

TEST(MatchingEngineTest, CancelsRestingOrderById) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Buy,
            100,
            25,
            1
        )
    );

    ASSERT_TRUE(book.contains(1));

    EXPECT_TRUE(engine.cancel_order(book, 1));

    EXPECT_FALSE(book.contains(1));
    EXPECT_TRUE(book.empty());
}

TEST(MatchingEngineTest, FullyFilledOrderLeavesIndex) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Sell,
            101,
            20,
            1
        )
    );

    ASSERT_TRUE(book.contains(1));

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            101,
            20,
            2
        )
    );

    ASSERT_EQ(trades.size(), 1U);

    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_TRUE(book.empty());
}

TEST(MatchingEngineTest, PartialFillKeepsOrderIndexed) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    engine.process_order(
        book,
        make_order(
            1,
            exchange::Side::Sell,
            101,
            30,
            1
        )
    );

    engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            101,
            10,
            2
        )
    );

    EXPECT_TRUE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
    EXPECT_EQ(book.order_count(), 1U);

    ASSERT_TRUE(book.has_asks());
    EXPECT_EQ(
        book.best_ask_level().front()
            .remaining_quantity,
        20
    );

    EXPECT_TRUE(engine.cancel_order(book, 1));
    EXPECT_TRUE(book.empty());
}

TEST(MatchingEngineTest, MarketSweepUpdatesOrderIndex) {
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
            102,
            20,
            2
        )
    );

    engine.process_order(
        book,
        make_order(
            3,
            exchange::Side::Sell,
            103,
            30,
            3
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            10,
            exchange::Side::Buy,
            0,
            35,
            10,
            exchange::OrderType::Market
        )
    );

    ASSERT_EQ(trades.size(), 3U);

    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
    EXPECT_TRUE(book.contains(3));
    EXPECT_FALSE(book.contains(10));

    EXPECT_EQ(book.order_count(), 1U);

    ASSERT_TRUE(book.has_asks());
    EXPECT_EQ(book.best_ask(), 103);
    EXPECT_EQ(
        book.best_ask_level().front()
            .remaining_quantity,
        25
    );
}


TEST(MatchingEngineTest, ImmediateOrCancelDoesNotRestRemainder) {
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

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            101,
            25,
            2,
            exchange::OrderType::Limit,
            exchange::TimeInForce::ImmediateOrCancel
        )
    );

    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades[0].quantity, 10);
    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.contains(2));
}

TEST(MatchingEngineTest, FillOrKillRejectsWithoutChangingBook) {
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

    const auto trades = engine.process_order(
        book,
        make_order(
            2,
            exchange::Side::Buy,
            101,
            25,
            2,
            exchange::OrderType::Limit,
            exchange::TimeInForce::FillOrKill
        )
    );

    EXPECT_TRUE(trades.empty());
    ASSERT_TRUE(book.has_asks());
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.best_ask_level().total_quantity(), 10);
    EXPECT_TRUE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
}

TEST(MatchingEngineTest, FillOrKillFillsAcrossMultipleLevels) {
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
            102,
            15,
            2
        )
    );

    const auto trades = engine.process_order(
        book,
        make_order(
            3,
            exchange::Side::Buy,
            102,
            25,
            3,
            exchange::OrderType::Limit,
            exchange::TimeInForce::FillOrKill
        )
    );

    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0].quantity, 10);
    EXPECT_EQ(trades[1].quantity, 15);
    EXPECT_TRUE(book.empty());
}