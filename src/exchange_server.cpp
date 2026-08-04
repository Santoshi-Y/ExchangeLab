#include "exchange/exchange_server.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "exchange/journal.hpp"
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

TimeInForce convert_time_in_force(
    protocol::TimeInForce time_in_force
) {
    switch (time_in_force) {
        case protocol::TimeInForce::GoodTillCancel:
            return TimeInForce::GoodTillCancel;

        case protocol::TimeInForce::ImmediateOrCancel:
            return TimeInForce::ImmediateOrCancel;

        case protocol::TimeInForce::FillOrKill:
            return TimeInForce::FillOrKill;
    }

    return TimeInForce::GoodTillCancel;
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

ExchangeServer::ExchangeServer(
    std::uint16_t port,
    std::optional<std::filesystem::path> journal_path
)
    : server_(port),
      running_(false),
      next_sequence_number_(1) {
    if (journal_path.has_value()) {
        journal_ =
            std::make_unique<ExchangeJournal>(
                *journal_path
            );
    }
}

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

    if (was_running) {
        server_.stop();

        const std::vector<int> sockets =
            client_socket_snapshot();

        for (const int socket : sockets) {
            TcpServer::close_connection(socket);
        }
    }

    if (journal_ != nullptr) {
        journal_->flush();
    }
}

void ExchangeServer::handle_client(
    int client_socket
) {
    std::vector<std::byte> receive_buffer;

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

        if (!header.has_value()) {
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

        if (journal_ != nullptr) {
            const std::vector<std::byte>
                complete_message(
                    receive_buffer.begin(),
                    receive_buffer.begin() +
                        static_cast<std::ptrdiff_t>(
                            full_message_size
                        )
                );

            std::lock_guard<std::mutex> journal_lock(
                journal_processing_mutex_
            );

            journal_->append(complete_message);

            handle_message(
                client_socket,
                *header,
                body,
                trade_buffer
            );
        } else {
            handle_message(
                client_socket,
                *header,
                body,
                trade_buffer
            );
        }

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
            handle_cancel_order(
                client_socket,
                header,
                body
            );
            break;

        case protocol::MessageType::ReplaceOrder:
            handle_replace_order(
                client_socket,
                header,
                body,
                trade_buffer
            );
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

    if (!request.has_value()) {
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
    std::vector<protocol::Level3OrderExecuted> level3_executions;
    std::vector<protocol::Level3OrderDeleted> level3_deletions;
    std::optional<protocol::Level3AddOrder> level3_add;

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
                    convert_time_in_force(
                        request->time_in_force
                    ),
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

            std::unordered_set<OrderId> deleted_order_ids;

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

                level3_executions.push_back({
                    .buy_order_id = trade.buy_order_id,
                    .sell_order_id = trade.sell_order_id,
                    .price = static_cast<std::int64_t>(trade.price),
                    .quantity = static_cast<std::uint64_t>(trade.quantity),
                    .sequence_number = 0
                });

                if (
                    trade.buy_order_id != request->order_id &&
                    book_.find_order(trade.buy_order_id) == nullptr
                ) {
                    deleted_order_ids.insert(trade.buy_order_id);
                }

                if (
                    trade.sell_order_id != request->order_id &&
                    book_.find_order(trade.sell_order_id) == nullptr
                ) {
                    deleted_order_ids.insert(trade.sell_order_id);
                }
            }

            for (const OrderId deleted_order_id : deleted_order_ids) {
                level3_deletions.push_back({
                    .order_id = deleted_order_id,
                    .sequence_number = 0
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

                const Order* resting_order =
                    book_.find_order(request->order_id);

                level3_add = protocol::Level3AddOrder {
                    .order_id = resting_order->id,
                    .timestamp = resting_order->timestamp,
                    .price = static_cast<std::int64_t>(
                        resting_order->price
                    ),
                    .quantity = static_cast<std::uint64_t>(
                        resting_order->remaining_quantity
                    ),
                    .side = resting_order->side == Side::Buy
                        ? protocol::Side::Buy
                        : protocol::Side::Sell,
                    .sequence_number = 0
                };
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

    for (const auto& event : level3_executions) {
        broadcast_level3_order_executed(event);
    }

    for (const auto& event : level3_deletions) {
        broadcast_level3_order_deleted(event);
    }

    if (level3_add.has_value()) {
        broadcast_level3_add_order(*level3_add);
    }
}


void ExchangeServer::handle_cancel_order(
    int client_socket,
    const protocol::MessageHeader& header,
    std::span<const std::byte> body
) {
    if (header.body_size != protocol::cancel_order_body_size) {
        send_order_response(client_socket, 0, false);
        return;
    }

    const auto request = protocol::decode_cancel_order(body);

    if (!request.has_value()) {
        send_order_response(client_socket, 0, false);
        return;
    }

    bool cancelled = false;
    BookSnapshot snapshot {
        .has_bid = false,
        .best_bid = 0,
        .bid_quantity = 0,
        .has_ask = false,
        .best_ask = 0,
        .ask_quantity = 0
    };

    {
        std::lock_guard<std::mutex> lock(engine_mutex_);

        const auto owner = order_owners_.find(request->order_id);

        if (
            owner != order_owners_.end() &&
            owner->second == client_socket &&
            book_.find_order(request->order_id) != nullptr
        ) {
            cancelled = engine_.cancel_order(
                book_,
                request->order_id
            );

            if (cancelled) {
                order_owners_.erase(owner);
                snapshot = capture_book_snapshot();
            }
        }
    }

    send_order_response(
        client_socket,
        request->order_id,
        cancelled,
        protocol::MessageType::OrderCancelled
    );

    if (!cancelled) {
        return;
    }

    broadcast_book_update(snapshot);

    broadcast_level3_order_deleted({
        .order_id = request->order_id,
        .sequence_number = 0
    });
}

void ExchangeServer::handle_replace_order(
    int client_socket,
    const protocol::MessageHeader& header,
    std::span<const std::byte> body,
    MatchingEngine::BufferedTrades& trade_buffer
) {
    if (header.body_size != protocol::replace_order_body_size) {
        send_order_response(client_socket, 0, false);
        return;
    }

    const auto request = protocol::decode_replace_order(body);

    if (!request.has_value()) {
        send_order_response(client_socket, 0, false);
        return;
    }

    if (
        request->new_quantity >
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

    const Quantity new_quantity =
        static_cast<Quantity>(request->new_quantity);

    bool replaced = false;
    std::vector<ExecutionDelivery> deliveries;
    std::vector<protocol::Level3OrderExecuted> level3_executions;
    std::vector<protocol::Level3OrderDeleted> level3_deletions;
    std::optional<protocol::Level3AddOrder> level3_add;

    BookSnapshot snapshot {
        .has_bid = false,
        .best_bid = 0,
        .bid_quantity = 0,
        .has_ask = false,
        .best_ask = 0,
        .ask_quantity = 0
    };

    {
        std::lock_guard<std::mutex> lock(engine_mutex_);

        const auto owner = order_owners_.find(request->order_id);

        if (
            owner != order_owners_.end() &&
            owner->second == client_socket &&
            book_.find_order(request->order_id) != nullptr
        ) {
            const ReplaceResult result =
                engine_.replace_order(
                    book_,
                    request->order_id,
                    request->new_price,
                    new_quantity,
                    request->timestamp
                );

            replaced = result.replaced;
            trade_buffer.clear();

            for (const Trade& trade : result.trades) {
                trade_buffer.push_back(trade);
            }

            if (replaced) {
                deliveries.reserve(trade_buffer.size());
                std::unordered_set<OrderId> deleted_order_ids;

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

                    level3_executions.push_back({
                        .buy_order_id = trade.buy_order_id,
                        .sell_order_id = trade.sell_order_id,
                        .price = static_cast<std::int64_t>(
                            trade.price
                        ),
                        .quantity = static_cast<std::uint64_t>(
                            trade.quantity
                        ),
                        .sequence_number = 0
                    });

                    if (
                        trade.buy_order_id != request->order_id &&
                        book_.find_order(trade.buy_order_id) == nullptr
                    ) {
                        deleted_order_ids.insert(
                            trade.buy_order_id
                        );
                    }

                    if (
                        trade.sell_order_id != request->order_id &&
                        book_.find_order(trade.sell_order_id) == nullptr
                    ) {
                        deleted_order_ids.insert(
                            trade.sell_order_id
                        );
                    }
                }

                for (const OrderId deleted_id : deleted_order_ids) {
                    level3_deletions.push_back({
                        .order_id = deleted_id,
                        .sequence_number = 0
                    });
                }

                remove_filled_order_owners(
                    trade_buffer,
                    request->order_id
                );

                if (
                    book_.find_order(request->order_id) != nullptr
                ) {
                    order_owners_[request->order_id] =
                        client_socket;

                    const Order* resting =
                        book_.find_order(request->order_id);

                    level3_add = protocol::Level3AddOrder {
                        .order_id = resting->id,
                        .timestamp = resting->timestamp,
                        .price = static_cast<std::int64_t>(
                            resting->price
                        ),
                        .quantity = static_cast<std::uint64_t>(
                            resting->remaining_quantity
                        ),
                        .side = resting->side == Side::Buy
                            ? protocol::Side::Buy
                            : protocol::Side::Sell,
                        .sequence_number = 0
                    };
                } else {
                    order_owners_.erase(request->order_id);
                }

                snapshot = capture_book_snapshot();
            }
        }
    }

    send_order_response(
        client_socket,
        request->order_id,
        replaced,
        protocol::MessageType::OrderReplaced
    );

    if (!replaced) {
        return;
    }

    send_execution_deliveries(deliveries);
    broadcast_book_update(snapshot);

    // A replacement removes the old resting representation first.
    broadcast_level3_order_deleted({
        .order_id = request->order_id,
        .sequence_number = 0
    });

    for (const auto& event : level3_executions) {
        broadcast_level3_order_executed(event);
    }

    for (const auto& event : level3_deletions) {
        broadcast_level3_order_deleted(event);
    }

    if (level3_add.has_value()) {
        broadcast_level3_add_order(*level3_add);
    }
}

void ExchangeServer::send_order_response(
    int client_socket,
    std::uint64_t order_id,
    bool success,
    protocol::MessageType success_type
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
                ? success_type
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

    std::lock_guard<std::mutex> send_lock(
        send_mutex_
    );

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

    std::lock_guard<std::mutex> send_lock(
        send_mutex_
    );

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
        .type =
            protocol::MessageType::BookUpdate,
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

    std::lock_guard<std::mutex> send_lock(
        send_mutex_
    );

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

void ExchangeServer::broadcast_level3_add_order(
    const protocol::Level3AddOrder& event
) {
    protocol::Level3AddOrder sequenced_event = event;
    sequenced_event.sequence_number =
        next_sequence_number_.fetch_add(1);

    const auto body =
        protocol::encode_level3_add_order(sequenced_event);

    const protocol::MessageHeader header {
        .magic = protocol::protocol_magic,
        .version = protocol::protocol_version,
        .type = protocol::MessageType::Level3AddOrder,
        .body_size = static_cast<std::uint32_t>(body.size()),
        .sequence_number = sequenced_event.sequence_number
    };

    const auto encoded_header = protocol::encode_header(header);
    const std::vector<std::byte> message =
        combine_message(encoded_header, body);
    const std::vector<int> clients = client_socket_snapshot();

    std::lock_guard<std::mutex> send_lock(send_mutex_);
    for (const int socket : clients) {
        if (!TcpServer::send_to(socket, message)) {
            std::cerr << "Failed to send Level-3 add event to socket "
                      << socket << '\n';
        }
    }
}

void ExchangeServer::broadcast_level3_order_executed(
    const protocol::Level3OrderExecuted& event
) {
    protocol::Level3OrderExecuted sequenced_event = event;
    sequenced_event.sequence_number =
        next_sequence_number_.fetch_add(1);

    const auto body =
        protocol::encode_level3_order_executed(sequenced_event);

    const protocol::MessageHeader header {
        .magic = protocol::protocol_magic,
        .version = protocol::protocol_version,
        .type = protocol::MessageType::Level3OrderExecuted,
        .body_size = static_cast<std::uint32_t>(body.size()),
        .sequence_number = sequenced_event.sequence_number
    };

    const auto encoded_header = protocol::encode_header(header);
    const std::vector<std::byte> message =
        combine_message(encoded_header, body);
    const std::vector<int> clients = client_socket_snapshot();

    std::lock_guard<std::mutex> send_lock(send_mutex_);
    for (const int socket : clients) {
        if (!TcpServer::send_to(socket, message)) {
            std::cerr << "Failed to send Level-3 execution event to socket "
                      << socket << '\n';
        }
    }
}

void ExchangeServer::broadcast_level3_order_deleted(
    const protocol::Level3OrderDeleted& event
) {
    protocol::Level3OrderDeleted sequenced_event = event;
    sequenced_event.sequence_number =
        next_sequence_number_.fetch_add(1);

    const auto body =
        protocol::encode_level3_order_deleted(sequenced_event);

    const protocol::MessageHeader header {
        .magic = protocol::protocol_magic,
        .version = protocol::protocol_version,
        .type = protocol::MessageType::Level3OrderDeleted,
        .body_size = static_cast<std::uint32_t>(body.size()),
        .sequence_number = sequenced_event.sequence_number
    };

    const auto encoded_header = protocol::encode_header(header);
    const std::vector<std::byte> message =
        combine_message(encoded_header, body);
    const std::vector<int> clients = client_socket_snapshot();

    std::lock_guard<std::mutex> send_lock(send_mutex_);
    for (const int socket : clients) {
        if (!TcpServer::send_to(socket, message)) {
            std::cerr << "Failed to send Level-3 delete event to socket "
                      << socket << '\n';
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
        snapshot.best_bid =
            book_.best_bid();

        snapshot.bid_quantity =
            book_.best_bid_level()
                .total_quantity();
    }

    if (snapshot.has_ask) {
        snapshot.best_ask =
            book_.best_ask();

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

std::vector<int>
ExchangeServer::client_socket_snapshot() {
    std::lock_guard<std::mutex> lock(
        clients_mutex_
    );

    return client_sockets_;
}

}  // namespace exchange