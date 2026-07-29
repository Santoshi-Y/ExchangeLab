#include <iostream>

#include "exchange/matching_engine.hpp"

int main() {

    exchange::OrderBook book;

    book.add_order({
        .id = 1,
        .side = exchange::Side::Sell,
        .type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 101,
        .initial_quantity = 40,
        .remaining_quantity = 40,
        .timestamp = 1
    });

    exchange::MatchingEngine engine;

    exchange::Order buy{
        .id = 2,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 105,
        .initial_quantity = 25,
        .remaining_quantity = 25,
        .timestamp = 2
    };

    auto trades = engine.match_buy(book, buy);

    for (const auto& trade : trades) {

        std::cout
            << "Trade\n";

        std::cout
            << "Buy "
            << trade.buy_order_id
            << '\n';

        std::cout
            << "Sell "
            << trade.sell_order_id
            << '\n';

        std::cout
            << "Price "
            << trade.price
            << '\n';

        std::cout
            << "Quantity "
            << trade.quantity
            << "\n\n";
    }
}