#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "exchange/exchange_server.hpp"
#include "exchange/journal.hpp"
#include "exchange/protocol.hpp"

namespace {

template <std::size_t HeaderSize, std::size_t BodySize>
std::vector<std::byte> combine_message(
    const std::array<std::byte, HeaderSize>& header,
    const std::array<std::byte, BodySize>& body
) {
    std::vector<std::byte> message;
    message.reserve(HeaderSize + BodySize);
    message.insert(message.end(), header.begin(), header.end());
    message.insert(message.end(), body.begin(), body.end());
    return message;
}

std::vector<std::byte> make_resting_order_message() {
    const exchange::protocol::NewOrderRequest request {
        .order_id = 9001,
        .timestamp = 1,
        .price = 105,
        .quantity = 10,
        .side = exchange::protocol::Side::Sell,
        .order_type = exchange::protocol::OrderType::Limit,
        .time_in_force =
            exchange::protocol::TimeInForce::GoodTillCancel
    };

    const auto body =
        exchange::protocol::encode_new_order(request);

    const exchange::protocol::MessageHeader header {
        .magic = exchange::protocol::protocol_magic,
        .version = exchange::protocol::protocol_version,
        .type = exchange::protocol::MessageType::NewOrder,
        .body_size =
            static_cast<std::uint32_t>(body.size()),
        .sequence_number = 1
    };

    return combine_message(
        exchange::protocol::encode_header(header),
        body
    );
}

class StartupRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        journal_path_ =
            std::filesystem::temp_directory_path() /
            "exchange_lab_startup_recovery_test.bin";

        std::filesystem::remove(journal_path_);
    }

    void TearDown() override {
        std::filesystem::remove(journal_path_);
    }

    std::filesystem::path journal_path_;
};

TEST_F(
    StartupRecoveryTest,
    ReplaysJournalBeforeAcceptingConnections
) {
    {
        exchange::ExchangeJournal journal(
            journal_path_,
            true
        );

        journal.append(
            make_resting_order_message()
        );

        journal.flush();
    }

    constexpr std::uint16_t port = 19020;

    exchange::ExchangeServer server(
        port,
        journal_path_
    );

    ASSERT_TRUE(server.start());

    const exchange::RecoveryState& recovery =
        server.recovery_state();

    EXPECT_TRUE(recovery.attempted);
    EXPECT_TRUE(recovery.successful);
    EXPECT_EQ(recovery.journal_records, 1U);
    EXPECT_EQ(recovery.new_orders, 1U);
    EXPECT_EQ(recovery.trades, 0U);
    EXPECT_EQ(recovery.rejected_messages, 0U);
    EXPECT_EQ(recovery.unsupported_messages, 0U);
    EXPECT_EQ(recovery.remaining_orders, 1U);

    server.stop();
}

}  // namespace