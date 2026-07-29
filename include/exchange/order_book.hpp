#pragma once

#include <map>

#include "exchange/price_level.hpp"

namespace exchange {

class OrderBook {
public:
    void add_order(const Order& order) {
        if (order.side == Side::Buy) {
            auto it = bids_.find(order.price);

            if (it == bids_.end()) {
                it = bids_.emplace(order.price, PriceLevel(order.price)).first;
            }

            it->second.add_order(order);
        } else {
            auto it = asks_.find(order.price);

            if (it == asks_.end()) {
                it = asks_.emplace(order.price, PriceLevel(order.price)).first;
            }

            it->second.add_order(order);
        }
    }

    [[nodiscard]] bool empty() const noexcept {
        return bids_.empty() && asks_.empty();
    }

    [[nodiscard]] const auto& bids() const noexcept {
        return bids_;
    }

    [[nodiscard]] const auto& asks() const noexcept {
        return asks_;
    }

private:
    std::map<Price, PriceLevel, std::greater<>> bids_;
    std::map<Price, PriceLevel> asks_;
};

} // namespace exchange