#include "exchange/exchange_server.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

#include "exchange/order.hpp"
#include "exchange/protocol.hpp"
#include "exchange/types.hpp"

namespace exchange {

namespace {

constexpr std::size_t maximum_body_size =
    1024U * 1024U;

Side convert_side(protocol::Side side) {
    switch (side) {
        case protocol::Side::Buy:
            return Side::Buy;

        case protocol::Side::Sell:
            return Side::Sell;
    }

    return Side::Buy;
}

OrderType convert_order_type(
    protocol::OrderType order_type
) {
    switch (order_type) {
        case protocol::OrderType::Limit:
            return OrderType::Limit;

        case protocol::OrderType::Market:
            return OrderType::Market;
    }

    return OrderType::Limit;
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

}  // namespace

ExchangeServer::ExchangeServer(std::uint16_t port)
    : server_(port),
      running_(false),
      next_sequence_number_(1) {}

ExchangeServer::~ExchangeServer() {
    stop();
}

bool ExchangeServer::start() {
    if (!server_.start()) {
        return false;
    }

    running_.store(true);
    return true;
}

void ExchangeServer::run() {
    while (running_.load()) {
        const int client_socket =
            server_.accept_connection();

        if (client_socket < 0) {
            if (running_.load()) {
                std::cerr
                    << "Failed to accept client\n";
            }

            break;
        }

        register_client(client_socket);

        std::cout
            << "Client connected on socket "
            << client_socket
            << '\n';

        std::lock_guard<std::mutex> lock(
            threads_mutex_
        );

        client_threads_.emplace_back(
            &ExchangeServer::handle_client,
            this,
            client_socket
        );
    }

    std::vector<std::thread> threads;

    {
        std::lock_guard<std::mutex> lock(
            threads_mutex_
        );

        threads.swap(client_threads_);
    }

    for (std::thread& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void ExchangeServer::stop() {
    const bool was_running =
        running_.exchange(false);

    if (!was_running) {
        return;
    }

    server_.stop();

    std::vector<int> sockets;

    {
        std::lock_guard<std::mutex> lock(
            clients_mutex_
        );

        sockets = client_sockets_;
    }

    for (const int socket : sockets) {
        TcpServer::close_connection(socket);
    }
}

void ExchangeServer::handle_client(
    int client_socket
) {
    std::vector<std::byte> receive_buffer;

    while (running_.load()) {
        const std::vector<std::byte> bytes =
            TcpServer::receive_from(client_socket);

        if (bytes.empty()) {
            break;
        }

        receive_buffer.insert(
            receive_buffer.end(),
            bytes.begin(),
            bytes.end()
        );

        process_receive_buffer(
            client_socket,
            receive_buffer
        );
    }

    unregister_client(client_socket);
    TcpServer::close_connection(client_socket);

    std::cout
        << "Client disconnected from socket "
        << client_socket
        << '\n';
}

void ExchangeServer::process_receive_buffer(
    int client_socket,
    std::vector<std::byte>& receive_buffer
) {
    while (receive_buffer.size() >=
           protocol::header_size) {

        const std::span<const std::byte>
            header_bytes {
                receive_buffer.data(),
                protocol::header_size
            };

        const auto header =
            protocol::decode_header(
                header_bytes
            );

        if (!header) {
            std::cerr
                << "Invalid protocol header\n";

            receive_buffer.clear();
            return;
        }

        if (header->body_size >
            maximum_body_size) {

            std::cerr
                << "Message body is too large\n";

            receive_buffer.clear();
            return;
        }

        const std::size_t full_message_size =
            protocol::header_size +
            static_cast<std::size_t>(
                header->body_size
            );

        if (receive_buffer.size() <
            full_message_size) {

            return;
        }

        const std::span<const std::byte> body {
            receive_buffer.data() +
                protocol::header_size,
            static_cast<std::size_t>(
                header->body_size
            )
        };

        handle_message(
            client_socket,
            *header,
            body
        );

        receive_buffer.erase(
            receive_buffer.begin(),
            receive_buffer.begin() +
                static_cast<std::ptrdiff_t>(
                    full_message_size
                )
        );
    }
}

void ExchangeServer::handle_message(
    int client_socket,
    const protocol::MessageHeader& header,
    std::span<const std::byte> body
) {
    switch (header.type) {
        case protocol::MessageType::NewOrder:
            handle_new_order(
                client_socket,
                header,
                body
            );
            break;

        case protocol::MessageType::CancelOrder:
        case protocol::MessageType::ReplaceOrder:
            std::cerr
                << "Message type is not implemented yet\n";
            break;

        default:
            std::cerr
                << "Unsupported message type\n";
            break;
    }
}

void ExchangeServer::handle_new_order(
    int client_socket,
    const protocol::MessageHeader& header,
    std::span<const std::byte> body
) {
    if (header.body_size !=
        protocol::new_order_body_size) {

        send_order_response(
            client_socket,
            0,
            false
        );

        return;
    }

    const auto request =
        protocol::decode_new_order(body);

    if (!request) {
        send_order_response(
            client_socket,
            0,
            false
        );

        return;
    }

    if (request->quantity >
        static_cast<std::uint64_t>(
            std::numeric_limits<Quantity>::max()
        )) {

        send_order_response(
            client_socket,
            request->order_id,
            false
        );

        return;
    }

    const Quantity quantity =
        static_cast<Quantity>(
            request->quantity
        );

    bool accepted = false;

    {
        // Only one thread may modify the shared
        // order book and matching engine at a time.
        std::lock_guard<std::mutex> lock(
            engine_mutex_
        );

        if (book_.find_order(
                request->order_id
            ) == nullptr) {

            const Order order {
                .id = request->order_id,
                .side =
                    convert_side(request->side),
                .type =
                    convert_order_type(
                        request->order_type
                    ),
                .time_in_force =
                    TimeInForce::GoodTillCancel,
                .price = request->price,
                .initial_quantity = quantity,
                .remaining_quantity = quantity,
                .timestamp = request->timestamp
            };

            engine_.process_order(book_, order);
            accepted = true;
        }
    }

    if (accepted) {
        std::cout
            << "Accepted order "
            << request->order_id
            << " from socket "
            << client_socket
            << '\n';
    } else {
        std::cerr
            << "Rejected duplicate order ID "
            << request->order_id
            << '\n';
    }

    send_order_response(
        client_socket,
        request->order_id,
        accepted
    );
}

void ExchangeServer::send_order_response(
    int client_socket,
    std::uint64_t order_id,
    bool success
) {
    const std::uint64_t sequence_number =
        next_sequence_number_.fetch_add(1);

    const protocol::OrderResponse response {
        .order_id = order_id,
        .sequence_number = sequence_number,
        .success = static_cast<std::uint8_t>(
            success ? 1 : 0
        )
    };

    const auto response_body =
        protocol::encode_order_response(
            response
        );

    const protocol::MessageHeader response_header {
        .magic = protocol::protocol_magic,
        .version =
            protocol::protocol_version,
        .type =
            success
                ? protocol::MessageType::
                    OrderAccepted
                : protocol::MessageType::
                    OrderRejected,
        .body_size =
            static_cast<std::uint32_t>(
                response_body.size()
            ),
        .sequence_number = sequence_number
    };

    const auto encoded_header =
        protocol::encode_header(
            response_header
        );

    const std::vector<std::byte> message =
        combine_message(
            encoded_header,
            response_body
        );

    if (!TcpServer::send_to(
            client_socket,
            message
        )) {

        std::cerr
            << "Failed to send response to socket "
            << client_socket
            << '\n';
    }
}

void ExchangeServer::register_client(
    int client_socket
) {
    std::lock_guard<std::mutex> lock(
        clients_mutex_
    );

    client_sockets_.push_back(client_socket);
}

void ExchangeServer::unregister_client(
    int client_socket
) {
    std::lock_guard<std::mutex> lock(
        clients_mutex_
    );

    const auto position = std::find(
        client_sockets_.begin(),
        client_sockets_.end(),
        client_socket
    );

    if (position != client_sockets_.end()) {
        client_sockets_.erase(position);
    }
}

}  // namespace exchange