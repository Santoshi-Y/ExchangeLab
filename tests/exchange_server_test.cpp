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
#include <unistd.h>

#include "exchange/exchange_server.hpp"
#include "exchange/protocol.hpp"

namespace {

using namespace std::chrono_literals;

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

        total_sent +=
            static_cast<std::size_t>(sent);
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
    for (int attempt = 0;
         attempt < 50;
         ++attempt) {

        const int socket =
            ::socket(AF_INET, SOCK_STREAM, 0);

        if (socket < 0) {
            return -1;
        }

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);

        if (::inet_pton(
                AF_INET,
                "127.0.0.1",
                &address.sin_addr
            ) != 1) {

            ::close(socket);
            return -1;
        }

        if (::connect(
                socket,
                reinterpret_cast<sockaddr*>(
                    &address
                ),
                sizeof(address)
            ) == 0) {

            return socket;
        }

        ::close(socket);
        std::this_thread::sleep_for(10ms);
    }

    return -1;
}

template <
    std::size_t HeaderSize,
    std::size_t BodySize
>
std::vector<std::byte> combine_message(
    const std::array<std::byte, HeaderSize>& header,
    const std::array<std::byte, BodySize>& body
) {
    std::vector<std::byte> message;

    message.reserve(
        HeaderSize + BodySize
    );

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
        exchange::protocol::encode_new_order(
            request
        );

    const exchange::protocol::MessageHeader header {
        .magic =
            exchange::protocol::protocol_magic,
        .version =
            exchange::protocol::protocol_version,
        .type =
            exchange::protocol::MessageType::NewOrder,
        .body_size =
            static_cast<std::uint32_t>(
                body.size()
            ),
        .sequence_number = sequence_number
    };

    const auto encoded_header =
        exchange::protocol::encode_header(header);

    const std::vector<std::byte> message =
        combine_message(
            encoded_header,
            body
        );

    return send_all(socket, message);
}

std::optional<exchange::protocol::OrderResponse>
receive_order_response(int socket) {
    std::array<
        std::byte,
        exchange::protocol::header_size
    > header_bytes {};

    if (!receive_exact(socket, header_bytes)) {
        return std::nullopt;
    }

    const auto header =
        exchange::protocol::decode_header(
            header_bytes
        );

    if (!header) {
        return std::nullopt;
    }

    if (header->body_size !=
        exchange::protocol::
            order_response_body_size) {

        return std::nullopt;
    }

    std::array<
        std::byte,
        exchange::protocol::
            order_response_body_size
    > body_bytes {};

    if (!receive_exact(socket, body_bytes)) {
        return std::nullopt;
    }

    return exchange::protocol::
        decode_order_response(body_bytes);
}

void close_client(int socket) {
    if (socket >= 0) {
        ::shutdown(socket, SHUT_RDWR);
        ::close(socket);
    }
}

TEST(
    ExchangeServerTest,
    AcceptsOrdersFromTwoClients
) {
    constexpr std::uint16_t port = 19001;

    exchange::ExchangeServer server(port);

    ASSERT_TRUE(server.start());

    std::thread server_thread([&server]() {
        server.run();
    });

    const int buyer_socket =
        connect_to_server(port);

    const int seller_socket =
        connect_to_server(port);

    ASSERT_GE(buyer_socket, 0);
    ASSERT_GE(seller_socket, 0);

    const exchange::protocol::NewOrderRequest
        sell_order {
            .order_id = 2001,
            .timestamp = 1,
            .price = 100,
            .quantity = 10,
            .side =
                exchange::protocol::Side::Sell,
            .order_type =
                exchange::protocol::OrderType::Limit
        };

    const exchange::protocol::NewOrderRequest
        buy_order {
            .order_id = 2002,
            .timestamp = 2,
            .price = 100,
            .quantity = 10,
            .side =
                exchange::protocol::Side::Buy,
            .order_type =
                exchange::protocol::OrderType::Limit
        };

    ASSERT_TRUE(
        send_order(
            seller_socket,
            sell_order,
            1
        )
    );

    const auto seller_response =
        receive_order_response(seller_socket);

    ASSERT_TRUE(seller_response.has_value());

    EXPECT_EQ(
        seller_response->order_id,
        2001
    );

    EXPECT_EQ(
        seller_response->success,
        1
    );

    ASSERT_TRUE(
        send_order(
            buyer_socket,
            buy_order,
            2
        )
    );

    const auto buyer_response =
        receive_order_response(buyer_socket);

    ASSERT_TRUE(buyer_response.has_value());

    EXPECT_EQ(
        buyer_response->order_id,
        2002
    );

    EXPECT_EQ(
        buyer_response->success,
        1
    );

    close_client(buyer_socket);
    close_client(seller_socket);

    server.stop();
    server_thread.join();
}

TEST(
    ExchangeServerTest,
    RoutesResponsesToCorrectClient
) {
    constexpr std::uint16_t port = 19002;

    exchange::ExchangeServer server(port);

    ASSERT_TRUE(server.start());

    std::thread server_thread([&server]() {
        server.run();
    });

    const int first_socket =
        connect_to_server(port);

    const int second_socket =
        connect_to_server(port);

    ASSERT_GE(first_socket, 0);
    ASSERT_GE(second_socket, 0);

    const exchange::protocol::NewOrderRequest
        first_order {
            .order_id = 3001,
            .timestamp = 1,
            .price = 99,
            .quantity = 5,
            .side =
                exchange::protocol::Side::Buy,
            .order_type =
                exchange::protocol::OrderType::Limit
        };

    const exchange::protocol::NewOrderRequest
        second_order {
            .order_id = 3002,
            .timestamp = 2,
            .price = 101,
            .quantity = 5,
            .side =
                exchange::protocol::Side::Sell,
            .order_type =
                exchange::protocol::OrderType::Limit
        };

    ASSERT_TRUE(
        send_order(
            first_socket,
            first_order,
            1
        )
    );

    ASSERT_TRUE(
        send_order(
            second_socket,
            second_order,
            2
        )
    );

    const auto first_response =
        receive_order_response(first_socket);

    const auto second_response =
        receive_order_response(second_socket);

    ASSERT_TRUE(first_response.has_value());
    ASSERT_TRUE(second_response.has_value());

    EXPECT_EQ(
        first_response->order_id,
        3001
    );

    EXPECT_EQ(
        second_response->order_id,
        3002
    );

    close_client(first_socket);
    close_client(second_socket);

    server.stop();
    server_thread.join();
}

}  // namespace