#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "exchange/protocol.hpp"

namespace {

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
bool send_request(
    int socket,
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

    return send_all(
        socket,
        combine_message(
            exchange::protocol::encode_header(header),
            body
        )
    );
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

    if (
        ::inet_pton(
            AF_INET,
            "127.0.0.1",
            &address.sin_addr
        ) != 1
    ) {
        ::close(client_socket);
        return -1;
    }

    if (
        ::connect(
            client_socket,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) != 0
    ) {
        ::close(client_socket);
        return -1;
    }

    return client_socket;
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

void print_message(const ReceivedMessage& message) {
    using exchange::protocol::MessageType;

    switch (message.header.type) {
        case MessageType::OrderAccepted:
        case MessageType::OrderRejected:
        case MessageType::OrderCancelled:
        case MessageType::OrderReplaced: {
            const auto response =
                exchange::protocol::decode_order_response(
                    message.body
                );

            if (!response.has_value()) {
                std::cout << "Invalid order response\n";
                return;
            }

            std::cout
                << "ORDER RESPONSE type="
                << static_cast<std::uint16_t>(
                    message.header.type
                )
                << " id=" << response->order_id
                << " success="
                << static_cast<int>(response->success)
                << '\n';
            break;
        }

        case MessageType::BookUpdate: {
            const auto update =
                exchange::protocol::decode_book_update(
                    message.body
                );

            if (!update.has_value()) {
                std::cout << "Invalid book update\n";
                return;
            }

            std::cout << "BOOK ";

            if (update->has_bid != 0) {
                std::cout
                    << "bid=" << update->best_bid
                    << " x " << update->bid_quantity
                    << ' ';
            } else {
                std::cout << "bid=none ";
            }

            if (update->has_ask != 0) {
                std::cout
                    << "ask=" << update->best_ask
                    << " x " << update->ask_quantity;
            } else {
                std::cout << "ask=none";
            }

            std::cout << '\n';
            break;
        }

        case MessageType::Level3AddOrder: {
            const auto event =
                exchange::protocol::decode_level3_add_order(
                    message.body
                );

            if (!event.has_value()) {
                std::cout << "Invalid Level-3 add event\n";
                return;
            }

            std::cout
                << "L3 ADD id=" << event->order_id
                << ' '
                << (
                    event->side ==
                            exchange::protocol::Side::Buy
                        ? "BUY "
                        : "SELL "
                )
                << event->price
                << " x " << event->quantity
                << '\n';
            break;
        }

        case MessageType::Level3OrderExecuted: {
            const auto event =
                exchange::protocol::
                    decode_level3_order_executed(
                        message.body
                    );

            if (!event.has_value()) {
                std::cout
                    << "Invalid Level-3 execution event\n";
                return;
            }

            std::cout
                << "L3 EXEC buy=" << event->buy_order_id
                << " sell=" << event->sell_order_id
                << " price=" << event->price
                << " quantity=" << event->quantity
                << '\n';
            break;
        }

        case MessageType::Level3OrderDeleted: {
            const auto event =
                exchange::protocol::
                    decode_level3_order_deleted(
                        message.body
                    );

            if (!event.has_value()) {
                std::cout
                    << "Invalid Level-3 delete event\n";
                return;
            }

            std::cout
                << "L3 DELETE id="
                << event->order_id
                << '\n';
            break;
        }

        default:
            std::cout
                << "Received message type "
                << static_cast<std::uint16_t>(
                    message.header.type
                )
                << '\n';
            break;
    }
}

bool receive_and_print(
    int socket,
    int message_count
) {
    for (int index = 0; index < message_count; ++index) {
        const auto message = receive_message(socket);

        if (!message.has_value()) {
            return false;
        }

        print_message(*message);
    }

    return true;
}

}  // namespace

int main() {
    constexpr std::uint16_t port = 9000;

    const int client_socket = connect_to_server(port);

    if (client_socket < 0) {
        std::cerr
            << "Could not connect to server on port "
            << port
            << '\n';
        return 1;
    }

    const exchange::protocol::NewOrderRequest new_order {
        .order_id = 1,
        .timestamp = 1,
        .price = 105,
        .quantity = 10,
        .side = exchange::protocol::Side::Sell,
        .order_type =
            exchange::protocol::OrderType::Limit,
        .time_in_force =
            exchange::protocol::TimeInForce::GoodTillCancel
    };

    if (
        !send_request(
            client_socket,
            exchange::protocol::MessageType::NewOrder,
            exchange::protocol::encode_new_order(new_order),
            1
        ) ||
        !receive_and_print(client_socket, 3)
    ) {
        std::cerr << "New-order lifecycle failed\n";
        ::close(client_socket);
        return 1;
    }

    const exchange::protocol::ReplaceOrderRequest replace {
        .order_id = 1,
        .timestamp = 2,
        .new_price = 106,
        .new_quantity = 12
    };

    if (
        !send_request(
            client_socket,
            exchange::protocol::MessageType::ReplaceOrder,
            exchange::protocol::encode_replace_order(replace),
            2
        ) ||
        !receive_and_print(client_socket, 4)
    ) {
        std::cerr << "Replace lifecycle failed\n";
        ::close(client_socket);
        return 1;
    }

    const exchange::protocol::CancelOrderRequest cancel {
        .order_id = 1,
        .timestamp = 3
    };

    if (
        !send_request(
            client_socket,
            exchange::protocol::MessageType::CancelOrder,
            exchange::protocol::encode_cancel_order(cancel),
            3
        ) ||
        !receive_and_print(client_socket, 3)
    ) {
        std::cerr << "Cancel lifecycle failed\n";
        ::close(client_socket);
        return 1;
    }

    ::shutdown(client_socket, SHUT_RDWR);
    ::close(client_socket);

    return 0;
}