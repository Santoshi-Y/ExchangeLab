#pragma once

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "exchange/order_book.hpp"
#include "exchange/trade.hpp"
#include "exchange/trade_buffer.hpp"

namespace exchange {

struct ReplaceResult {
    bool replaced {false};
    std::vector<Trade> trades;
};

class MatchingEngine {
public:
    using BufferedTrades = DefaultTradeBuffer;

    /*
     * Legacy convenience API.
     *
     * This preserves all existing callers and tests.
     * It converts the inline buffer into std::vector,
     * so executions may allocate.
     */
    [[nodiscard]] std::vector<Trade> process_order(
        OrderBook& book,
        Order incoming
    ) {
        BufferedTrades trades;

        process_order_into(
            book,
            std::move(incoming),
            trades
        );

        return trades.to_vector();
    }

    /*
     * Allocation-optimized API.
     *
     * The first eight trades are stored inline.
     * Reusing the same buffer also retains overflow
     * capacity for large sweeps.
     */
    void process_order_into(
        OrderBook& book,
        Order incoming,
        BufferedTrades& trades
    ) {
        validate_incoming(incoming);
        trades.clear();

        if (incoming.side == Side::Buy) {
            match_buy(
                book,
                std::move(incoming),
                trades
            );

            return;
        }

        match_sell(
            book,
            std::move(incoming),
            trades
        );
    }

    [[nodiscard]] bool cancel_order(
        OrderBook& book,
        OrderId order_id
    ) {
        return book.cancel_order(order_id);
    }

    /*
     * Legacy replacement API.
     */
    [[nodiscard]] ReplaceResult replace_order(
        OrderBook& book,
        OrderId order_id,
        Price new_price,
        Quantity new_quantity,
        Timestamp new_timestamp
    ) {
        BufferedTrades trades;

        const bool replaced = replace_order_into(
            book,
            order_id,
            new_price,
            new_quantity,
            new_timestamp,
            trades
        );

        return {
            .replaced = replaced,
            .trades = trades.to_vector()
        };
    }

    /*
     * Allocation-optimized replacement API.
     */
    [[nodiscard]] bool replace_order_into(
        OrderBook& book,
        OrderId order_id,
        Price new_price,
        Quantity new_quantity,
        Timestamp new_timestamp,
        BufferedTrades& trades
    ) {
        if (new_quantity == 0) {
            throw std::invalid_argument(
                "Replacement quantity must be positive"
            );
        }

        trades.clear();

        std::optional<Order> existing =
            book.extract_order(order_id);

        if (!existing.has_value()) {
            return false;
        }

        Order replacement = *existing;

        replacement.price = new_price;
        replacement.initial_quantity = new_quantity;
        replacement.remaining_quantity = new_quantity;
        replacement.timestamp = new_timestamp;

        process_order_into(
            book,
            std::move(replacement),
            trades
        );

        return true;
    }

private:
    static void validate_incoming(
        const Order& incoming
    ) {
        if (incoming.remaining_quantity == 0) {
            throw std::invalid_argument(
                "Incoming order quantity must be positive"
            );
        }
    }

    [[nodiscard]] static bool buy_can_match(
        const Order& incoming,
        const OrderBook& book
    ) {
        if (!book.has_asks()) {
            return false;
        }

        if (incoming.type == OrderType::Market) {
            return true;
        }

        return incoming.price >= book.best_ask();
    }

    [[nodiscard]] static bool sell_can_match(
        const Order& incoming,
        const OrderBook& book
    ) {
        if (!book.has_bids()) {
            return false;
        }

        if (incoming.type == OrderType::Market) {
            return true;
        }

        return incoming.price <= book.best_bid();
    }

    static void match_buy(
        OrderBook& book,
        Order incoming,
        BufferedTrades& trades
    ) {
        while (
            incoming.remaining_quantity > 0 &&
            buy_can_match(incoming, book)
        ) {
            PriceLevel& ask_level =
                book.best_ask_level();

            const Order& resting_sell =
                ask_level.front();

            const OrderId resting_order_id =
                resting_sell.id;

            const Quantity resting_quantity =
                resting_sell.remaining_quantity;

            const Quantity trade_quantity =
                std::min(
                    incoming.remaining_quantity,
                    resting_quantity
                );

            trades.push_back({
                .buy_order_id = incoming.id,
                .sell_order_id = resting_order_id,
                .price = ask_level.price(),
                .quantity = trade_quantity,
                .timestamp = incoming.timestamp
            });

            incoming.remaining_quantity -=
                trade_quantity;

            ask_level.fill_front(trade_quantity);

            if (
                trade_quantity ==
                resting_quantity
            ) {
                book.remove_from_index(
                    resting_order_id
                );
            }

            book.remove_best_ask_if_empty();
        }

        if (
            incoming.remaining_quantity > 0 &&
            incoming.type == OrderType::Limit
        ) {
            book.add_order(std::move(incoming));
        }
    }

    static void match_sell(
        OrderBook& book,
        Order incoming,
        BufferedTrades& trades
    ) {
        while (
            incoming.remaining_quantity > 0 &&
            sell_can_match(incoming, book)
        ) {
            PriceLevel& bid_level =
                book.best_bid_level();

            const Order& resting_buy =
                bid_level.front();

            const OrderId resting_order_id =
                resting_buy.id;

            const Quantity resting_quantity =
                resting_buy.remaining_quantity;

            const Quantity trade_quantity =
                std::min(
                    incoming.remaining_quantity,
                    resting_quantity
                );

            trades.push_back({
                .buy_order_id = resting_order_id,
                .sell_order_id = incoming.id,
                .price = bid_level.price(),
                .quantity = trade_quantity,
                .timestamp = incoming.timestamp
            });

            incoming.remaining_quantity -=
                trade_quantity;

            bid_level.fill_front(trade_quantity);

            if (
                trade_quantity ==
                resting_quantity
            ) {
                book.remove_from_index(
                    resting_order_id
                );
            }

            book.remove_best_bid_if_empty();
        }

        if (
            incoming.remaining_quantity > 0 &&
            incoming.type == OrderType::Limit
        ) {
            book.add_order(std::move(incoming));
        }
    }
};

}  // namespace exchange