#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "exchange/journal.hpp"
#include "exchange/matching_engine.hpp"
#include "exchange/multicast_publisher.hpp"
#include "exchange/order_book.hpp"
#include "exchange/protocol.hpp"
#include "exchange/risk_engine.hpp"
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
    std::size_t instruments {0};
};

class ExchangeServer {
public:
    explicit ExchangeServer(
        std::uint16_t port,
        std::optional<std::filesystem::path> journal_path =
            std::nullopt,
        std::optional<MulticastConfig> multicast_config =
            std::nullopt,
        RiskLimits risk_limits = {}
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

    [[nodiscard]] const RiskLimits&
    risk_limits() const noexcept;

    /*
     * Administrative circuit breaker. Existing resting orders remain on
     * the book; new and replacement orders are rejected while enabled.
     * Cancels are intentionally still allowed so exposure can be reduced.
     */
    void set_global_kill_switch(bool enabled);

private:
    struct OrderOwner {
        int socket {-1};
        RiskClientId risk_client_id {invalid_risk_client_id};
    };

    struct InstrumentState {
        OrderBook book;
        std::unordered_map<OrderId, OrderOwner> order_owners;
    };

    struct ExecutionDelivery {
        Trade trade;
        protocol::Symbol symbol;
        int buyer_socket {-1};
        int seller_socket {-1};
        RiskClientId buyer_risk_client_id {
            invalid_risk_client_id
        };
        RiskClientId seller_risk_client_id {
            invalid_risk_client_id
        };
    };

    struct BookSnapshot {
        protocol::Symbol symbol;
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

    void append_journal_record(
        const protocol::MessageHeader& header,
        std::span<const std::byte> body
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
        const protocol::Symbol& symbol,
        const Trade& trade
    );

    void send_execution_deliveries(
        const std::vector<ExecutionDelivery>& deliveries
    );

    void broadcast_book_update(
        const BookSnapshot& snapshot
    );

    void broadcast_level3_add_order(
        const protocol::Level3AddOrder& event
    );

    void broadcast_level3_order_executed(
        const protocol::Level3OrderExecuted& event
    );

    void broadcast_level3_order_deleted(
        const protocol::Level3OrderDeleted& event
    );

    [[nodiscard]] BookSnapshot capture_book_snapshot(
        const protocol::Symbol& symbol,
        const OrderBook& book
    ) const;

    [[nodiscard]] OrderOwner find_order_owner(
        const InstrumentState& instrument,
        OrderId order_id,
        OrderId incoming_order_id,
        int incoming_socket,
        RiskClientId incoming_risk_client_id
    ) const;

    void remove_filled_order_owners(
        InstrumentState& instrument,
        const MatchingEngine::BufferedTrades& trades,
        OrderId incoming_order_id
    );

    [[nodiscard]] InstrumentState& instrument_for(
        const std::string& symbol
    );

    [[nodiscard]] InstrumentState* find_instrument(
        const std::string& symbol
    );

    [[nodiscard]] std::optional<Price>
    market_reference_price(
        Side side,
        const OrderBook& book
    ) const;

    [[nodiscard]] RiskClientId register_client(
        int client_socket
    );

    void unregister_client(int client_socket);

    [[nodiscard]] RiskClientId risk_client_id_for_socket(
        int client_socket
    );

    [[nodiscard]] std::vector<int>
    client_socket_snapshot();

    TcpServer server_;
    MatchingEngine engine_;
    RiskEngine risk_engine_;

    std::mutex engine_mutex_;
    std::unordered_map<std::string, InstrumentState> instruments_;

    std::mutex send_mutex_;
    std::mutex clients_mutex_;
    std::vector<int> client_sockets_;
    std::unordered_map<int, RiskClientId> client_risk_ids_;
    std::mutex threads_mutex_;
    std::vector<std::thread> client_threads_;

    std::optional<std::filesystem::path> journal_path_;
    std::unique_ptr<ExchangeJournal> journal_;

    std::optional<MulticastConfig> multicast_config_;
    std::unique_ptr<MulticastPublisher> multicast_publisher_;
    std::mutex journal_processing_mutex_;

    RecoveryState recovery_state_;
    bool recovery_completed_ {false};

    std::atomic<bool> running_;
    std::atomic<std::uint64_t> next_sequence_number_;
    std::atomic<RiskClientId> next_risk_client_id_;
};

}  // namespace exchange
