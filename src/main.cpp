#include <iostream>

#include "exchange/order_book.hpp"

int main() {

    exchange::OrderBook book;

    book.add_order({
        .id = 1,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 101,
        .initial_quantity = 10,
        .remaining_quantity = 10,
        .timestamp = 1
    });

    book.add_order({
        .id = 2,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 103,
        .initial_quantity = 5,
        .remaining_quantity = 5,
        .timestamp = 2
    });

    book.add_order({
        .id = 3,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 108,
        .initial_quantity = 12,
        .remaining_quantity = 12,
        .timestamp = 3
    });

    book.add_order({
        .id = 4,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 106,
        .initial_quantity = 8,
        .remaining_quantity = 8,
        .timestamp = 4
    });

    std::cout << "Best Bid: " << book.best_bid() << '\n';
    std::cout << "Best Ask: " << book.best_ask() << '\n';

    return 0;
}