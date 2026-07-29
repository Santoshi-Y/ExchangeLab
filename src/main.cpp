#include <iostream>

#include "exchange/order.hpp"

int main() {
    const exchange::Order order{
        .id = 1,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 10'025,
        .initial_quantity = 100,
        .remaining_quantity = 100,
        .timestamp = 1
    };

    std::cout << "ExchangeLab initialized.\n";
    std::cout << "Order ID: " << order.id << '\n';
    std::cout << "Price: " << order.price << '\n';
    std::cout << "Remaining quantity: "
              << order.remaining_quantity << '\n';
    std::cout << "Filled: "
              << (order.is_filled() ? "Yes" : "No")
              << '\n';

    return 0;
}