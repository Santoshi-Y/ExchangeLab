#include <cstdint>
#include <filesystem>
#include <iostream>

#include "exchange/replay.hpp"

namespace {

void print_book(
    const exchange::OrderBook& book
) {
    std::cout
        << "Remaining orders: "
        << book.order_count()
        << '\n';

    if (book.has_bids()) {
        std::cout
            << "Best bid: "
            << book.best_bid()
            << " (quantity "
            << book.best_bid_level()
                   .total_quantity()
            << ")\n";
    } else {
        std::cout << "Best bid: none\n";
    }

    if (book.has_asks()) {
        std::cout
            << "Best ask: "
            << book.best_ask()
            << " (quantity "
            << book.best_ask_level()
                   .total_quantity()
            << ")\n";
    } else {
        std::cout << "Best ask: none\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path journal_path =
        argc >= 2
            ? std::filesystem::path(argv[1])
            : std::filesystem::path(
                "exchange.log"
            );

    exchange::ExchangeReplayer replayer;

    if (!replayer.replay(journal_path)) {
        std::cerr
            << "Failed to replay journal: "
            << journal_path
            << '\n';

        return 1;
    }

    const exchange::ReplaySummary& summary =
        replayer.summary();

    std::cout
        << "ExchangeLab Journal Replay\n"
        << "==========================\n"
        << "Journal: "
        << journal_path
        << '\n'
        << "Records: "
        << summary.journal_records
        << '\n'
        << "New orders: "
        << summary.new_orders
        << '\n'
        << "Trades: "
        << summary.trades
        << '\n'
        << "Rejected messages: "
        << summary.rejected_messages
        << '\n'
        << "Unsupported messages: "
        << summary.unsupported_messages
        << "\n\n";

    print_book(replayer.order_book());

    return 0;
}