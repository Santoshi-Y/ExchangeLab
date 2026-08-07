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
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "exchange/journal.hpp"
#include "exchange/multicast_publisher.hpp"
#include "exchange/order.hpp"
#include "exchange/protocol.hpp"
#include "exchange/replay.hpp"
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
    std::optional<std::filesystem::path> journal_path,
    std::optional<MulticastConfig> multicast_config,
    RiskLimits risk_limits
)
    : server_(port),
      risk_engine_(std::move(risk_limits)),
      journal_path_(std::move(journal_path)),
      multicast_config_(std::move(multicast_config)),
      running_(false),
      next_sequence_number_(1),
      next_risk_client_id_(1) {}

ExchangeServer::~ExchangeServer() {
    stop();
}

bool ExchangeServer::recover_from_journal() {
    recovery_state_ = RecoveryState {};

    if (!journal_path_.has_value()) {
        recovery_state_.successful = true;
        return true;
    }

    recovery_state_.attempted = true;

    std::error_code error;

    const bool journal_exists =
        std::filesystem::exists(
            *journal_path_,
            error
        );

    if (error) {
        std::cerr
            << "Could not inspect journal: "
            << error.message()
            << '\n';

        return false;
    }

    if (!journal_exists) {
        recovery_state_.successful = true;
        return true;
    }

    const std::uintmax_t journal_size =
        std::filesystem::file_size(
            *journal_path_,
            error
        );

    if (error) {
        std::cerr
            << "Could not read journal size: "
            << error.message()
            << '\n';

        return false;
    }

    if (journal_size == 0) {
        recovery_state_.successful = true;
        return true;
    }

    ExchangeReplayer replayer;

    if (!replayer.replay(*journal_path_)) {
        std::cerr
            << "Failed to replay journal: "
            << *journal_path_
            << '\n';

        return false;
    }

    const ReplaySummary replay_summary =
        replayer.summary();

    ExchangeReplayer::RecoveredBooks recovered_books =
        replayer.release_order_books();

    std::size_t remaining_orders = 0;

    for (auto& [symbol, recovered_book] : recovered_books) {
        if (recovered_book == nullptr) {
            continue;
        }

        InstrumentState& instrument =
            instrument_for(symbol);

        instrument.book.swap(*recovered_book);

        remaining_orders +=
            instrument.book.order_count();
    }

    recovery_state_.successful = true;
    recovery_state_.journal_records =
        replay_summary.journal_records;
    recovery_state_.new_orders =
        replay_summary.new_orders;
    recovery_state_.trades =
        replay_summary.trades;
    recovery_state_.rejected_messages =
        replay_summary.rejected_messages;
    recovery_state_.unsupported_messages =
        replay_summary.unsupported_messages;
    recovery_state_.remaining_orders =
        remaining_orders;
    recovery_state_.instruments =
        instruments_.size();

    return true;
}

bool ExchangeServer::start() {
    if (!recovery_completed_) {
        if (!recover_from_journal()) {
            return false;
        }

        recovery_completed_ = true;
    }

    /*
     * Open the append-only writer only after replay has
     * finished. Recovered messages are therefore never
     * written to the journal a second time.
     */
    if (
        journal_path_.has_value() &&
        journal_ == nullptr
    ) {
        try {
            journal_ =
                std::make_unique<ExchangeJournal>(
                    *journal_path_
                );
        } catch (const std::exception& exception) {
            std::cerr
                << "Failed to open journal: "
                << exception.what()
                << '\n';

            return false;
        }
    }

    if (
        multicast_config_.has_value() &&
        multicast_publisher_ == nullptr
    ) {
        multicast_publisher_ =
            std::make_unique<MulticastPublisher>(
                *multicast_config_
            );

        if (!multicast_publisher_->start()) {
            std::cerr
                << "Failed to start market-data multicast publisher\n";

            multicast_publisher_.reset();
            return false;
        }
    }

    if (!server_.start()) {
        if (multicast_publisher_ != nullptr) {
            multicast_publisher_->stop();
            multicast_publisher_.reset();
        }

        return false;
    }

    running_.store(true);
    return true;
}

const RecoveryState&
ExchangeServer::recovery_state() const noexcept {
    return recovery_state_;
}

const RiskLimits&
ExchangeServer::risk_limits() const noexcept {
    return risk_engine_.limits();
}

void ExchangeServer::set_global_kill_switch(
    bool enabled
) {
    std::lock_guard<std::mutex> lock(
        engine_mutex_
    );

    risk_engine_.set_global_kill_switch(enabled);
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

        const RiskClientId risk_client_id =
            register_client(client_socket);

        std::cout
            << "Client connected on socket "
            << client_socket
            << " (risk session "
            << risk_client_id
            << ")\n";

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

    if (multicast_publisher_ != nullptr) {
        multicast_publisher_->stop();
        multicast_publisher_.reset();
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
            /*
             * Serialize processing for journaled sessions so accepted
             * commands are written in exactly the same order in which
             * they mutate the live engine. Individual handlers append
             * only after validation and risk approval, which prevents a
             * risk-rejected order from reappearing during recovery.
             */
            std::lock_guard<std::mutex> journal_lock(
                journal_processing_mutex_
            );

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

    const std::string symbol =
        protocol::symbol_to_string(
            request->symbol
        );

    if (symbol.empty()) {
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

    const Side side =
        convert_side(request->side);

    const OrderType order_type =
        convert_order_type(
            request->order_type
        );

    const TimeInForce time_in_force =
        convert_time_in_force(
            request->time_in_force
        );

    const RiskClientId risk_client_id =
        risk_client_id_for_socket(
            client_socket
        );

    bool accepted = false;
    bool duplicate_order_id = false;
    RiskRejectReason risk_reject_reason =
        RiskRejectReason::None;

    std::vector<ExecutionDelivery> deliveries;
    std::vector<protocol::Level3OrderExecuted> level3_executions;
    std::vector<protocol::Level3OrderDeleted> level3_deletions;
    std::optional<protocol::Level3AddOrder> level3_add;

    BookSnapshot snapshot {
        .symbol = request->symbol,
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

        InstrumentState& instrument =
            instrument_for(symbol);

        duplicate_order_id =
            instrument.book.find_order(
                request->order_id
            ) != nullptr ||
            instrument.order_owners.contains(
                request->order_id
            );

        if (!duplicate_order_id) {
            const Order order {
                .id = request->order_id,
                .side = side,
                .type = order_type,
                .time_in_force = time_in_force,
                .price = request->price,
                .initial_quantity = quantity,
                .remaining_quantity = quantity,
                .timestamp = request->timestamp
            };

            std::optional<Price> reference_price;

            if (order_type == OrderType::Market) {
                reference_price =
                    market_reference_price(
                        side,
                        instrument.book
                    );
            }

            const RiskDecision risk_decision =
                risk_engine_.check_new_order({
                    .client_id = risk_client_id,
                    .symbol = symbol,
                    .order_id = request->order_id,
                    .side = side,
                    .order_type = order_type,
                    .time_in_force = time_in_force,
                    .price = request->price,
                    .quantity = quantity,
                    .reference_price = reference_price
                });

            if (!risk_decision.accepted) {
                risk_reject_reason =
                    risk_decision.reason;
                trade_buffer.clear();
            } else {
                /*
                 * Accepted-command journaling is deliberately performed
                 * after validation/risk approval but before the matching
                 * mutation. The outer journal-processing lock preserves
                 * global command order across client threads.
                 */
                append_journal_record(
                    header,
                    body
                );

                engine_.process_order_into(
                    instrument.book,
                    order,
                    trade_buffer
                );

                accepted = true;

                deliveries.reserve(
                    trade_buffer.size()
                );

                std::unordered_set<OrderId>
                    deleted_order_ids;

                for (const Trade& trade : trade_buffer) {
                    const OrderOwner buyer_owner =
                        find_order_owner(
                            instrument,
                            trade.buy_order_id,
                            request->order_id,
                            client_socket,
                            risk_client_id
                        );

                    const OrderOwner seller_owner =
                        find_order_owner(
                            instrument,
                            trade.sell_order_id,
                            request->order_id,
                            client_socket,
                            risk_client_id
                        );

                    deliveries.push_back({
                        .trade = trade,
                        .symbol = request->symbol,
                        .buyer_socket = buyer_owner.socket,
                        .seller_socket = seller_owner.socket,
                        .buyer_risk_client_id =
                            buyer_owner.risk_client_id,
                        .seller_risk_client_id =
                            seller_owner.risk_client_id
                    });

                    risk_engine_.on_trade(
                        symbol,
                        buyer_owner.risk_client_id,
                        seller_owner.risk_client_id,
                        trade.buy_order_id,
                        trade.sell_order_id,
                        trade.quantity
                    );

                    level3_executions.push_back({
                        .buy_order_id = trade.buy_order_id,
                        .sell_order_id = trade.sell_order_id,
                        .price = static_cast<std::int64_t>(
                            trade.price
                        ),
                        .quantity = static_cast<std::uint64_t>(
                            trade.quantity
                        ),
                        .sequence_number = 0,
                        .symbol = request->symbol
                    });

                    if (
                        trade.buy_order_id != request->order_id &&
                        instrument.book.find_order(
                            trade.buy_order_id
                        ) == nullptr
                    ) {
                        deleted_order_ids.insert(
                            trade.buy_order_id
                        );
                    }

                    if (
                        trade.sell_order_id != request->order_id &&
                        instrument.book.find_order(
                            trade.sell_order_id
                        ) == nullptr
                    ) {
                        deleted_order_ids.insert(
                            trade.sell_order_id
                        );
                    }
                }

                for (
                    const OrderId deleted_order_id :
                    deleted_order_ids
                ) {
                    level3_deletions.push_back({
                        .order_id = deleted_order_id,
                        .sequence_number = 0,
                        .symbol = request->symbol
                    });
                }

                remove_filled_order_owners(
                    instrument,
                    trade_buffer,
                    request->order_id
                );

                if (
                    instrument.book.find_order(
                        request->order_id
                    ) != nullptr
                ) {
                    instrument.order_owners[
                        request->order_id
                    ] = OrderOwner {
                        .socket = client_socket,
                        .risk_client_id = risk_client_id
                    };

                    const Order* resting_order =
                        instrument.book.find_order(
                            request->order_id
                        );

                    risk_engine_.on_order_resting(
                        risk_client_id,
                        symbol,
                        resting_order->id,
                        resting_order->side,
                        resting_order->price,
                        resting_order->remaining_quantity
                    );

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
                        .sequence_number = 0,
                        .symbol = request->symbol
                    };
                }

                snapshot = capture_book_snapshot(
                    request->symbol,
                    instrument.book
                );
            }
        } else {
            trade_buffer.clear();
        }
    }

    if (accepted) {
        std::cout
            << "Accepted "
            << symbol
            << " order "
            << request->order_id
            << " from socket "
            << client_socket
            << '\n';
    } else if (
        risk_reject_reason !=
        RiskRejectReason::None
    ) {
        std::cerr
            << "Risk rejected "
            << symbol
            << " order "
            << request->order_id
            << ": "
            << to_string(risk_reject_reason)
            << '\n';
    } else if (duplicate_order_id) {
        std::cerr
            << "Rejected duplicate "
            << symbol
            << " order ID "
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
    if (
        header.body_size !=
        protocol::cancel_order_body_size
    ) {
        send_order_response(client_socket, 0, false);
        return;
    }

    const auto request =
        protocol::decode_cancel_order(body);

    if (!request.has_value()) {
        send_order_response(client_socket, 0, false);
        return;
    }

    const std::string symbol =
        protocol::symbol_to_string(
            request->symbol
        );

    const RiskClientId risk_client_id =
        risk_client_id_for_socket(
            client_socket
        );

    bool cancelled = false;

    BookSnapshot snapshot {
        .symbol = request->symbol,
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

        InstrumentState* instrument =
            find_instrument(symbol);

        if (instrument != nullptr) {
            const auto owner =
                instrument->order_owners.find(
                    request->order_id
                );

            if (
                owner != instrument->order_owners.end() &&
                owner->second.socket == client_socket &&
                owner->second.risk_client_id == risk_client_id &&
                instrument->book.find_order(
                    request->order_id
                ) != nullptr
            ) {
                append_journal_record(
                    header,
                    body
                );

                cancelled = engine_.cancel_order(
                    instrument->book,
                    request->order_id
                );

                if (cancelled) {
                    risk_engine_.on_order_cancelled(
                        owner->second.risk_client_id,
                        symbol,
                        request->order_id
                    );

                    instrument->order_owners.erase(owner);
                    snapshot = capture_book_snapshot(
                        request->symbol,
                        instrument->book
                    );
                }
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
        .sequence_number = 0,
        .symbol = request->symbol
    });
}

void ExchangeServer::handle_replace_order(
    int client_socket,
    const protocol::MessageHeader& header,
    std::span<const std::byte> body,
    MatchingEngine::BufferedTrades& trade_buffer
) {
    if (
        header.body_size !=
        protocol::replace_order_body_size
    ) {
        send_order_response(client_socket, 0, false);
        return;
    }

    const auto request =
        protocol::decode_replace_order(body);

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

    const std::string symbol =
        protocol::symbol_to_string(
            request->symbol
        );

    const Quantity new_quantity =
        static_cast<Quantity>(
            request->new_quantity
        );

    const RiskClientId risk_client_id =
        risk_client_id_for_socket(
            client_socket
        );

    bool replaced = false;
    RiskRejectReason risk_reject_reason =
        RiskRejectReason::None;

    std::vector<ExecutionDelivery> deliveries;
    std::vector<protocol::Level3OrderExecuted> level3_executions;
    std::vector<protocol::Level3OrderDeleted> level3_deletions;
    std::optional<protocol::Level3AddOrder> level3_add;

    BookSnapshot snapshot {
        .symbol = request->symbol,
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

        InstrumentState* instrument =
            find_instrument(symbol);

        if (instrument != nullptr) {
            const auto owner =
                instrument->order_owners.find(
                    request->order_id
                );

            const Order* existing_order =
                instrument->book.find_order(
                    request->order_id
                );

            if (
                owner != instrument->order_owners.end() &&
                owner->second.socket == client_socket &&
                owner->second.risk_client_id == risk_client_id &&
                existing_order != nullptr
            ) {
                const RiskDecision risk_decision =
                    risk_engine_.check_replace(
                        risk_client_id,
                        symbol,
                        request->order_id,
                        request->new_price,
                        new_quantity
                    );

                if (!risk_decision.accepted) {
                    risk_reject_reason =
                        risk_decision.reason;
                    trade_buffer.clear();
                } else {
                    const Order original_order =
                        *existing_order;

                    append_journal_record(
                        header,
                        body
                    );

                    /*
                     * Remove the original resting exposure before matching
                     * the replacement. Any replacement remainder is added
                     * back below with its new price and quantity.
                     */
                    risk_engine_.on_order_cancelled(
                        risk_client_id,
                        symbol,
                        request->order_id
                    );

                    replaced = engine_.replace_order_into(
                        instrument->book,
                        request->order_id,
                        request->new_price,
                        new_quantity,
                        request->timestamp,
                        trade_buffer
                    );

                    if (!replaced) {
                        risk_engine_.on_order_resting(
                            risk_client_id,
                            symbol,
                            original_order.id,
                            original_order.side,
                            original_order.price,
                            original_order.remaining_quantity
                        );
                    } else {
                        deliveries.reserve(
                            trade_buffer.size()
                        );

                        std::unordered_set<OrderId>
                            deleted_order_ids;

                        for (const Trade& trade : trade_buffer) {
                            const OrderOwner buyer_owner =
                                find_order_owner(
                                    *instrument,
                                    trade.buy_order_id,
                                    request->order_id,
                                    client_socket,
                                    risk_client_id
                                );

                            const OrderOwner seller_owner =
                                find_order_owner(
                                    *instrument,
                                    trade.sell_order_id,
                                    request->order_id,
                                    client_socket,
                                    risk_client_id
                                );

                            deliveries.push_back({
                                .trade = trade,
                                .symbol = request->symbol,
                                .buyer_socket = buyer_owner.socket,
                                .seller_socket = seller_owner.socket,
                                .buyer_risk_client_id =
                                    buyer_owner.risk_client_id,
                                .seller_risk_client_id =
                                    seller_owner.risk_client_id
                            });

                            risk_engine_.on_trade(
                                symbol,
                                buyer_owner.risk_client_id,
                                seller_owner.risk_client_id,
                                trade.buy_order_id,
                                trade.sell_order_id,
                                trade.quantity
                            );

                            level3_executions.push_back({
                                .buy_order_id = trade.buy_order_id,
                                .sell_order_id = trade.sell_order_id,
                                .price = static_cast<std::int64_t>(
                                    trade.price
                                ),
                                .quantity = static_cast<std::uint64_t>(
                                    trade.quantity
                                ),
                                .sequence_number = 0,
                                .symbol = request->symbol
                            });

                            if (
                                trade.buy_order_id != request->order_id &&
                                instrument->book.find_order(
                                    trade.buy_order_id
                                ) == nullptr
                            ) {
                                deleted_order_ids.insert(
                                    trade.buy_order_id
                                );
                            }

                            if (
                                trade.sell_order_id != request->order_id &&
                                instrument->book.find_order(
                                    trade.sell_order_id
                                ) == nullptr
                            ) {
                                deleted_order_ids.insert(
                                    trade.sell_order_id
                                );
                            }
                        }

                        for (
                            const OrderId deleted_id :
                            deleted_order_ids
                        ) {
                            level3_deletions.push_back({
                                .order_id = deleted_id,
                                .sequence_number = 0,
                                .symbol = request->symbol
                            });
                        }

                        remove_filled_order_owners(
                            *instrument,
                            trade_buffer,
                            request->order_id
                        );

                        if (
                            instrument->book.find_order(
                                request->order_id
                            ) != nullptr
                        ) {
                            instrument->order_owners[
                                request->order_id
                            ] = OrderOwner {
                                .socket = client_socket,
                                .risk_client_id = risk_client_id
                            };

                            const Order* resting =
                                instrument->book.find_order(
                                    request->order_id
                                );

                            risk_engine_.on_order_resting(
                                risk_client_id,
                                symbol,
                                resting->id,
                                resting->side,
                                resting->price,
                                resting->remaining_quantity
                            );

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
                                .sequence_number = 0,
                                .symbol = request->symbol
                            };
                        } else {
                            instrument->order_owners.erase(
                                request->order_id
                            );
                        }

                        snapshot = capture_book_snapshot(
                            request->symbol,
                            instrument->book
                        );
                    }
                }
            }
        }
    }

    if (
        !replaced &&
        risk_reject_reason != RiskRejectReason::None
    ) {
        std::cerr
            << "Risk rejected replacement for "
            << symbol
            << " order "
            << request->order_id
            << ": "
            << to_string(risk_reject_reason)
            << '\n';
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

    /*
     * A replacement removes the old resting representation first.
     */
    broadcast_level3_order_deleted({
        .order_id = request->order_id,
        .sequence_number = 0,
        .symbol = request->symbol
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

void ExchangeServer::append_journal_record(
    const protocol::MessageHeader& header,
    std::span<const std::byte> body
) {
    if (journal_ == nullptr) {
        return;
    }

    const auto encoded_header =
        protocol::encode_header(header);

    std::vector<std::byte> message;
    message.reserve(
        encoded_header.size() + body.size()
    );

    message.insert(
        message.end(),
        encoded_header.begin(),
        encoded_header.end()
    );

    message.insert(
        message.end(),
        body.begin(),
        body.end()
    );

    journal_->append(message);
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
                : protocol::MessageType::OrderRejected,
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
    const protocol::Symbol& symbol,
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
        .sequence_number = sequence_number,
        .symbol = symbol
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
            delivery.symbol,
            delivery.trade
        );

        send_trade_execution(
            delivery.seller_socket,
            delivery.symbol,
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
        .sequence_number = sequence_number,
        .symbol = snapshot.symbol
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

    if (
        multicast_publisher_ != nullptr &&
        !multicast_publisher_->send(message)
    ) {
        std::cerr
            << "Failed to enqueue multicast market-data datagram\n";
    }

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
        protocol::encode_level3_add_order(
            sequenced_event
        );

    const protocol::MessageHeader header {
        .magic = protocol::protocol_magic,
        .version = protocol::protocol_version,
        .type = protocol::MessageType::Level3AddOrder,
        .body_size =
            static_cast<std::uint32_t>(body.size()),
        .sequence_number =
            sequenced_event.sequence_number
    };

    const auto encoded_header =
        protocol::encode_header(header);

    const std::vector<std::byte> message =
        combine_message(encoded_header, body);

    if (
        multicast_publisher_ != nullptr &&
        !multicast_publisher_->send(message)
    ) {
        std::cerr
            << "Failed to enqueue multicast market-data datagram\n";
    }

    const std::vector<int> clients =
        client_socket_snapshot();

    std::lock_guard<std::mutex> send_lock(
        send_mutex_
    );

    for (const int socket : clients) {
        if (!TcpServer::send_to(socket, message)) {
            std::cerr
                << "Failed to send Level-3 add event to socket "
                << socket
                << '\n';
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
        protocol::encode_level3_order_executed(
            sequenced_event
        );

    const protocol::MessageHeader header {
        .magic = protocol::protocol_magic,
        .version = protocol::protocol_version,
        .type =
            protocol::MessageType::Level3OrderExecuted,
        .body_size =
            static_cast<std::uint32_t>(body.size()),
        .sequence_number =
            sequenced_event.sequence_number
    };

    const auto encoded_header =
        protocol::encode_header(header);

    const std::vector<std::byte> message =
        combine_message(encoded_header, body);

    if (
        multicast_publisher_ != nullptr &&
        !multicast_publisher_->send(message)
    ) {
        std::cerr
            << "Failed to enqueue multicast market-data datagram\n";
    }

    const std::vector<int> clients =
        client_socket_snapshot();

    std::lock_guard<std::mutex> send_lock(
        send_mutex_
    );

    for (const int socket : clients) {
        if (!TcpServer::send_to(socket, message)) {
            std::cerr
                << "Failed to send Level-3 execution event to socket "
                << socket
                << '\n';
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
        protocol::encode_level3_order_deleted(
            sequenced_event
        );

    const protocol::MessageHeader header {
        .magic = protocol::protocol_magic,
        .version = protocol::protocol_version,
        .type =
            protocol::MessageType::Level3OrderDeleted,
        .body_size =
            static_cast<std::uint32_t>(body.size()),
        .sequence_number =
            sequenced_event.sequence_number
    };

    const auto encoded_header =
        protocol::encode_header(header);

    const std::vector<std::byte> message =
        combine_message(encoded_header, body);

    if (
        multicast_publisher_ != nullptr &&
        !multicast_publisher_->send(message)
    ) {
        std::cerr
            << "Failed to enqueue multicast market-data datagram\n";
    }

    const std::vector<int> clients =
        client_socket_snapshot();

    std::lock_guard<std::mutex> send_lock(
        send_mutex_
    );

    for (const int socket : clients) {
        if (!TcpServer::send_to(socket, message)) {
            std::cerr
                << "Failed to send Level-3 delete event to socket "
                << socket
                << '\n';
        }
    }
}

ExchangeServer::BookSnapshot
ExchangeServer::capture_book_snapshot(
    const protocol::Symbol& symbol,
    const OrderBook& book
) const {
    BookSnapshot snapshot {
        .symbol = symbol,
        .has_bid = book.has_bids(),
        .best_bid = 0,
        .bid_quantity = 0,
        .has_ask = book.has_asks(),
        .best_ask = 0,
        .ask_quantity = 0
    };

    if (snapshot.has_bid) {
        snapshot.best_bid =
            book.best_bid();

        snapshot.bid_quantity =
            book.best_bid_level()
                .total_quantity();
    }

    if (snapshot.has_ask) {
        snapshot.best_ask =
            book.best_ask();

        snapshot.ask_quantity =
            book.best_ask_level()
                .total_quantity();
    }

    return snapshot;
}

ExchangeServer::OrderOwner
ExchangeServer::find_order_owner(
    const InstrumentState& instrument,
    OrderId order_id,
    OrderId incoming_order_id,
    int incoming_socket,
    RiskClientId incoming_risk_client_id
) const {
    if (order_id == incoming_order_id) {
        return {
            .socket = incoming_socket,
            .risk_client_id = incoming_risk_client_id
        };
    }

    const auto owner =
        instrument.order_owners.find(order_id);

    if (owner == instrument.order_owners.end()) {
        return {};
    }

    return owner->second;
}

void ExchangeServer::remove_filled_order_owners(
    InstrumentState& instrument,
    const MatchingEngine::BufferedTrades& trades,
    OrderId incoming_order_id
) {
    for (const Trade& trade : trades) {
        if (
            trade.buy_order_id != incoming_order_id &&
            instrument.book.find_order(
                trade.buy_order_id
            ) == nullptr
        ) {
            instrument.order_owners.erase(
                trade.buy_order_id
            );
        }

        if (
            trade.sell_order_id != incoming_order_id &&
            instrument.book.find_order(
                trade.sell_order_id
            ) == nullptr
        ) {
            instrument.order_owners.erase(
                trade.sell_order_id
            );
        }
    }
}

ExchangeServer::InstrumentState&
ExchangeServer::instrument_for(
    const std::string& symbol
) {
    return instruments_.try_emplace(symbol).first->second;
}

ExchangeServer::InstrumentState*
ExchangeServer::find_instrument(
    const std::string& symbol
) {
    const auto iterator =
        instruments_.find(symbol);

    if (iterator == instruments_.end()) {
        return nullptr;
    }

    return &iterator->second;
}

std::optional<Price>
ExchangeServer::market_reference_price(
    Side side,
    const OrderBook& book
) const {
    if (side == Side::Buy) {
        if (!book.has_asks()) {
            return std::nullopt;
        }

        return book.best_ask();
    }

    if (!book.has_bids()) {
        return std::nullopt;
    }

    return book.best_bid();
}

RiskClientId ExchangeServer::register_client(
    int client_socket
) {
    const RiskClientId risk_client_id =
        next_risk_client_id_.fetch_add(1);

    std::lock_guard<std::mutex> lock(
        clients_mutex_
    );

    client_sockets_.push_back(client_socket);
    client_risk_ids_[client_socket] =
        risk_client_id;

    return risk_client_id;
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

    client_risk_ids_.erase(client_socket);
}

RiskClientId ExchangeServer::risk_client_id_for_socket(
    int client_socket
) {
    std::lock_guard<std::mutex> lock(
        clients_mutex_
    );

    const auto iterator =
        client_risk_ids_.find(client_socket);

    if (iterator == client_risk_ids_.end()) {
        return invalid_risk_client_id;
    }

    return iterator->second;
}

std::vector<int>
ExchangeServer::client_socket_snapshot() {
    std::lock_guard<std::mutex> lock(
        clients_mutex_
    );

    return client_sockets_;
}

}  // namespace exchange