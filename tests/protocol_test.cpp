#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "exchange/protocol.hpp"

namespace {

using exchange::protocol::MessageHeader;
using exchange::protocol::MessageType;
using exchange::protocol::NewOrderRequest;
using exchange::protocol::OrderType;
using exchange::protocol::Side;

TEST(ProtocolTest, EncodesAndDecodesHeader) {
    const MessageHeader original {
        .magic = exchange::protocol::protocol_magic,
        .version = exchange::protocol::protocol_version,
        .type = MessageType::NewOrder,
        .body_size = exchange::protocol::new_order_body_size,
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
    EXPECT_EQ(decoded->sequence_number, original.sequence_number);
}

TEST(ProtocolTest, RejectsHeaderWithInvalidMagic) {
    MessageHeader header {
        .magic = 0,
        .version = exchange::protocol::protocol_version,
        .type = MessageType::NewOrder,
        .body_size = exchange::protocol::new_order_body_size,
        .sequence_number = 1
    };

    const auto encoded =
        exchange::protocol::encode_header(header);

    EXPECT_FALSE(
        exchange::protocol::decode_header(encoded).has_value()
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
        exchange::protocol::decode_new_order(encoded).has_value()
    );
}

TEST(ProtocolTest, RejectsTruncatedOrderMessage) {
    const std::array<std::byte, 5> truncated {};

    EXPECT_FALSE(
        exchange::protocol::decode_new_order(truncated).has_value()
    );
}

}  // namespace