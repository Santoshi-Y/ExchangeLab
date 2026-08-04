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

    const std::vector<int> sockets =
        client_socket_snapshot();

    for (const int socket : sockets) {
        TcpServer::close_connection(socket);
    }
}

void ExchangeServer::handle_client(
    int client_socket
) {
    std::vector<std::byte> receive_buffer;

    /*
     * This buffer belongs only to this client thread.
     *
     * It stores up to eight trades inline and retains
     * overflow capacity after a larger sweep.
     */
    MatchingEngine::BufferedTrades trade_buffer;

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
            receive_buffer,
            trade_buffer
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
    std::vector<std::byte>& receive_buffer,
    MatchingEngine::BufferedTrades& trade_buffer
) {
    while (
        receive_buffer.size() >=
        protocol::header_size
    ) {
        const std::span<const std::byte>
            header_bytes {
                receive_buffer.data(),
                protocol::header_size
            };

        const auto header =
            protocol::decode_header(header_bytes);

        if (!header) {
            std::cerr
                << "Invalid protocol header\n";

            receive_buffer.clear();
            return;
        }

        if (header->body_size > maximum_body_size) {
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

        if (
            receive_buffer.size() <
            full_message_size
        ) {
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
            body,
            trade_buffer
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
    std::span<const std::byte> body,
    MatchingEngine::BufferedTrades& trade_buffer
) {
    switch (header.type) {
        case protocol::MessageType::NewOrder:
            handle_new_order(
                client_socket,
                header,
                body,
                trade_buffer
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
    std::span<const std::byte> body,
    MatchingEngine::BufferedTrades& trade_buffer
) {
    if (
        header.body_size !=
        protocol::new_order_body_size
    ) {
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

    if (
        request->quantity >
        static_cast<std::uint64_t>(
            std::numeric_limits<Quantity>::max()
        )
    ) {
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

    std::vector<ExecutionDelivery> deliveries;

    BookSnapshot snapshot {
        .has_bid = false,
        .best_bid = 0,
        .bid_quantity = 0,
        .has_ask = false,
        .best_ask = 0,
        .ask_quantity = 0
    };

    {
        std::lock_guard<std::mutex> lock(
            engine_mutex_
        );

        const bool duplicate_order_id =
            book_.find_order(
                request->order_id
            ) != nullptr ||
            order_owners_.contains(
                request->order_id
            );

        if (!duplicate_order_id) {
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

            engine_.process_order_into(
                book_,
                order,
                trade_buffer
            );

            accepted = true;

            deliveries.reserve(
                trade_buffer.size()
            );

            for (const Trade& trade : trade_buffer) {
                deliveries.push_back({
                    .trade = trade,
                    .buyer_socket =
                        find_order_owner(
                            trade.buy_order_id,
                            request->order_id,
                            client_socket
                        ),
                    .seller_socket =
                        find_order_owner(
                            trade.sell_order_id,
                            request->order_id,
                            client_socket
                        )
                });
            }

            remove_filled_order_owners(
                trade_buffer,
                request->order_id
            );

            if (
                book_.find_order(
                    request->order_id
                ) != nullptr
            ) {
                order_owners_[
                    request->order_id
                ] = client_socket;
            }

            snapshot = capture_book_snapshot();
        } else {
            trade_buffer.clear();
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

    if (!accepted) {
        return;
    }

    send_execution_deliveries(deliveries);
    broadcast_book_update(snapshot);
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
        .version = protocol::protocol_version,
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

    std::lock_guard<std::mutex> send_lock {
        send_mutex_
    };

    if (
        !TcpServer::send_to(
            client_socket,
            message
        )
    ) {
        std::cerr
            << "Failed to send response to socket "
            << client_socket
            << '\n';
    }
}

void ExchangeServer::send_trade_execution(
    int client_socket,
    const Trade& trade
) {
    if (client_socket < 0) {
        return;
    }

    const std::uint64_t sequence_number =
        next_sequence_number_.fetch_add(1);

    const protocol::TradeExecution execution {
        .buy_order_id = trade.buy_order_id,
        .sell_order_id = trade.sell_order_id,
        .price = static_cast<std::int64_t>(
            trade.price
        ),
        .quantity =
            static_cast<std::uint64_t>(
                trade.quantity
            ),
        .sequence_number = sequence_number
    };

    const auto response_body =
        protocol::encode_trade_execution(
            execution
        );

    const protocol::MessageHeader response_header {
        .magic = protocol::protocol_magic,
        .version = protocol::protocol_version,
        .type =
            protocol::MessageType::TradeExecution,
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

    std::lock_guard<std::mutex> send_lock {
        send_mutex_
    };

    if (
        !TcpServer::send_to(
            client_socket,
            message
        )
    ) {
        std::cerr
            << "Failed to send execution to socket "
            << client_socket
            << '\n';
    }
}

void ExchangeServer::send_execution_deliveries(
    const std::vector<ExecutionDelivery>& deliveries
) {
    for (
        const ExecutionDelivery& delivery :
        deliveries
    ) {
        send_trade_execution(
            delivery.buyer_socket,
            delivery.trade
        );

        send_trade_execution(
            delivery.seller_socket,
            delivery.trade
        );
    }
}

void ExchangeServer::broadcast_book_update(
    const BookSnapshot& snapshot
) {
    const std::uint64_t sequence_number =
        next_sequence_number_.fetch_add(1);

    const protocol::BookUpdate update {
        .has_bid = static_cast<std::uint8_t>(
            snapshot.has_bid ? 1 : 0
        ),
        .best_bid =
            snapshot.has_bid
                ? static_cast<std::int64_t>(
                    snapshot.best_bid
                )
                : 0,
        .bid_quantity =
            snapshot.has_bid
                ? static_cast<std::uint64_t>(
                    snapshot.bid_quantity
                )
                : 0,
        .has_ask = static_cast<std::uint8_t>(
            snapshot.has_ask ? 1 : 0
        ),
        .best_ask =
            snapshot.has_ask
                ? static_cast<std::int64_t>(
                    snapshot.best_ask
                )
                : 0,
        .ask_quantity =
            snapshot.has_ask
                ? static_cast<std::uint64_t>(
                    snapshot.ask_quantity
                )
                : 0,
        .sequence_number = sequence_number
    };

    const auto body =
        protocol::encode_book_update(update);

    const protocol::MessageHeader header {
        .magic = protocol::protocol_magic,
        .version = protocol::protocol_version,
        .type = protocol::MessageType::BookUpdate,
        .body_size =
            static_cast<std::uint32_t>(
                body.size()
            ),
        .sequence_number = sequence_number
    };

    const auto encoded_header =
        protocol::encode_header(header);

    const std::vector<std::byte> message =
        combine_message(
            encoded_header,
            body
        );

    const std::vector<int> clients =
        client_socket_snapshot();

    std::lock_guard<std::mutex> send_lock {
        send_mutex_
    };

    for (const int socket : clients) {
        if (
            !TcpServer::send_to(
                socket,
                message
            )
        ) {
            std::cerr
                << "Failed to send book update to socket "
                << socket
                << '\n';
        }
    }
}

ExchangeServer::BookSnapshot
ExchangeServer::capture_book_snapshot() const {
    BookSnapshot snapshot {
        .has_bid = book_.has_bids(),
        .best_bid = 0,
        .bid_quantity = 0,
        .has_ask = book_.has_asks(),
        .best_ask = 0,
        .ask_quantity = 0
    };

    if (snapshot.has_bid) {
        snapshot.best_bid = book_.best_bid();

        snapshot.bid_quantity =
            book_.best_bid_level()
                .total_quantity();
    }

    if (snapshot.has_ask) {
        snapshot.best_ask = book_.best_ask();

        snapshot.ask_quantity =
            book_.best_ask_level()
                .total_quantity();
    }

    return snapshot;
}

int ExchangeServer::find_order_owner(
    OrderId order_id,
    OrderId incoming_order_id,
    int incoming_socket
) const {
    if (order_id == incoming_order_id) {
        return incoming_socket;
    }

    const auto owner =
        order_owners_.find(order_id);

    if (owner == order_owners_.end()) {
        return -1;
    }

    return owner->second;
}

void ExchangeServer::remove_filled_order_owners(
    const MatchingEngine::BufferedTrades& trades,
    OrderId incoming_order_id
) {
    for (const Trade& trade : trades) {
        if (
            trade.buy_order_id !=
                incoming_order_id &&
            book_.find_order(
                trade.buy_order_id
            ) == nullptr
        ) {
            order_owners_.erase(
                trade.buy_order_id
            );
        }

        if (
            trade.sell_order_id !=
                incoming_order_id &&
            book_.find_order(
                trade.sell_order_id
            ) == nullptr
        ) {
            order_owners_.erase(
                trade.sell_order_id
            );
        }
    }
}

void ExchangeServer::register_client(
    int client_socket
) {
    std::lock_guard<std::mutex> lock {
        clients_mutex_
    };

    client_sockets_.push_back(client_socket);
}

void ExchangeServer::unregister_client(
    int client_socket
) {
    std::lock_guard<std::mutex> lock {
        clients_mutex_
    };

    const auto position = std::find(
        client_sockets_.begin(),
        client_sockets_.end(),
        client_socket
    );

    if (position != client_sockets_.end()) {
        client_sockets_.erase(position);
    }
}

std::vector<int>
ExchangeServer::client_socket_snapshot() {
    std::lock_guard<std::mutex> lock {
        clients_mutex_
    };

    return client_sockets_;
}

}  // namespace exchange