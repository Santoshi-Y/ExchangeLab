#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "exchange/fix.hpp"
#include "exchange/fix_gateway.hpp"
#include "exchange/protocol.hpp"

namespace {

exchange::fix::Message make_logon() {
    exchange::fix::Message message;
    message.add(35, "A");
    message.add(34, "1");
    message.add(49, "CLIENT1");
    message.add(56, "EXCHANGELAB");
    message.add(52, "20260807-15:00:00.000");
    message.add(98, "0");
    message.add(108, "30");
    return message;
}

exchange::fix::Message make_limit_order() {
    exchange::fix::Message message;
    message.add(35, "D");
    message.add(34, "2");
    message.add(49, "CLIENT1");
    message.add(56, "EXCHANGELAB");
    message.add(52, "20260807-15:00:01.000");
    message.add(11, "A-1");
    message.add(55, "AAPL");
    message.add(54, "1");
    message.add(38, "25");
    message.add(40, "2");
    message.add(44, "185");
    message.add(59, "1");
    return message;
}

}  // namespace

TEST(FixCodecTest, RoundTripsAndValidatesMessage) {
    const auto original = make_logon();
    const std::string wire = exchange::fix::encode(original);

    const auto parsed = exchange::fix::parse(wire);

    ASSERT_TRUE(parsed);
    ASSERT_TRUE(parsed.message.has_value());
    EXPECT_EQ(parsed.message->get(35), "A");
    EXPECT_EQ(parsed.message->get(49), "CLIENT1");
    EXPECT_EQ(parsed.message->get(108), "30");
    EXPECT_TRUE(wire.starts_with("8=FIX.4.4\x01"));
    EXPECT_NE(wire.find("10="), std::string::npos);
}

TEST(FixCodecTest, RejectsBadChecksum) {
    std::string wire = exchange::fix::encode(make_logon());

    const std::size_t checksum = wire.rfind("10=");
    ASSERT_NE(checksum, std::string::npos);
    wire[checksum + 5] = wire[checksum + 5] == '9' ? '0' : '9';

    const auto parsed = exchange::fix::parse(wire);

    EXPECT_FALSE(parsed);
    EXPECT_EQ(parsed.error, "CheckSum mismatch");
}

TEST(FixCodecTest, RejectsBadBodyLength) {
    std::string wire = exchange::fix::encode(make_logon());

    const std::size_t body_length = wire.find("9=");
    ASSERT_NE(body_length, std::string::npos);
    const std::size_t end = wire.find(exchange::fix::soh, body_length);
    ASSERT_NE(end, std::string::npos);

    wire.replace(body_length + 2, end - (body_length + 2), "1");

    const auto parsed = exchange::fix::parse(wire);

    EXPECT_FALSE(parsed);
    EXPECT_EQ(parsed.error, "BodyLength does not end at CheckSum");
}

TEST(FixCodecTest, ExtractsMessageAfterPartialTcpRead) {
    const std::string wire = exchange::fix::encode(make_logon());
    std::string buffer = wire.substr(0, wire.size() / 2);

    auto first = exchange::fix::extract_one(buffer);
    EXPECT_EQ(
        first.status,
        exchange::fix::ExtractStatus::NeedMoreData
    );

    buffer += wire.substr(wire.size() / 2);
    auto second = exchange::fix::extract_one(buffer);

    EXPECT_EQ(
        second.status,
        exchange::fix::ExtractStatus::MessageReady
    );
    EXPECT_EQ(second.wire_message, wire);
    EXPECT_TRUE(buffer.empty());
}

TEST(FixCodecTest, ExtractsBackToBackMessages) {
    const std::string first_wire = exchange::fix::encode(make_logon());

    auto heartbeat = make_logon();
    heartbeat.set(35, "0");
    heartbeat.set(34, "2");
    const std::string second_wire = exchange::fix::encode(heartbeat);

    std::string buffer = first_wire + second_wire;

    const auto first = exchange::fix::extract_one(buffer);
    const auto second = exchange::fix::extract_one(buffer);

    EXPECT_EQ(first.wire_message, first_wire);
    EXPECT_EQ(second.wire_message, second_wire);
    EXPECT_TRUE(buffer.empty());
}

TEST(FixGatewayTest, TranslatesLimitGtcOrder) {
    const auto message = make_limit_order();
    std::string error;

    const auto translated =
        exchange::fixbridge::translate_new_order_single(
            message,
            9001,
            123456,
            error
        );

    ASSERT_TRUE(translated.has_value()) << error;
    EXPECT_EQ(translated->cl_ord_id, "A-1");
    EXPECT_EQ(translated->request.order_id, 9001U);
    EXPECT_EQ(
        exchange::protocol::symbol_to_string(
            translated->request.symbol
        ),
        "AAPL"
    );
    EXPECT_EQ(
        translated->request.side,
        exchange::protocol::Side::Buy
    );
    EXPECT_EQ(
        translated->request.order_type,
        exchange::protocol::OrderType::Limit
    );
    EXPECT_EQ(
        translated->request.time_in_force,
        exchange::protocol::TimeInForce::GoodTillCancel
    );
    EXPECT_EQ(translated->request.quantity, 25U);
    EXPECT_EQ(translated->request.price, 185);
}

TEST(FixGatewayTest, TranslatesMarketGtcAsIoc) {
    auto message = make_limit_order();
    message.set(40, "1");
    message.set(59, "1");

    std::string error;
    const auto translated =
        exchange::fixbridge::translate_new_order_single(
            message,
            9002,
            123457,
            error
        );

    ASSERT_TRUE(translated.has_value()) << error;
    EXPECT_EQ(
        translated->request.order_type,
        exchange::protocol::OrderType::Market
    );
    EXPECT_EQ(
        translated->request.time_in_force,
        exchange::protocol::TimeInForce::ImmediateOrCancel
    );
    EXPECT_EQ(translated->request.price, 0);
}

TEST(FixGatewayTest, RejectsInvalidSymbolAndUnsupportedTif) {
    auto bad_symbol = make_limit_order();
    bad_symbol.set(55, "apple");

    std::string error;
    EXPECT_FALSE(
        exchange::fixbridge::translate_new_order_single(
            bad_symbol,
            9003,
            123458,
            error
        ).has_value()
    );
    EXPECT_NE(error.find("invalid Symbol"), std::string::npos);

    auto bad_tif = make_limit_order();
    bad_tif.set(59, "0");
    error.clear();

    EXPECT_FALSE(
        exchange::fixbridge::translate_new_order_single(
            bad_tif,
            9004,
            123459,
            error
        ).has_value()
    );
    EXPECT_NE(error.find("unsupported TimeInForce"), std::string::npos);
}