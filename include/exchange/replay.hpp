#pragma once

#include <cstddef>
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

private:
    void reset();

    std::unique_ptr<OrderBook> book_;
    MatchingEngine engine_;
    MatchingEngine::BufferedTrades trade_buffer_;
    ReplaySummary summary_;
};

}  // namespace exchange