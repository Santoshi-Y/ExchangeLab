#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "exchange/journal.hpp"
#include "exchange/protocol.hpp"
#include "exchange/replay.hpp"

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

template <std::size_t BodySize>
std::vector<std::byte> make_message(
    exchange::protocol::MessageType type,
    const std::array<std::byte, BodySize>& body,
    std::uint64_t sequence_number
) {
    const exchange::protocol::MessageHeader header {
        .magic = exchange::protocol::protocol_magic,
        .version = exchange::protocol::protocol_version,
        .type = type,
        .body_size = static_cast<std::uint32_t>(body.size()),
        .sequence_number = sequence_number
    };

    return combine_message(
        exchange::protocol::encode_header(header),
        body
    );
}

std::vector<std::byte> make_new_order_message(
    std::uint64_t order_id,
    std::uint64_t timestamp,
    std::int64_t price,
    std::uint64_t quantity,
    exchange::protocol::Side side
) {
    const exchange::protocol::NewOrderRequest request {
        .order_id = order_id,
        .timestamp = timestamp,
        .price = price,
        .quantity = quantity,
        .side = side,
        .order_type = exchange::protocol::OrderType::Limit
    };

    return make_message(
        exchange::protocol::MessageType::NewOrder,
        exchange::protocol::encode_new_order(request),
        timestamp
    );
}

std::vector<std::byte> make_cancel_message(
    std::uint64_t order_id,
    std::uint64_t timestamp
) {
    const exchange::protocol::CancelOrderRequest request {
        .order_id = order_id,
        .timestamp = timestamp
    };

    return make_message(
        exchange::protocol::MessageType::CancelOrder,
        exchange::protocol::encode_cancel_order(request),
        timestamp
    );
}

std::vector<std::byte> make_replace_message(
    std::uint64_t order_id,
    std::uint64_t timestamp,
    std::int64_t price,
    std::uint64_t quantity
) {
    const exchange::protocol::ReplaceOrderRequest request {
        .order_id = order_id,
        .timestamp = timestamp,
        .new_price = price,
        .new_quantity = quantity
    };

    return make_message(
        exchange::protocol::MessageType::ReplaceOrder,
        exchange::protocol::encode_replace_order(request),
        timestamp
    );
}

class ReplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        journal_path_ =
            std::filesystem::temp_directory_path() /
            "exchange_lab_replay_test.bin";
        std::filesystem::remove(journal_path_);
    }

    void TearDown() override {
        std::filesystem::remove(journal_path_);
    }

    std::filesystem::path journal_path_;
};

TEST_F(ReplayTest, ReconstructsOrderBookAndTrades) {
    {
        exchange::ExchangeJournal journal(journal_path_, true);
        journal.append(make_new_order_message(
            1, 1, 100, 20, exchange::protocol::Side::Buy));
        journal.append(make_new_order_message(
            2, 2, 105, 10, exchange::protocol::Side::Sell));
        journal.append(make_new_order_message(
            3, 3, 105, 6, exchange::protocol::Side::Buy));
        journal.flush();
    }

    exchange::ExchangeReplayer replayer;
    ASSERT_TRUE(replayer.replay(journal_path_));

    const auto& summary = replayer.summary();
    EXPECT_EQ(summary.journal_records, 3U);
    EXPECT_EQ(summary.new_orders, 3U);
    EXPECT_EQ(summary.trades, 1U);
    EXPECT_EQ(summary.rejected_messages, 0U);
    EXPECT_EQ(summary.unsupported_messages, 0U);

    const auto& book = replayer.order_book();
    EXPECT_EQ(book.order_count(), 2U);
    ASSERT_TRUE(book.has_bids());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_bid_level().total_quantity(), 20);
    ASSERT_TRUE(book.has_asks());
    EXPECT_EQ(book.best_ask(), 105);
    EXPECT_EQ(book.best_ask_level().total_quantity(), 4);
}

TEST_F(ReplayTest, ReplaysReplaceAndCancelLifecycle) {
    {
        exchange::ExchangeJournal journal(journal_path_, true);
        journal.append(make_new_order_message(
            10, 1, 100, 20, exchange::protocol::Side::Buy));
        journal.append(make_replace_message(
            10, 2, 102, 25));
        journal.append(make_new_order_message(
            11, 3, 110, 5, exchange::protocol::Side::Sell));
        journal.append(make_cancel_message(
            11, 4));
        journal.flush();
    }

    exchange::ExchangeReplayer replayer;
    ASSERT_TRUE(replayer.replay(journal_path_));

    const auto& summary = replayer.summary();
    EXPECT_EQ(summary.journal_records, 4U);
    EXPECT_EQ(summary.new_orders, 2U);
    EXPECT_EQ(summary.rejected_messages, 0U);
    EXPECT_EQ(summary.unsupported_messages, 0U);

    const auto& book = replayer.order_book();
    EXPECT_EQ(book.order_count(), 1U);

    const exchange::Order* replaced = book.find_order(10);
    ASSERT_NE(replaced, nullptr);
    EXPECT_EQ(replaced->price, 102);
    EXPECT_EQ(replaced->remaining_quantity, 25);
    EXPECT_EQ(book.find_order(11), nullptr);
}

TEST_F(ReplayTest, ProducesSameStateAcrossRepeatedReplays) {
    {
        exchange::ExchangeJournal journal(journal_path_, true);
        journal.append(make_new_order_message(
            20, 1, 99, 15, exchange::protocol::Side::Buy));
        journal.append(make_replace_message(
            20, 2, 100, 18));
        journal.flush();
    }

    exchange::ExchangeReplayer first;
    exchange::ExchangeReplayer second;

    ASSERT_TRUE(first.replay(journal_path_));
    ASSERT_TRUE(second.replay(journal_path_));

    EXPECT_EQ(
        first.summary().journal_records,
        second.summary().journal_records
    );
    EXPECT_EQ(
        first.order_book().order_count(),
        second.order_book().order_count()
    );
    EXPECT_EQ(
        first.order_book().best_bid(),
        second.order_book().best_bid()
    );
    EXPECT_EQ(
        first.order_book().best_bid_level().total_quantity(),
        second.order_book().best_bid_level().total_quantity()
    );
}

}  // namespace