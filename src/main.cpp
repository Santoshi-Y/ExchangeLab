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

    std::cout << "Quantity before: "
              << level.total_quantity()
              << '\n';

    level.fill_front(7);

    std::cout << "Quantity after: "
              << level.total_quantity()
              << '\n';

    std::cout << "Remaining: "
              << level.front().remaining_quantity
              << '\n';

    level.fill_front(13);

    std::cout << "Orders remaining: "
              << level.order_count()
              << '\n';
}