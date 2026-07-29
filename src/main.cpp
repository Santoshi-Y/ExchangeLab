#include <iostream>

#include "exchange/matching_engine.hpp"

int main() {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;

    engine.process_order(
        book,
        {
            .id = 1,
            .side = exchange::Side::Buy,
            .type = exchange::OrderType::Limit,
            .time_in_force =
                exchange::TimeInForce::GoodTillCancel,
            .price = 100,
            .initial_quantity = 20,
            .remaining_quantity = 20,
            .timestamp = 1
        }
    );

    engine.process_order(
        book,
        {
            .id = 2,
            .side = exchange::Side::Sell,
            .type = exchange::OrderType::Limit,
            .time_in_force =
                exchange::TimeInForce::GoodTillCancel,
            .price = 105,
            .initial_quantity = 15,
            .remaining_quantity = 15,
            .timestamp = 2
        }
    );

    std::cout << "ExchangeLab Cancel/Replace Demo\n";
    std::cout << "===============================\n";

    std::cout
        << "Original order 1: buy 20 @ 100\n";

    const exchange::ReplaceResult result =
        engine.replace_order(
            book,
            1,
            105,
            20,
            3
        );

    std::cout
        << "Replace order 1 with buy 20 @ 105: "
        << (result.replaced ? "successful" : "not found")
        << '\n';

    for (const exchange::Trade& trade : result.trades) {
        std::cout
            << "Trade: "
            << trade.quantity
            << " units at "
            << trade.price
            << '\n';
    }

    const exchange::Order* remaining =
        book.find_order(1);

    if (remaining != nullptr) {
        std::cout
            << "Remaining replaced order: "
            << remaining->remaining_quantity
            << " units at "
            << remaining->price
            << '\n';
    } else {
        std::cout
            << "Replacement was completely filled.\n";
    }

    return 0;
}