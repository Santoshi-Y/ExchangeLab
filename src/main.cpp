#include <iostream>

#include "exchange/matching_engine.hpp"

int main() {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    const exchange::Order first_buy{
        .id = 1,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 100,
        .initial_quantity = 10,
        .remaining_quantity = 10,
        .timestamp = 1
    };

    const exchange::Order second_buy{
        .id = 2,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 100,
        .initial_quantity = 20,
        .remaining_quantity = 20,
        .timestamp = 2
    };

    const exchange::Order third_buy{
        .id = 3,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 100,
        .initial_quantity = 30,
        .remaining_quantity = 30,
        .timestamp = 3
    };

    engine.process_order(book, first_buy);
    engine.process_order(book, second_buy);
    engine.process_order(book, third_buy);

    std::cout << "ExchangeLab Cancellation Demo\n";
    std::cout << "=============================\n";

    std::cout
        << "Before cancellation: "
        << book.best_bid_level().order_count()
        << " orders, "
        << book.best_bid_level().total_quantity()
        << " total quantity\n";

    const bool cancelled =
        engine.cancel_order(book, 2);

    std::cout
        << "Cancel order 2: "
        << (cancelled ? "successful" : "not found")
        << '\n';

    std::cout
        << "After cancellation: "
        << book.best_bid_level().order_count()
        << " orders, "
        << book.best_bid_level().total_quantity()
        << " total quantity\n";

    std::cout
        << "Next order in FIFO queue: "
        << book.best_bid_level().front().id
        << '\n';

    return 0;
}