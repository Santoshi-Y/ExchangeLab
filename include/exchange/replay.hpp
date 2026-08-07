#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "exchange/matching_engine.hpp"
#include "exchange/order_book.hpp"

namespace exchange {

struct ReplaySummary {
    std::uint64_t journal_records {0};
    std::uint64_t new_orders {0};
    std::uint64_t trades {0};
    std::uint64_t rejected_messages {0};
    std::uint64_t unsupported_messages {0};
    std::uint64_t symbols {0};
};

class ExchangeReplayer {
public:
    using RecoveredBooks = std::unordered_map<
        std::string,
        std::unique_ptr<OrderBook>
    >;

    ExchangeReplayer();

    [[nodiscard]] bool replay(
        const std::filesystem::path& journal_path
    );

    [[nodiscard]] const ReplaySummary&
    summary() const noexcept;

    /*
     * Backward-compatible accessor for the default TEST instrument.
     */
    [[nodiscard]] const OrderBook&
    order_book() const noexcept;

    [[nodiscard]] const OrderBook& order_book(
        std::string_view symbol
    ) const noexcept;

    [[nodiscard]] bool has_symbol(
        std::string_view symbol
    ) const noexcept;

    [[nodiscard]] std::vector<std::string>
    symbols() const;

    /*
     * Backward-compatible release for the default TEST instrument.
     */
    [[nodiscard]] std::unique_ptr<OrderBook>
    release_order_book();

    [[nodiscard]] RecoveredBooks
    release_order_books();

private:
    [[nodiscard]] OrderBook& book_for(
        const std::string& symbol
    );

    void reset();

    MatchingEngine engine_;
    MatchingEngine::BufferedTrades trade_buffer_;
    RecoveredBooks books_;
    ReplaySummary summary_;
};

}  // namespace exchange