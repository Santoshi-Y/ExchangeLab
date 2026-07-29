#pragma once

#include <algorithm>
#include <vector>

#include "exchange/order_book.hpp"
#include "exchange/trade.hpp"

namespace exchange {

class MatchingEngine {
public:
    [[nodiscard]] std::vector<Trade> process_order(
        OrderBook& book,
        Order incoming
    ) {
        if (incoming.side == Side::Buy) {
            return match_buy(book, incoming);
        }

        return match_sell(book, incoming);
    }

private:
    [[nodiscard]] std::vector<Trade> match_buy(
        OrderBook& book,
        Order incoming
    ) {
        std::vector<Trade> trades;

        while (
            incoming.remaining_quantity > 0 &&
            book.has_asks() &&
            incoming.price >= book.best_ask()
        ) {
            PriceLevel& ask_level = book.best_ask_level();
            const Order& resting_sell = ask_level.front();

            const Quantity trade_quantity = std::min(
                incoming.remaining_quantity,
                resting_sell.remaining_quantity
            );

            trades.push_back({
                .buy_order_id = incoming.id,
                .sell_order_id = resting_sell.id,
                .price = ask_level.price(),
                .quantity = trade_quantity,
                .timestamp = incoming.timestamp
            });

            incoming.remaining_quantity -= trade_quantity;
            ask_level.fill_front(trade_quantity);

            book.remove_best_ask_if_empty();
        }

        // A limit order that was not completely filled rests on the book.
        if (incoming.remaining_quantity > 0) {
            book.add_order(incoming);
        }

        return trades;
    }

    [[nodiscard]] std::vector<Trade> match_sell(
        OrderBook& book,
        Order incoming
    ) {
        std::vector<Trade> trades;

        while (
            incoming.remaining_quantity > 0 &&
            book.has_bids() &&
            incoming.price <= book.best_bid()
        ) {
            PriceLevel& bid_level = book.best_bid_level();
            const Order& resting_buy = bid_level.front();

            const Quantity trade_quantity = std::min(
                incoming.remaining_quantity,
                resting_buy.remaining_quantity
            );

            trades.push_back({
                .buy_order_id = resting_buy.id,
                .sell_order_id = incoming.id,
                .price = bid_level.price(),
                .quantity = trade_quantity,
                .timestamp = incoming.timestamp
            });

            incoming.remaining_quantity -= trade_quantity;
            bid_level.fill_front(trade_quantity);

            book.remove_best_bid_if_empty();
        }

        // A limit order that was not completely filled rests on the book.
        if (incoming.remaining_quantity > 0) {
            book.add_order(incoming);
        }

        return trades;
    }
};

}  // namespace exchange