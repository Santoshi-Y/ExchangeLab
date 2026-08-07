#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "exchange/exchange_server.hpp"
#include "exchange/protocol.hpp"

namespace {

using namespace std::chrono_literals;

struct ReceivedMessage {
    exchange::protocol::MessageHeader header;
    std::vector<std::byte> body;
};

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

        total_received += static_cast<std::size_t>(received);
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
            const timeval receive_timeout {
                .tv_sec = 2,
                .tv_usec = 0
            };

            (void)::setsockopt(
                socket,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &receive_timeout,
                sizeof(receive_timeout)
            );

            return socket;
        }

        ::close(socket);
        std::this_thread::sleep_for(10ms);
    }

    return -1;
}

template <std::size_t HeaderSize, std::size_t BodySize>
std::vector<std::byte> combine_message(
    const std::array<std::byte, HeaderSize>& header,
    const std::array<std::byte, BodySize>& body
) {
    std::vector<std::byte> message;
    message.reserve(HeaderSize + BodySize);

    message.insert(
        message.end(),
        header.begin(),
        header.end()
    );

    message.insert(
        message.end(),
        body.begin(),
        body.end()
    );

    return message;
}

bool send_order(
    int socket,
    const exchange::protocol::NewOrderRequest& request,
    std::uint64_t sequence_number
) {
    const auto body =
        exchange::protocol::encode_new_order(request);

    const exchange::protocol::MessageHeader header {
        .magic = exchange::protocol::protocol_magic,
        .version = exchange::protocol::protocol_version,
        .type = exchange::protocol::MessageType::NewOrder,
        .body_size = static_cast<std::uint32_t>(body.size()),
        .sequence_number = sequence_number
    };

    return send_all(
        socket,
        combine_message(
            exchange::protocol::encode_header(header),
            body
        )
    );
}

std::optional<ReceivedMessage> receive_message(int socket) {
    std::array<
        std::byte,
        exchange::protocol::header_size
    > header_bytes {};

    if (!receive_exact(socket, header_bytes)) {
        return std::nullopt;
    }

    const auto header =
        exchange::protocol::decode_header(header_bytes);

    if (!header.has_value()) {
        return std::nullopt;
    }

    std::vector<std::byte> body(
        static_cast<std::size_t>(header->body_size)
    );

    if (!body.empty() && !receive_exact(socket, body)) {
        return std::nullopt;
    }

    return ReceivedMessage {
        .header = *header,
        .body = std::move(body)
    };
}

std::optional<ReceivedMessage> receive_until_type(
    int socket,
    exchange::protocol::MessageType expected_type,
    std::size_t maximum_messages = 16
) {
    for (
        std::size_t index = 0;
        index < maximum_messages;
        ++index
    ) {
        auto message = receive_message(socket);

        if (!message.has_value()) {
            return std::nullopt;
        }

        if (message->header.type == expected_type) {
            return message;
        }
    }

    return std::nullopt;
}

std::optional<exchange::protocol::OrderResponse>
receive_order_response(int socket) {
    for (std::size_t index = 0; index < 16; ++index) {
        auto message = receive_message(socket);

        if (!message.has_value()) {
            return std::nullopt;
        }

        if (
            message->header.type ==
                exchange::protocol::MessageType::OrderAccepted ||
            message->header.type ==
                exchange::protocol::MessageType::OrderRejected
        ) {
            return exchange::protocol::decode_order_response(
                message->body
            );
        }
    }

    return std::nullopt;
}

std::optional<exchange::protocol::TradeExecution>
receive_trade_execution(int socket) {
    const auto message = receive_until_type(
        socket,
        exchange::protocol::MessageType::TradeExecution
    );

    if (!message.has_value()) {
        return std::nullopt;
    }

    return exchange::protocol::decode_trade_execution(
        message->body
    );
}

std::optional<exchange::protocol::BookUpdate>
receive_book_update(int socket) {
    const auto message = receive_until_type(
        socket,
        exchange::protocol::MessageType::BookUpdate
    );

    if (!message.has_value()) {
        return std::nullopt;
    }

    return exchange::protocol::decode_book_update(
        message->body
    );
}

void close_client(int socket) {
    if (socket >= 0) {
        ::shutdown(socket, SHUT_RDWR);
        ::close(socket);
    }
}

TEST(
    ExchangeServerTest,
    BroadcastsBestAskAfterRestingSellOrder
) {
    constexpr std::uint16_t port = 19001;

    exchange::ExchangeServer server(port);
    ASSERT_TRUE(server.start());

    std::thread server_thread([&server]() {
        server.run();
    });

    const int first_socket = connect_to_server(port);
    const int second_socket = connect_to_server(port);

    ASSERT_GE(first_socket, 0);
    ASSERT_GE(second_socket, 0);

    const exchange::protocol::NewOrderRequest sell_order {
        .order_id = 2001,
        .timestamp = 1,
        .price = 105,
        .quantity = 20,
        .side = exchange::protocol::Side::Sell,
        .order_type = exchange::protocol::OrderType::Limit
    };

    ASSERT_TRUE(send_order(first_socket, sell_order, 1));

    const auto acceptance = receive_order_response(first_socket);
    ASSERT_TRUE(acceptance.has_value());
    EXPECT_EQ(acceptance->order_id, 2001);
    EXPECT_EQ(acceptance->success, 1);

    const auto first_update = receive_book_update(first_socket);
    const auto second_update = receive_book_update(second_socket);

    ASSERT_TRUE(first_update.has_value());
    ASSERT_TRUE(second_update.has_value());

    EXPECT_EQ(first_update->has_bid, 0);
    EXPECT_EQ(first_update->has_ask, 1);
    EXPECT_EQ(first_update->best_ask, 105);
    EXPECT_EQ(first_update->ask_quantity, 20);

    EXPECT_EQ(second_update->has_bid, 0);
    EXPECT_EQ(second_update->has_ask, 1);
    EXPECT_EQ(second_update->best_ask, 105);
    EXPECT_EQ(second_update->ask_quantity, 20);

    close_client(first_socket);
    close_client(second_socket);
    server.stop();
    server_thread.join();
}

TEST(
    ExchangeServerTest,
    BroadcastsTwoSidedBestPrices
) {
    constexpr std::uint16_t port = 19002;

    exchange::ExchangeServer server(port);
    ASSERT_TRUE(server.start());

    std::thread server_thread([&server]() {
        server.run();
    });

    // Establish and exercise the first connection before opening the
    // observer. This removes a race where connect() can succeed while the
    // server thread has not yet accepted/registered the observer socket.
    const int buyer_socket = connect_to_server(port);
    ASSERT_GE(buyer_socket, 0);

    const exchange::protocol::NewOrderRequest buy_order {
        .order_id = 3001,
        .timestamp = 1,
        .price = 100,
        .quantity = 30,
        .side = exchange::protocol::Side::Buy,
        .order_type = exchange::protocol::OrderType::Limit
    };

    ASSERT_TRUE(send_order(buyer_socket, buy_order, 1));

    const auto buy_acceptance = receive_order_response(buyer_socket);
    ASSERT_TRUE(buy_acceptance.has_value());
    EXPECT_EQ(buy_acceptance->order_id, 3001);

    const auto buyer_first_update = receive_book_update(buyer_socket);
    ASSERT_TRUE(buyer_first_update.has_value());
    EXPECT_EQ(buyer_first_update->has_bid, 1);
    EXPECT_EQ(buyer_first_update->best_bid, 100);
    EXPECT_EQ(buyer_first_update->bid_quantity, 30);
    EXPECT_EQ(buyer_first_update->has_ask, 0);

    const int observer_socket = connect_to_server(port);
    ASSERT_GE(observer_socket, 0);

    const exchange::protocol::NewOrderRequest sell_order {
        .order_id = 3002,
        .timestamp = 2,
        .price = 105,
        .quantity = 40,
        .side = exchange::protocol::Side::Sell,
        .order_type = exchange::protocol::OrderType::Limit
    };

    // Processing a request from observer_socket proves that the server has
    // accepted and registered it before the resulting book update is
    // broadcast.
    ASSERT_TRUE(send_order(observer_socket, sell_order, 2));

    const auto sell_acceptance =
        receive_order_response(observer_socket);

    ASSERT_TRUE(sell_acceptance.has_value());
    EXPECT_EQ(sell_acceptance->order_id, 3002);

    const auto buyer_second_update =
        receive_book_update(buyer_socket);

    const auto observer_second_update =
        receive_book_update(observer_socket);

    ASSERT_TRUE(buyer_second_update.has_value());
    ASSERT_TRUE(observer_second_update.has_value());

    EXPECT_EQ(buyer_second_update->has_bid, 1);
    EXPECT_EQ(buyer_second_update->best_bid, 100);
    EXPECT_EQ(buyer_second_update->bid_quantity, 30);
    EXPECT_EQ(buyer_second_update->has_ask, 1);
    EXPECT_EQ(buyer_second_update->best_ask, 105);
    EXPECT_EQ(buyer_second_update->ask_quantity, 40);

    EXPECT_EQ(observer_second_update->has_bid, 1);
    EXPECT_EQ(observer_second_update->best_bid, 100);
    EXPECT_EQ(observer_second_update->bid_quantity, 30);
    EXPECT_EQ(observer_second_update->has_ask, 1);
    EXPECT_EQ(observer_second_update->best_ask, 105);
    EXPECT_EQ(observer_second_update->ask_quantity, 40);

    close_client(buyer_socket);
    close_client(observer_socket);
    server.stop();
    server_thread.join();
}

TEST(
    ExchangeServerTest,
    BroadcastsEmptyBookAfterFullExecution
) {
    constexpr std::uint16_t port = 19003;

    exchange::ExchangeServer server(port);
    ASSERT_TRUE(server.start());

    std::thread server_thread([&server]() {
        server.run();
    });

    const int buyer_socket = connect_to_server(port);
    const int seller_socket = connect_to_server(port);

    ASSERT_GE(buyer_socket, 0);
    ASSERT_GE(seller_socket, 0);

    const exchange::protocol::NewOrderRequest sell_order {
        .order_id = 4001,
        .timestamp = 1,
        .price = 100,
        .quantity = 10,
        .side = exchange::protocol::Side::Sell,
        .order_type = exchange::protocol::OrderType::Limit
    };

    ASSERT_TRUE(send_order(seller_socket, sell_order, 1));

    const auto sell_acceptance =
        receive_order_response(seller_socket);

    ASSERT_TRUE(sell_acceptance.has_value());

    ASSERT_TRUE(receive_book_update(seller_socket).has_value());
    ASSERT_TRUE(receive_book_update(buyer_socket).has_value());

    const exchange::protocol::NewOrderRequest buy_order {
        .order_id = 4002,
        .timestamp = 2,
        .price = 100,
        .quantity = 10,
        .side = exchange::protocol::Side::Buy,
        .order_type = exchange::protocol::OrderType::Limit
    };

    ASSERT_TRUE(send_order(buyer_socket, buy_order, 2));

    const auto buy_acceptance =
        receive_order_response(buyer_socket);

    ASSERT_TRUE(buy_acceptance.has_value());
    EXPECT_EQ(buy_acceptance->order_id, 4002);

    const auto buyer_execution =
        receive_trade_execution(buyer_socket);

    const auto seller_execution =
        receive_trade_execution(seller_socket);

    ASSERT_TRUE(buyer_execution.has_value());
    ASSERT_TRUE(seller_execution.has_value());

    EXPECT_EQ(buyer_execution->buy_order_id, 4002);
    EXPECT_EQ(buyer_execution->sell_order_id, 4001);
    EXPECT_EQ(buyer_execution->quantity, 10);

    const auto buyer_update = receive_book_update(buyer_socket);
    const auto seller_update = receive_book_update(seller_socket);

    ASSERT_TRUE(buyer_update.has_value());
    ASSERT_TRUE(seller_update.has_value());

    EXPECT_EQ(buyer_update->has_bid, 0);
    EXPECT_EQ(buyer_update->has_ask, 0);
    EXPECT_EQ(buyer_update->bid_quantity, 0);
    EXPECT_EQ(buyer_update->ask_quantity, 0);

    EXPECT_EQ(seller_update->has_bid, 0);
    EXPECT_EQ(seller_update->has_ask, 0);

    close_client(buyer_socket);
    close_client(seller_socket);
    server.stop();
    server_thread.join();
}

}  // namespace