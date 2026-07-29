#include <iostream>

#include "exchange/trade.hpp"

int main() {

    exchange::Trade trade{
        .buy_order_id = 12,
        .sell_order_id = 7,
        .price = 103,
        .quantity = 25,
        .timestamp = 15
    };

    std::cout << "Trade Executed\n";
    std::cout << "Buy Order: " << trade.buy_order_id << '\n';
    std::cout << "Sell Order: " << trade.sell_order_id << '\n';
    std::cout << "Price: " << trade.price << '\n';
    std::cout << "Quantity: " << trade.quantity << '\n';

    return 0;
}