#include <iostream>

#include "exchange/price_level.hpp"

int main() {

    exchange::PriceLevel level{103};

    level.add_order({
        .id = 1,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 103,
        .initial_quantity = 20,
        .remaining_quantity = 20,
        .timestamp = 1
    });

    std::cout << "Before fill\n";
    std::cout << level.front().remaining_quantity << '\n';

    level.front().remaining_quantity -= 5;

    std::cout << "After partial fill\n";
    std::cout << level.front().remaining_quantity << '\n';

    return 0;
}