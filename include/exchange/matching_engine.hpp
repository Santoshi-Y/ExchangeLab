#pragma once

#include <vector>

#include "exchange/order_book.hpp"
#include "exchange/trade.hpp"

namespace exchange {

class MatchingEngine {
public:
    std::vector<Trade> match_buy(OrderBook& book, Order incoming) {

        std::vector<Trade> trades;

        while (incoming.remaining_quantity > 0 &&
               book.has_asks() &&
               incoming.price >= book.best_ask()) {

            auto& level =
                const_cast<PriceLevel&>(book.asks().begin()->second);

            auto quantity =
                std::min(incoming.remaining_quantity,
                         level.front().remaining_quantity);

            trades.push_back({
                .buy_order_id = incoming.id,
                .sell_order_id = level.front().id,
                .price = level.price(),
                .quantity = quantity,
                .timestamp = incoming.timestamp
            });

            incoming.remaining_quantity -= quantity;

            level.fill_front(quantity);
        }

        return trades;
    }
};

}