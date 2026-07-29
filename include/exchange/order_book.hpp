#pragma once

#include <functional>
#include <map>
#include <stdexcept>
#include <unordered_map>

#include "exchange/price_level.hpp"

namespace exchange {

class OrderBook {
public:
    using BidLevels = std::map<
        Price,
        PriceLevel,
        std::greater<Price>
    >;

    using AskLevels = std::map<
        Price,
        PriceLevel,
        std::less<Price>
    >;

    void add_order(Order order) {
        if (contains(order.id)) {
            throw std::invalid_argument(
                "Duplicate order ID"
            );
        }

        const OrderId order_id = order.id;
        const Side side = order.side;
        const Price price = order.price;

        if (side == Side::Buy) {
            auto [level_iterator, inserted] =
                bids_.try_emplace(price, price);

            auto order_iterator =
                level_iterator->second.add_order(
                    std::move(order)
                );

            order_locations_.emplace(
                order_id,
                OrderLocation{
                    .side = side,
                    .price = price,
                    .order_iterator = order_iterator
                }
            );

            return;
        }

        auto [level_iterator, inserted] =
            asks_.try_emplace(price, price);

        auto order_iterator =
            level_iterator->second.add_order(
                std::move(order)
            );

        order_locations_.emplace(
            order_id,
            OrderLocation{
                .side = side,
                .price = price,
                .order_iterator = order_iterator
            }
        );
    }

    [[nodiscard]] bool cancel_order(OrderId order_id) {
        const auto location_iterator =
            order_locations_.find(order_id);

        if (location_iterator == order_locations_.end()) {
            return false;
        }

        const OrderLocation location =
            location_iterator->second;

        if (location.side == Side::Buy) {
            auto level_iterator =
                bids_.find(location.price);

            if (level_iterator == bids_.end()) {
                throw std::logic_error(
                    "Bid order index is inconsistent"
                );
            }

            level_iterator->second.erase(
                location.order_iterator
            );

            if (level_iterator->second.empty()) {
                bids_.erase(level_iterator);
            }
        } else {
            auto level_iterator =
                asks_.find(location.price);

            if (level_iterator == asks_.end()) {
                throw std::logic_error(
                    "Ask order index is inconsistent"
                );
            }

            level_iterator->second.erase(
                location.order_iterator
            );

            if (level_iterator->second.empty()) {
                asks_.erase(level_iterator);
            }
        }

        order_locations_.erase(location_iterator);
        return true;
    }

    [[nodiscard]] bool contains(
        OrderId order_id
    ) const noexcept {
        return order_locations_.contains(order_id);
    }

    [[nodiscard]] std::size_t order_count() const noexcept {
        return order_locations_.size();
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
        if (bids_.empty()) {
            throw std::out_of_range(
                "Order book has no bids"
            );
        }

        return bids_.begin()->first;
    }

    [[nodiscard]] Price best_ask() const {
        if (asks_.empty()) {
            throw std::out_of_range(
                "Order book has no asks"
            );
        }

        return asks_.begin()->first;
    }

    [[nodiscard]] PriceLevel& best_bid_level() {
        if (bids_.empty()) {
            throw std::out_of_range(
                "Order book has no bids"
            );
        }

        return bids_.begin()->second;
    }

    [[nodiscard]] const PriceLevel&
    best_bid_level() const {
        if (bids_.empty()) {
            throw std::out_of_range(
                "Order book has no bids"
            );
        }

        return bids_.begin()->second;
    }

    [[nodiscard]] PriceLevel& best_ask_level() {
        if (asks_.empty()) {
            throw std::out_of_range(
                "Order book has no asks"
            );
        }

        return asks_.begin()->second;
    }

    [[nodiscard]] const PriceLevel&
    best_ask_level() const {
        if (asks_.empty()) {
            throw std::out_of_range(
                "Order book has no asks"
            );
        }

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

    void remove_from_index(OrderId order_id) {
        order_locations_.erase(order_id);
    }

private:
    struct OrderLocation {
        Side side;
        Price price;
        PriceLevel::iterator order_iterator;
    };

    BidLevels bids_;
    AskLevels asks_;

    std::unordered_map<
        OrderId,
        OrderLocation
    > order_locations_;
};

}  // namespace exchange