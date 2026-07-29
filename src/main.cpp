#include <iostream>

#include "exchange/matching_engine.hpp"

int main() {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    const exchange::Order resting_sell{
        .id = 1,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 101,
        .initial_quantity = 40,
        .remaining_quantity = 40,
        .timestamp = 1
    };

    book.add_order(resting_sell);

    const exchange::Order incoming_buy{
        .id = 2,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 105,
        .initial_quantity = 25,
        .remaining_quantity = 25,
        .timestamp = 2
    };

    const auto trades =
        engine.process_order(book, incoming_buy);

    std::cout << "ExchangeLab\n";
    std::cout << "===========\n";

    for (const auto& trade : trades) {
        std::cout
            << "Trade: "
            << trade.quantity
            << " units at "
            << trade.price
            << '\n';
    }

    if (book.has_asks()) {
        std::cout
            << "Remaining best ask: "
            << book.best_ask()
            << " x "
            << book.best_ask_level().total_quantity()
            << '\n';
    }

    return 0;
}