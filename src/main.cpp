#include <iostream>

#include "exchange/matching_engine.hpp"

int main() {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    // Three sell orders arrive at the same price.
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
        .price = 101,
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
        .price = 101,
        .initial_quantity = 30,
        .remaining_quantity = 30,
        .timestamp = 3
    });

    const auto trades = engine.process_order(
        book,
        {
            .id = 10,
            .side = exchange::Side::Buy,
            .type = exchange::OrderType::Limit,
            .time_in_force =
                exchange::TimeInForce::GoodTillCancel,
            .price = 101,
            .initial_quantity = 35,
            .remaining_quantity = 35,
            .timestamp = 10
        }
    );

    for (const auto& trade : trades) {
        std::cout
            << "Matched sell order "
            << trade.sell_order_id
            << " for "
            << trade.quantity
            << '\n';
    }

    std::cout
        << "Remaining ask quantity: "
        << book.best_ask_level().total_quantity()
        << '\n';

    std::cout
        << "Next order ID: "
        << book.best_ask_level().front().id
        << '\n';

    std::cout
        << "Next order remaining quantity: "
        << book.best_ask_level().front().remaining_quantity
        << '\n';

    return 0;
}