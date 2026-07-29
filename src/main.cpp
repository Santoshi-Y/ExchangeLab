#include <iostream>

#include "exchange/price_level.hpp"

int main() {
    exchange::PriceLevel level{10'025};

    const exchange::Order first_order{
        .id = 1,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 10'025,
        .initial_quantity = 100,
        .remaining_quantity = 100,
        .timestamp = 1
    };

    const exchange::Order second_order{
        .id = 2,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 10'025,
        .initial_quantity = 50,
        .remaining_quantity = 50,
        .timestamp = 2
    };

    level.add_order(first_order);
    level.add_order(second_order);

    std::cout << "Price: " << level.price() << '\n';
    std::cout << "Orders: " << level.order_count() << '\n';
    std::cout << "Total quantity: " << level.total_quantity() << '\n';
    std::cout << "First order ID: " << level.front().id << '\n';

    return 0;
}