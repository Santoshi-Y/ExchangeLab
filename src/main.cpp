#include <iostream>

#include "exchange/types.hpp"

int main() {
    const exchange::OrderId order_id{1};
    const exchange::Price price{10'025};
    const exchange::Quantity quantity{100};
    const exchange::Side side{exchange::Side::Buy};

    std::cout << "ExchangeLab initialized.\n";
    std::cout << "Order ID: " << order_id << '\n';
    std::cout << "Price: " << price << '\n';
    std::cout << "Quantity: " << quantity << '\n';
    std::cout << "Side: "
              << (side == exchange::Side::Buy ? "Buy" : "Sell")
              << '\n';

    return 0;
}