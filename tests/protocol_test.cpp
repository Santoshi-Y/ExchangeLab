#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "exchange/protocol.hpp"

namespace {

using exchange::protocol::BookUpdate;
using exchange::protocol::MessageHeader;
using exchange::protocol::MessageType;
using exchange::protocol::NewOrderRequest;
using exchange::protocol::OrderType;
using exchange::protocol::Side;
using exchange::protocol::TradeExecution;

TEST(ProtocolTest, EncodesAndDecodesHeader) {
    const MessageHeader original {
        .magic = exchange::protocol::protocol_magic,
        .version = exchange::protocol::protocol_version,
        .type = MessageType::NewOrder,
        .body_size =
            exchange::protocol::new_order_body_size,
        .sequence_number = 42
    };

    const auto encoded =
        exchange::protocol::encode_header(original);

    const auto decoded =
        exchange::protocol::decode_header(encoded);

    ASSERT_TRUE(decoded.has_value());

    EXPECT_EQ(decoded->magic, original.magic);
    EXPECT_EQ(decoded->version, original.version);
    EXPECT_EQ(decoded->type, original.type);
    EXPECT_EQ(decoded->body_size, original.body_size);
    EXPECT_EQ(
        decoded->sequence_number,
        original.sequence_number
    );
}

TEST(ProtocolTest, RejectsHeaderWithInvalidMagic) {
    const MessageHeader header {
        .magic = 0,
        .version = exchange::protocol::protocol_version,
        .type = MessageType::NewOrder,
        .body_size =
            exchange::protocol::new_order_body_size,
        .sequence_number = 1
    };

    const auto encoded =
        exchange::protocol::encode_header(header);

    EXPECT_FALSE(
        exchange::protocol::decode_header(
            encoded
        ).has_value()
    );
}

TEST(ProtocolTest, EncodesAndDecodesNewLimitOrder) {
    const NewOrderRequest original {
        .order_id = 123,
        .timestamp = 456,
        .price = 10'025,
        .quantity = 100,
        .side = Side::Buy,
        .order_type = OrderType::Limit
    };

    const auto encoded =
        exchange::protocol::encode_new_order(original);

    const auto decoded =
        exchange::protocol::decode_new_order(encoded);

    ASSERT_TRUE(decoded.has_value());

    EXPECT_EQ(decoded->order_id, original.order_id);
    EXPECT_EQ(decoded->timestamp, original.timestamp);
    EXPECT_EQ(decoded->price, original.price);
    EXPECT_EQ(decoded->quantity, original.quantity);
    EXPECT_EQ(decoded->side, original.side);
    EXPECT_EQ(decoded->order_type, original.order_type);
}

TEST(ProtocolTest, EncodesAndDecodesMarketOrder) {
    const NewOrderRequest original {
        .order_id = 900,
        .timestamp = 1'000,
        .price = 0,
        .quantity = 25,
        .side = Side::Sell,
        .order_type = OrderType::Market
    };

    const auto encoded =
        exchange::protocol::encode_new_order(original);

    const auto decoded =
        exchange::protocol::decode_new_order(encoded);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->order_type, OrderType::Market);
    EXPECT_EQ(decoded->side, Side::Sell);
    EXPECT_EQ(decoded->quantity, 25);
}

TEST(ProtocolTest, RejectsZeroQuantityOrder) {
    const NewOrderRequest request {
        .order_id = 1,
        .timestamp = 2,
        .price = 100,
        .quantity = 0,
        .side = Side::Buy,
        .order_type = OrderType::Limit
    };

    const auto encoded =
        exchange::protocol::encode_new_order(request);

    EXPECT_FALSE(
        exchange::protocol::decode_new_order(
            encoded
        ).has_value()
    );
}

TEST(ProtocolTest, RejectsTruncatedOrderMessage) {
    const std::array<std::byte, 5> truncated {};

    EXPECT_FALSE(
        exchange::protocol::decode_new_order(
            truncated
        ).has_value()
    );
}

TEST(ProtocolTest, EncodesAndDecodesTradeExecution) {
    const TradeExecution original {
        .buy_order_id = 101,
        .sell_order_id = 202,
        .price = 10'050,
        .quantity = 75,
        .sequence_number = 9
    };

    const auto encoded =
        exchange::protocol::encode_trade_execution(
            original
        );

    const auto decoded =
        exchange::protocol::decode_trade_execution(
            encoded
        );

    ASSERT_TRUE(decoded.has_value());

    EXPECT_EQ(
        decoded->buy_order_id,
        original.buy_order_id
    );

    EXPECT_EQ(
        decoded->sell_order_id,
        original.sell_order_id
    );

    EXPECT_EQ(decoded->price, original.price);
    EXPECT_EQ(decoded->quantity, original.quantity);

    EXPECT_EQ(
        decoded->sequence_number,
        original.sequence_number
    );
}

TEST(ProtocolTest, EncodesAndDecodesTwoSidedBookUpdate) {
    const BookUpdate original {
        .has_bid = 1,
        .best_bid = 100,
        .bid_quantity = 30,
        .has_ask = 1,
        .best_ask = 105,
        .ask_quantity = 40,
        .sequence_number = 12
    };

    const auto encoded =
        exchange::protocol::encode_book_update(
            original
        );

    const auto decoded =
        exchange::protocol::decode_book_update(
            encoded
        );

    ASSERT_TRUE(decoded.has_value());

    EXPECT_EQ(decoded->has_bid, 1);
    EXPECT_EQ(decoded->best_bid, 100);
    EXPECT_EQ(decoded->bid_quantity, 30);

    EXPECT_EQ(decoded->has_ask, 1);
    EXPECT_EQ(decoded->best_ask, 105);
    EXPECT_EQ(decoded->ask_quantity, 40);

    EXPECT_EQ(decoded->sequence_number, 12);
}

TEST(ProtocolTest, EncodesAndDecodesEmptyBookUpdate) {
    const BookUpdate original {
        .has_bid = 0,
        .best_bid = 0,
        .bid_quantity = 0,
        .has_ask = 0,
        .best_ask = 0,
        .ask_quantity = 0,
        .sequence_number = 15
    };

    const auto encoded =
        exchange::protocol::encode_book_update(
            original
        );

    const auto decoded =
        exchange::protocol::decode_book_update(
            encoded
        );

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->has_bid, 0);
    EXPECT_EQ(decoded->has_ask, 0);
    EXPECT_EQ(decoded->sequence_number, 15);
}

TEST(ProtocolTest, RejectsInconsistentBookUpdate) {
    const BookUpdate invalid {
        .has_bid = 0,
        .best_bid = 100,
        .bid_quantity = 20,
        .has_ask = 0,
        .best_ask = 0,
        .ask_quantity = 0,
        .sequence_number = 1
    };

    const auto encoded =
        exchange::protocol::encode_book_update(
            invalid
        );

    EXPECT_FALSE(
        exchange::protocol::decode_book_update(
            encoded
        ).has_value()
    );
}

}  // namespace