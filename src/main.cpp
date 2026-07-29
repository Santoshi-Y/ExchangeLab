#include <iostream>

#include "exchange/matching_engine.hpp"

int main() {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    book.add_order({
        .id = 1,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 101,
        .initial_quantity = 10,
        .remaining_quantity = 10,
        .timestamp = 1
    });

    book.add_order({
        .id = 2,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 102,
        .initial_quantity = 20,
        .remaining_quantity = 20,
        .timestamp = 2
    });

    book.add_order({
        .id = 3,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 104,
        .initial_quantity = 30,
        .remaining_quantity = 30,
        .timestamp = 3
    });

    const exchange::Order market_buy{
        .id = 10,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Market,
        .time_in_force =
            exchange::TimeInForce::ImmediateOrCancel,
        .price = 0,
        .initial_quantity = 45,
        .remaining_quantity = 45,
        .timestamp = 10
    };

    const auto trades =
        engine.process_order(book, market_buy);

    std::cout << "ExchangeLab Market Order Demo\n";
    std::cout << "=============================\n";

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
    } else {
        std::cout << "No asks remain.\n";
    }

    return 0;
}