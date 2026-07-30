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

constexpr std::size_t maximum_body_size = 1024 * 1024;

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

}  // namespace

ExchangeServer::ExchangeServer(std::uint16_t port)
    : server_(port),
      next_sequence_number_(1) {}

bool ExchangeServer::start() {
    return server_.start();
}

void ExchangeServer::run() {
    if (!server_.accept_client()) {
        std::cerr << "Failed to accept client\n";
        return;
    }

    while (true) {
        const std::vector<std::byte> bytes =
            server_.receive();

        if (bytes.empty()) {
            break;
        }

        receive_buffer_.insert(
            receive_buffer_.end(),
            bytes.begin(),
            bytes.end()
        );

        process_receive_buffer();
    }
}

void ExchangeServer::stop() {
    server_.stop();
}

void ExchangeServer::process_receive_buffer() {
    while (receive_buffer_.size() >=
           protocol::header_size) {

        const std::span<const std::byte> header_bytes {
            receive_buffer_.data(),
            protocol::header_size
        };

        const auto header =
            protocol::decode_header(header_bytes);

        if (!header) {
            std::cerr << "Invalid protocol header\n";
            receive_buffer_.clear();
            return;
        }

        if (header->body_size > maximum_body_size) {
            std::cerr << "Message body is too large\n";
            receive_buffer_.clear();
            return;
        }

        const std::size_t full_message_size =
            protocol::header_size +
            static_cast<std::size_t>(header->body_size);

        if (receive_buffer_.size() <
            full_message_size) {

            return;
        }

        const std::span<const std::byte> body {
            receive_buffer_.data() +
                protocol::header_size,
            static_cast<std::size_t>(
                header->body_size
            )
        };

        handle_message(*header, body);

        receive_buffer_.erase(
            receive_buffer_.begin(),
            receive_buffer_.begin() +
                static_cast<std::ptrdiff_t>(
                    full_message_size
                )
        );
    }
}

void ExchangeServer::handle_message(
    const protocol::MessageHeader& header,
    std::span<const std::byte> body
) {
    switch (header.type) {
        case protocol::MessageType::NewOrder:
            handle_new_order(header, body);
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
    const protocol::MessageHeader& header,
    std::span<const std::byte> body
) {
    if (header.body_size !=
        protocol::new_order_body_size) {

        std::cerr
            << "Invalid new-order body size\n";

        send_order_response(0, false);
        return;
    }

    const auto request =
        protocol::decode_new_order(body);

    if (!request) {
        std::cerr
            << "Could not decode new order\n";

        send_order_response(0, false);
        return;
    }

    if (book_.find_order(request->order_id) != nullptr) {
        std::cerr
            << "Duplicate order ID: "
            << request->order_id
            << '\n';

        send_order_response(
            request->order_id,
            false
        );

        return;
    }

    if (request->quantity >
        static_cast<std::uint64_t>(
            std::numeric_limits<Quantity>::max()
        )) {

        send_order_response(
            request->order_id,
            false
        );

        return;
    }

    const Quantity quantity =
        static_cast<Quantity>(request->quantity);

    const Order order {
        .id = request->order_id,
        .side = convert_side(request->side),
        .type =
            convert_order_type(request->order_type),
        .time_in_force =
            TimeInForce::GoodTillCancel,
        .price = request->price,
        .initial_quantity = quantity,
        .remaining_quantity = quantity,
        .timestamp = request->timestamp
    };

    engine_.process_order(book_, order);

    std::cout
        << "Accepted order "
        << request->order_id
        << ": "
        << (request->side == protocol::Side::Buy
                ? "BUY "
                : "SELL ")
        << request->quantity;

    if (request->order_type ==
        protocol::OrderType::Limit) {

        std::cout << " @ " << request->price;
    } else {
        std::cout << " MARKET";
    }

    std::cout << '\n';

    send_order_response(
        request->order_id,
        true
    );
}

void ExchangeServer::send_order_response(
    std::uint64_t order_id,
    bool success
) {
    const std::uint64_t sequence_number =
        next_sequence_number_++;

    const protocol::OrderResponse response {
        .order_id = order_id,
        .sequence_number = sequence_number,
        .success =
            static_cast<std::uint8_t>(
                success ? 1 : 0
            )
    };

    const auto response_body =
        protocol::encode_order_response(response);

    const protocol::MessageHeader response_header {
        .magic = protocol::protocol_magic,
        .version = protocol::protocol_version,
        .type =
            success
                ? protocol::MessageType::OrderAccepted
                : protocol::MessageType::OrderRejected,
        .body_size = static_cast<std::uint32_t>(
            response_body.size()
        ),
        .sequence_number = sequence_number
    };

    const auto encoded_header =
        protocol::encode_header(response_header);

    const std::vector<std::byte> message =
        combine_message(
            encoded_header,
            response_body
        );

    if (!server_.send(message)) {
        std::cerr
            << "Failed to send order response\n";
    }
}

}  // namespace exchange