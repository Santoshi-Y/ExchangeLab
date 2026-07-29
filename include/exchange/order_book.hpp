#pragma once

#include <functional>
#include <map>

#include "exchange/price_level.hpp"

namespace exchange {

class OrderBook {
public:
    void add_order(const Order& order) {
        // bids_ and asks_ use different comparator types, so they cannot
        // be selected using a ternary expression.
        if (order.side == Side::Buy) {
            auto it = bids_.find(order.price);

            if (it == bids_.end()) {
                it = bids_
                         .emplace(
                             order.price,
                             PriceLevel{order.price}
                         )
                         .first;
            }

            it->second.add_order(order);
        } else {
            auto it = asks_.find(order.price);

            if (it == asks_.end()) {
                it = asks_
                         .emplace(
                             order.price,
                             PriceLevel{order.price}
                         )
                         .first;
            }

            it->second.add_order(order);
        }
    }

    [[nodiscard]] bool empty() const noexcept {
        return bids_.empty() && asks_.empty();
    }

    [[nodiscard]] bool has_bids() const noexcept {
        return !bids_.empty();
    }

    [[nodiscard]] bool has_asks() const noexcept {
        return !asks_.empty();
    }

    [[nodiscard]] Price best_bid() const {
        return bids_.begin()->first;
    }

    [[nodiscard]] Price best_ask() const {
        return asks_.begin()->first;
    }

    [[nodiscard]] PriceLevel& best_bid_level() {
        return bids_.begin()->second;
    }

    [[nodiscard]] const PriceLevel& best_bid_level() const {
        return bids_.begin()->second;
    }

    [[nodiscard]] PriceLevel& best_ask_level() {
        return asks_.begin()->second;
    }

    [[nodiscard]] const PriceLevel& best_ask_level() const {
        return asks_.begin()->second;
    }

    void remove_best_bid_if_empty() {
        if (
            !bids_.empty() &&
            bids_.begin()->second.empty()
        ) {
            bids_.erase(bids_.begin());
        }
    }

    void remove_best_ask_if_empty() {
        if (
            !asks_.empty() &&
            asks_.begin()->second.empty()
        ) {
            asks_.erase(asks_.begin());
        }
    }

    [[nodiscard]] const auto& bids() const noexcept {
        return bids_;
    }

    [[nodiscard]] const auto& asks() const noexcept {
        return asks_;
    }

private:
    // Highest bid appears first.
    std::map<
        Price,
        PriceLevel,
        std::greater<Price>
    > bids_;

    // Lowest ask appears first.
    std::map<Price, PriceLevel> asks_;
};

}  // namespace exchange