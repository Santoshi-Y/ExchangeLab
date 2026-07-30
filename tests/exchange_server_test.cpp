#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
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
    const int client_socket =
        ::socket(AF_INET, SOCK_STREAM, 0);

    if (client_socket < 0) {
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

        ::close(client_socket);
        return -1;
    }

    if (::connect(
            client_socket,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) != 0) {

        ::close(client_socket);
        return -1;
    }

    return client_socket;
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

TEST(
    ExchangeServerTest,
    AcceptsBinaryNewOrder
) {
    constexpr std::uint16_t port = 19001;

    exchange::ExchangeServer server(port);

    ASSERT_TRUE(server.start());

    std::thread server_thread([&server]() {
        server.run();
    });

    const int client_socket =
        connect_to_server(port);

    ASSERT_GE(client_socket, 0);

    const exchange::protocol::NewOrderRequest request {
        .order_id = 1001,
        .timestamp = 500,
        .price = 105,
        .quantity = 20,
        .side = exchange::protocol::Side::Buy,
        .order_type =
            exchange::protocol::OrderType::Limit
    };

    const auto request_body =
        exchange::protocol::encode_new_order(request);

    const exchange::protocol::MessageHeader request_header {
        .magic =
            exchange::protocol::protocol_magic,
        .version =
            exchange::protocol::protocol_version,
        .type =
            exchange::protocol::MessageType::NewOrder,
        .body_size = static_cast<std::uint32_t>(
            request_body.size()
        ),
        .sequence_number = 1
    };

    const auto encoded_header =
        exchange::protocol::encode_header(
            request_header
        );

    const std::vector<std::byte> request_message =
        combine_message(
            encoded_header,
            request_body
        );

    ASSERT_TRUE(
        send_all(client_socket, request_message)
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
        exchange::protocol::MessageType::OrderAccepted
    );

    EXPECT_EQ(
        response_header->body_size,
        exchange::protocol::order_response_body_size
    );

    std::array<
        std::byte,
        exchange::protocol::order_response_body_size
    > response_body_bytes {};

    ASSERT_TRUE(
        receive_exact(
            client_socket,
            response_body_bytes
        )
    );

    const auto response =
        exchange::protocol::decode_order_response(
            response_body_bytes
        );

    ASSERT_TRUE(response.has_value());

    EXPECT_EQ(response->order_id, 1001);
    EXPECT_EQ(response->success, 1);

    ::shutdown(client_socket, SHUT_RDWR);
    ::close(client_socket);

    server_thread.join();
    server.stop();
}

}  // namespace