#include <filesystem>
#include <iostream>
#include <string>

#include "exchange/replay.hpp"

int main(int argc, char** argv) {
    const std::filesystem::path journal_path =
        argc >= 2
            ? std::filesystem::path(argv[1])
            : std::filesystem::path("exchange.log");

    if (argc > 2) {
        std::cerr
            << "Usage: "
            << argv[0]
            << " [journal_path]\n";
        return 1;
    }

    exchange::ExchangeReplayer replayer;

    if (!replayer.replay(journal_path)) {
        std::cerr
            << "Failed to replay journal: \""
            << journal_path.string()
            << "\"\n";
        return 1;
    }

    const exchange::ReplaySummary& summary =
        replayer.summary();

    std::cout
        << "ExchangeLab Journal Replay\n"
        << "==========================\n"
        << "Journal: \""
        << journal_path.string()
        << "\"\n"
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
        << '\n'
        << "Symbols: "
        << summary.symbols
        << "\n\n";

    for (const std::string& symbol : replayer.symbols()) {
        const exchange::OrderBook& book =
            replayer.order_book(symbol);

        std::cout
            << symbol
            << '\n'
            << std::string(symbol.size(), '-')
            << '\n'
            << "Remaining orders: "
            << book.order_count()
            << '\n';

        if (book.has_bids()) {
            std::cout
                << "Best bid: "
                << book.best_bid()
                << " x "
                << book.best_bid_level().total_quantity()
                << '\n';
        } else {
            std::cout << "Best bid: none\n";
        }

        if (book.has_asks()) {
            std::cout
                << "Best ask: "
                << book.best_ask()
                << " x "
                << book.best_ask_level().total_quantity()
                << '\n';
        } else {
            std::cout << "Best ask: none\n";
        }

        std::cout << '\n';
    }

    return 0;
}