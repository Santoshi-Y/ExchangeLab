#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "exchange/exchange_server.hpp"
#include "exchange/protocol.hpp"
#include "exchange/replay.hpp"
#include "exchange/risk_engine.hpp"

namespace {

using namespace std::chrono_literals;

exchange::RiskOrderRequest make_risk_order(
    exchange::RiskClientId client_id,
    exchange::OrderId order_id,
    exchange::Side side,
    exchange::Price price,
    exchange::Quantity quantity,
    std::string_view symbol = "AAPL"
) {
    return {
        .client_id = client_id,
        .symbol = symbol,
        .order_id = order_id,
        .side = side,
        .order_type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = price,
        .quantity = quantity,
        .reference_price = std::nullopt
    };
}

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

bool send_all(
    int socket,
    std::span<const std::byte> bytes
) {
    std::size_t total_sent = 0;

    while (total_sent < bytes.size()) {
        const auto sent = ::send(
            socket,
            bytes.data() + total_sent,
            bytes.size() - total_sent,
            0
        );

        if (sent <= 0) {
            return false;
        }

        total_sent += static_cast<std::size_t>(sent);
    }

    return true;
}

bool receive_exact(
    int socket,
    std::span<std::byte> output
) {
    std::size_t total_received = 0;

    while (total_received < output.size()) {
        const auto received = ::recv(
            socket,
            output.data() + total_received,
            output.size() - total_received,
            0
        );

        if (received <= 0) {
            return false;
        }

        total_received +=
            static_cast<std::size_t>(received);
    }

    return true;
}

int connect_to_server(std::uint16_t port) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const int socket = ::socket(AF_INET, SOCK_STREAM, 0);

        if (socket < 0) {
            return -1;
        }

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);

        if (
            ::inet_pton(
                AF_INET,
                "127.0.0.1",
                &address.sin_addr
            ) != 1
        ) {
            ::close(socket);
            return -1;
        }

        if (
            ::connect(
                socket,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)
            ) == 0
        ) {
            return socket;
        }

        ::close(socket);
        std::this_thread::sleep_for(10ms);
    }

    return -1;
}

void close_client(int socket) {
    if (socket >= 0) {
        ::shutdown(socket, SHUT_RDWR);
        ::close(socket);
    }
}

}  // namespace

TEST(
    RiskEngineTest,
    RejectsOrderAboveQuantityLimit
) {
    exchange::RiskLimits limits;
    limits.max_order_quantity = 10;

    exchange::RiskEngine risk(limits);

    const auto decision = risk.check_new_order(
        make_risk_order(
            1,
            10,
            exchange::Side::Buy,
            100,
            11
        )
    );

    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(
        decision.reason,
        exchange::RiskRejectReason::MaxOrderQuantity
    );
}

TEST(
    RiskEngineTest,
    RejectsOrderAboveNotionalLimit
) {
    exchange::RiskLimits limits;
    limits.max_order_notional = 1'000;

    exchange::RiskEngine risk(limits);

    const auto decision = risk.check_new_order(
        make_risk_order(
            1,
            11,
            exchange::Side::Buy,
            101,
            10
        )
    );

    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(
        decision.reason,
        exchange::RiskRejectReason::MaxOrderNotional
    );
}

TEST(
    RiskEngineTest,
    TracksOpenOrderLimit
) {
    exchange::RiskLimits limits;
    limits.max_open_orders = 1;

    exchange::RiskEngine risk(limits);

    ASSERT_TRUE(
        risk.check_new_order(
            make_risk_order(
                1,
                1,
                exchange::Side::Buy,
                100,
                10
            )
        ).accepted
    );

    risk.on_order_resting(
        1,
        "AAPL",
        1,
        exchange::Side::Buy,
        100,
        10
    );

    const auto second = risk.check_new_order(
        make_risk_order(
            1,
            2,
            exchange::Side::Sell,
            110,
            5,
            "MSFT"
        )
    );

    EXPECT_FALSE(second.accepted);
    EXPECT_EQ(
        second.reason,
        exchange::RiskRejectReason::MaxOpenOrders
    );
}

TEST(
    RiskEngineTest,
    ProjectedPositionIncludesWorkingOrders
) {
    exchange::RiskLimits limits;
    limits.max_position_per_symbol = 100;

    exchange::RiskEngine risk(limits);

    risk.on_order_resting(
        1,
        "AAPL",
        1,
        exchange::Side::Buy,
        100,
        70
    );

    const auto decision = risk.check_new_order(
        make_risk_order(
            1,
            2,
            exchange::Side::Buy,
            101,
            40
        )
    );

    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(
        decision.reason,
        exchange::RiskRejectReason::MaxPosition
    );
}

TEST(
    RiskEngineTest,
    TradeUpdatesPositionsAndWorkingExposure
) {
    exchange::RiskEngine risk;

    risk.on_order_resting(
        2,
        "AAPL",
        22,
        exchange::Side::Sell,
        105,
        50
    );

    risk.on_trade(
        "AAPL",
        1,
        2,
        11,
        22,
        20
    );

    const auto buyer =
        risk.snapshot(1, "AAPL");

    const auto seller =
        risk.snapshot(2, "AAPL");

    EXPECT_EQ(buyer.position, 20);
    EXPECT_EQ(seller.position, -20);
    EXPECT_EQ(seller.open_sell_quantity, 30U);
    EXPECT_EQ(seller.gross_open_quantity, 30U);
    EXPECT_EQ(seller.open_orders, 1U);
}

TEST(
    RiskEngineTest,
    CancelReleasesWorkingExposure
) {
    exchange::RiskEngine risk;

    risk.on_order_resting(
        7,
        "MSFT",
        99,
        exchange::Side::Buy,
        410,
        25
    );

    risk.on_order_cancelled(
        7,
        "MSFT",
        99
    );

    const auto snapshot =
        risk.snapshot(7, "MSFT");

    EXPECT_EQ(snapshot.open_orders, 0U);
    EXPECT_EQ(snapshot.gross_open_quantity, 0U);
    EXPECT_EQ(snapshot.open_buy_quantity, 0U);
}

TEST(
    RiskEngineTest,
    ReplacementDoesNotDoubleCountOriginalOrder
) {
    exchange::RiskLimits limits;
    limits.max_open_orders = 1;
    limits.max_open_quantity = 100;
    limits.max_position_per_symbol = 1'000;

    exchange::RiskEngine risk(limits);

    risk.on_order_resting(
        3,
        "AAPL",
        55,
        exchange::Side::Buy,
        100,
        80
    );

    const auto accepted = risk.check_replace(
        3,
        "AAPL",
        55,
        101,
        90
    );

    EXPECT_TRUE(accepted.accepted);

    const auto rejected = risk.check_replace(
        3,
        "AAPL",
        55,
        101,
        101
    );

    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(
        rejected.reason,
        exchange::RiskRejectReason::MaxOpenQuantity
    );
}

TEST(
    RiskEngineTest,
    GlobalKillSwitchRejectsRiskIncreasingOrders
) {
    exchange::RiskEngine risk;
    risk.set_global_kill_switch(true);

    const auto decision = risk.check_new_order(
        make_risk_order(
            1,
            1,
            exchange::Side::Buy,
            100,
            1
        )
    );

    EXPECT_FALSE(decision.accepted);
    EXPECT_EQ(
        decision.reason,
        exchange::RiskRejectReason::GlobalKillSwitch
    );
}

TEST(
    RiskEngineIntegrationTest,
    RiskRejectedOrderIsNotJournaledOrRecovered
) {
    constexpr std::uint16_t port = 19040;

    const std::filesystem::path journal_path =
        std::filesystem::temp_directory_path() /
        "exchange_lab_risk_rejection.bin";

    std::filesystem::remove(journal_path);

    exchange::RiskLimits limits;
    limits.max_order_quantity = 10;

    exchange::ExchangeServer server(
        port,
        journal_path,
        std::nullopt,
        limits
    );

    ASSERT_TRUE(server.start());

    std::thread server_thread([&server]() {
        server.run();
    });

    const int client_socket =
        connect_to_server(port);

    ASSERT_GE(client_socket, 0);

    const exchange::protocol::NewOrderRequest request {
        .order_id = 500,
        .timestamp = 1,
        .price = 100,
        .quantity = 11,
        .side = exchange::protocol::Side::Buy,
        .order_type = exchange::protocol::OrderType::Limit,
        .time_in_force =
            exchange::protocol::TimeInForce::GoodTillCancel,
        .symbol =
            exchange::protocol::make_symbol("AAPL")
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

    ASSERT_TRUE(
        send_all(
            client_socket,
            combine_message(
                exchange::protocol::encode_header(header),
                body
            )
        )
    );

    std::array<
        std::byte,
        exchange::protocol::header_size
    > response_header_bytes {};

    ASSERT_TRUE(
        receive_exact(
            client_socket,
            response_header_bytes
        )
    );

    const auto response_header =
        exchange::protocol::decode_header(
            response_header_bytes
        );

    ASSERT_TRUE(response_header.has_value());
    EXPECT_EQ(
        response_header->type,
        exchange::protocol::MessageType::OrderRejected
    );

    std::vector<std::byte> response_body(
        response_header->body_size
    );

    ASSERT_TRUE(
        receive_exact(
            client_socket,
            response_body
        )
    );

    const auto response =
        exchange::protocol::decode_order_response(
            response_body
        );

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->order_id, 500U);
    EXPECT_EQ(response->success, 0);

    close_client(client_socket);
    server.stop();
    server_thread.join();

    exchange::ExchangeReplayer replayer;
    ASSERT_TRUE(replayer.replay(journal_path));
    EXPECT_EQ(replayer.summary().journal_records, 0U);
    EXPECT_EQ(replayer.summary().new_orders, 0U);

    std::filesystem::remove(journal_path);
}