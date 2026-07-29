#include <iostream>
#include <vector>

#include "exchange/matching_engine.hpp"

namespace {

void print_trades(const std::vector<exchange::Trade>& trades) {
    if (trades.empty()) {
        std::cout << "No trades executed.\n";
        return;
    }

    for (const auto& trade : trades) {
        std::cout
            << "Trade: buy_order="
            << trade.buy_order_id
            << ", sell_order="
            << trade.sell_order_id
            << ", price="
            << trade.price
            << ", quantity="
            << trade.quantity
            << '\n';
    }
}

void print_top_of_book(const exchange::OrderBook& book) {
    std::cout << "\nTop of book\n";

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
}

}  // namespace

int main() {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    // Build three ask levels.
    book.add_order({
        .id = 1,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 101,
        .initial_quantity = 20,
        .remaining_quantity = 20,
        .timestamp = 1
    });

    book.add_order({
        .id = 2,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 102,
        .initial_quantity = 30,
        .remaining_quantity = 30,
        .timestamp = 2
    });

    book.add_order({
        .id = 3,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 103,
        .initial_quantity = 50,
        .remaining_quantity = 50,
        .timestamp = 3
    });

    std::cout << "Incoming buy: 60 @ 103\n";

    auto buy_trades = engine.process_order(
        book,
        {
            .id = 10,
            .side = exchange::Side::Buy,
            .type = exchange::OrderType::Limit,
            .time_in_force =
                exchange::TimeInForce::GoodTillCancel,
            .price = 103,
            .initial_quantity = 60,
            .remaining_quantity = 60,
            .timestamp = 10
        }
    );

    print_trades(buy_trades);
    print_top_of_book(book);

    std::cout << "\nIncoming sell: 25 @ 100\n";

    auto sell_trades = engine.process_order(
        book,
        {
            .id = 11,
            .side = exchange::Side::Sell,
            .type = exchange::OrderType::Limit,
            .time_in_force =
                exchange::TimeInForce::GoodTillCancel,
            .price = 100,
            .initial_quantity = 25,
            .remaining_quantity = 25,
            .timestamp = 11
        }
    );

    print_trades(sell_trades);
    print_top_of_book(book);

    return 0;
}