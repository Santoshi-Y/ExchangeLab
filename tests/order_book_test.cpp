#include <gtest/gtest.h>

#include "exchange/order_book.hpp"

namespace {

exchange::Order make_order(
    exchange::OrderId id,
    exchange::Side side,
    exchange::Price price,
    exchange::Quantity quantity,
    exchange::Timestamp timestamp
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
        .timestamp = timestamp
    };
}

}  // namespace

TEST(OrderBookTest, StartsEmpty) {
    const exchange::OrderBook book;

    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.has_bids());
    EXPECT_FALSE(book.has_asks());
}

TEST(OrderBookTest, SelectsHighestBid) {
    exchange::OrderBook book;

    book.add_order(
        make_order(1, exchange::Side::Buy, 100, 10, 1)
    );

    book.add_order(
        make_order(2, exchange::Side::Buy, 103, 20, 2)
    );

    book.add_order(
        make_order(3, exchange::Side::Buy, 101, 30, 3)
    );

    ASSERT_TRUE(book.has_bids());
    EXPECT_EQ(book.best_bid(), 103);
    EXPECT_EQ(
        book.best_bid_level().total_quantity(),
        20
    );
}

TEST(OrderBookTest, SelectsLowestAsk) {
    exchange::OrderBook book;

    book.add_order(
        make_order(1, exchange::Side::Sell, 106, 10, 1)
    );

    book.add_order(
        make_order(2, exchange::Side::Sell, 104, 20, 2)
    );

    book.add_order(
        make_order(3, exchange::Side::Sell, 105, 30, 3)
    );

    ASSERT_TRUE(book.has_asks());
    EXPECT_EQ(book.best_ask(), 104);
    EXPECT_EQ(
        book.best_ask_level().total_quantity(),
        20
    );
}

TEST(OrderBookTest, AggregatesOrdersAtSamePrice) {
    exchange::OrderBook book;

    book.add_order(
        make_order(1, exchange::Side::Buy, 100, 10, 1)
    );

    book.add_order(
        make_order(2, exchange::Side::Buy, 100, 20, 2)
    );

    ASSERT_TRUE(book.has_bids());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(
        book.best_bid_level().order_count(),
        2U
    );
    EXPECT_EQ(
        book.best_bid_level().total_quantity(),
        30
    );
}

TEST(OrderBookTest, RemovesEmptyBestAskLevel) {
    exchange::OrderBook book;

    book.add_order(
        make_order(1, exchange::Side::Sell, 101, 10, 1)
    );

    book.add_order(
        make_order(2, exchange::Side::Sell, 102, 20, 2)
    );

    book.best_ask_level().fill_front(10);
    book.remove_best_ask_if_empty();

    ASSERT_TRUE(book.has_asks());
    EXPECT_EQ(book.best_ask(), 102);
}

TEST(OrderBookTest, RemovesEmptyBestBidLevel) {
    exchange::OrderBook book;

    book.add_order(
        make_order(1, exchange::Side::Buy, 101, 10, 1)
    );

    book.add_order(
        make_order(2, exchange::Side::Buy, 100, 20, 2)
    );

    book.best_bid_level().fill_front(10);
    book.remove_best_bid_if_empty();

    ASSERT_TRUE(book.has_bids());
    EXPECT_EQ(book.best_bid(), 100);
}

TEST(OrderBookTest, TracksAddedOrdersById) {
    exchange::OrderBook book;

    book.add_order(
        make_order(
            1,
            exchange::Side::Buy,
            100,
            10,
            1
        )
    );

    book.add_order(
        make_order(
            2,
            exchange::Side::Sell,
            105,
            20,
            2
        )
    );

    EXPECT_TRUE(book.contains(1));
    EXPECT_TRUE(book.contains(2));
    EXPECT_FALSE(book.contains(3));
    EXPECT_EQ(book.order_count(), 2U);
}

TEST(OrderBookTest, CancelsBidById) {
    exchange::OrderBook book;

    book.add_order(
        make_order(
            1,
            exchange::Side::Buy,
            100,
            25,
            1
        )
    );

    EXPECT_TRUE(book.cancel_order(1));

    EXPECT_FALSE(book.contains(1));
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_FALSE(book.has_bids());
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, CancelsAskById) {
    exchange::OrderBook book;

    book.add_order(
        make_order(
            1,
            exchange::Side::Sell,
            105,
            25,
            1
        )
    );

    EXPECT_TRUE(book.cancel_order(1));

    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.has_asks());
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, CancelsMiddleOrderAtPriceLevel) {
    exchange::OrderBook book;

    book.add_order(
        make_order(
            1,
            exchange::Side::Buy,
            100,
            10,
            1
        )
    );

    book.add_order(
        make_order(
            2,
            exchange::Side::Buy,
            100,
            20,
            2
        )
    );

    book.add_order(
        make_order(
            3,
            exchange::Side::Buy,
            100,
            30,
            3
        )
    );

    EXPECT_TRUE(book.cancel_order(2));

    ASSERT_TRUE(book.has_bids());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(
        book.best_bid_level().order_count(),
        2U
    );
    EXPECT_EQ(
        book.best_bid_level().total_quantity(),
        40
    );

    // FIFO front remains unchanged.
    EXPECT_EQ(book.best_bid_level().front().id, 1);

    EXPECT_TRUE(book.contains(1));
    EXPECT_FALSE(book.contains(2));
    EXPECT_TRUE(book.contains(3));
}

TEST(OrderBookTest, CancelRemovesOnlyEmptyPriceLevel) {
    exchange::OrderBook book;

    book.add_order(
        make_order(
            1,
            exchange::Side::Buy,
            101,
            10,
            1
        )
    );

    book.add_order(
        make_order(
            2,
            exchange::Side::Buy,
            100,
            20,
            2
        )
    );

    EXPECT_TRUE(book.cancel_order(1));

    ASSERT_TRUE(book.has_bids());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.order_count(), 1U);
    EXPECT_TRUE(book.contains(2));
}

TEST(OrderBookTest, CancelUnknownOrderReturnsFalse) {
    exchange::OrderBook book;

    EXPECT_FALSE(book.cancel_order(999));
    EXPECT_TRUE(book.empty());
}

TEST(OrderBookTest, RejectsDuplicateOrderIds) {
    exchange::OrderBook book;

    book.add_order(
        make_order(
            1,
            exchange::Side::Buy,
            100,
            10,
            1
        )
    );

    EXPECT_THROW(
        book.add_order(
            make_order(
                1,
                exchange::Side::Sell,
                105,
                20,
                2
            )
        ),
        std::invalid_argument
    );

    EXPECT_EQ(book.order_count(), 1U);
    EXPECT_TRUE(book.contains(1));
}