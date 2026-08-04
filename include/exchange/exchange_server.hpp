#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include "exchange/journal.hpp"
#include "exchange/matching_engine.hpp"
#include "exchange/order_book.hpp"
#include "exchange/protocol.hpp"
#include "exchange/tcp_server.hpp"
#include "exchange/trade.hpp"

namespace exchange {

struct RecoveryState {
    bool attempted {false};
    bool successful {false};

    std::uint64_t journal_records {0};
    std::uint64_t new_orders {0};
    std::uint64_t trades {0};
    std::uint64_t rejected_messages {0};
    std::uint64_t unsupported_messages {0};

    std::size_t remaining_orders {0};
};

class ExchangeServer {
public:
    explicit ExchangeServer(
        std::uint16_t port,
        std::optional<std::filesystem::path> journal_path =
            std::nullopt
    );

    ~ExchangeServer();

    ExchangeServer(const ExchangeServer&) = delete;
    ExchangeServer& operator=(const ExchangeServer&) = delete;
    ExchangeServer(ExchangeServer&&) = delete;
    ExchangeServer& operator=(ExchangeServer&&) = delete;

    bool start();
    void run();
    void stop();

    [[nodiscard]] const RecoveryState&
    recovery_state() const noexcept;

private:
    struct ExecutionDelivery {
        Trade trade;
        int buyer_socket;
        int seller_socket;
    };

    struct BookSnapshot {
        bool has_bid;
        Price best_bid;
        Quantity bid_quantity;
        bool has_ask;
        Price best_ask;
        Quantity ask_quantity;
    };

    [[nodiscard]] bool recover_from_journal();

    void handle_client(int client_socket);

    void process_receive_buffer(
        int client_socket,
        std::vector<std::byte>& receive_buffer,
        MatchingEngine::BufferedTrades& trade_buffer
    );

    void handle_message(
        int client_socket,
        const protocol::MessageHeader& header,
        std::span<const std::byte> body,
        MatchingEngine::BufferedTrades& trade_buffer
    );

    void handle_new_order(
        int client_socket,
        const protocol::MessageHeader& header,
        std::span<const std::byte> body,
        MatchingEngine::BufferedTrades& trade_buffer
    );

    void handle_cancel_order(
        int client_socket,
        const protocol::MessageHeader& header,
        std::span<const std::byte> body
    );

    void handle_replace_order(
        int client_socket,
        const protocol::MessageHeader& header,
        std::span<const std::byte> body,
        MatchingEngine::BufferedTrades& trade_buffer
    );

    void send_order_response(
        int client_socket,
        std::uint64_t order_id,
        bool success,
        protocol::MessageType success_type =
            protocol::MessageType::OrderAccepted
    );

    void send_trade_execution(
        int client_socket,
        const Trade& trade
    );

    void send_execution_deliveries(
        const std::vector<ExecutionDelivery>& deliveries
    );

    void broadcast_book_update(const BookSnapshot& snapshot);

    void broadcast_level3_add_order(
        const protocol::Level3AddOrder& event
    );

    void broadcast_level3_order_executed(
        const protocol::Level3OrderExecuted& event
    );

    void broadcast_level3_order_deleted(
        const protocol::Level3OrderDeleted& event
    );

    [[nodiscard]] BookSnapshot capture_book_snapshot() const;

    [[nodiscard]] int find_order_owner(
        OrderId order_id,
        OrderId incoming_order_id,
        int incoming_socket
    ) const;

    void remove_filled_order_owners(
        const MatchingEngine::BufferedTrades& trades,
        OrderId incoming_order_id
    );

    void register_client(int client_socket);
    void unregister_client(int client_socket);

    [[nodiscard]] std::vector<int> client_socket_snapshot();

    TcpServer server_;
    OrderBook book_;
    MatchingEngine engine_;

    std::mutex engine_mutex_;
    std::unordered_map<OrderId, int> order_owners_;
    std::mutex send_mutex_;
    std::mutex clients_mutex_;
    std::vector<int> client_sockets_;
    std::mutex threads_mutex_;
    std::vector<std::thread> client_threads_;

    std::optional<std::filesystem::path> journal_path_;
    std::unique_ptr<ExchangeJournal> journal_;
    std::mutex journal_processing_mutex_;

    RecoveryState recovery_state_;
    bool recovery_completed_ {false};

    std::atomic<bool> running_;
    std::atomic<std::uint64_t> next_sequence_number_;
};

}  // namespace exchange
