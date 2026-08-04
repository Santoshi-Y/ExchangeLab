#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "exchange/matching_engine.hpp"
#include "exchange/order_book.hpp"

namespace exchange {

struct ReplaySummary {
    std::uint64_t journal_records {0};
    std::uint64_t new_orders {0};
    std::uint64_t trades {0};
    std::uint64_t rejected_messages {0};
    std::uint64_t unsupported_messages {0};
};

class ExchangeReplayer {
public:
    ExchangeReplayer();

    [[nodiscard]] bool replay(
        const std::filesystem::path& journal_path
    );

    [[nodiscard]] const ReplaySummary&
    summary() const noexcept;

    [[nodiscard]] const OrderBook&
    order_book() const noexcept;

    /*
     * Transfers the reconstructed order book to a live server.
     * After this call, this replayer owns a fresh empty book.
     */
    [[nodiscard]] std::unique_ptr<OrderBook>
    release_order_book();

private:
    void reset();

    MatchingEngine engine_;
    MatchingEngine::BufferedTrades trade_buffer_;
    std::unique_ptr<OrderBook> book_;
    ReplaySummary summary_;
};

}  // namespace exchange