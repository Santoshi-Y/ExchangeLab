#pragma once

#include <cstddef>
#include <list>
#include <stdexcept>

#include "exchange/order.hpp"

namespace exchange {

class PriceLevel {
public:
    using OrderContainer = std::list<Order>;
    using iterator = OrderContainer::iterator;
    using const_iterator = OrderContainer::const_iterator;

    explicit PriceLevel(Price price)
        : price_(price) {}

    [[nodiscard]] Price price() const noexcept {
        return price_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return orders_.empty();
    }

    [[nodiscard]] std::size_t order_count() const noexcept {
        return orders_.size();
    }

    [[nodiscard]] Quantity total_quantity() const noexcept {
        return total_quantity_;
    }

    [[nodiscard]] Order& front() {
        if (orders_.empty()) {
            throw std::out_of_range(
                "Cannot access an empty price level"
            );
        }

        return orders_.front();
    }

    [[nodiscard]] const Order& front() const {
        if (orders_.empty()) {
            throw std::out_of_range(
                "Cannot access an empty price level"
            );
        }

        return orders_.front();
    }

    iterator add_order(Order order) {
        if (order.price != price_) {
            throw std::invalid_argument(
                "Order price does not match price level"
            );
        }

        if (order.remaining_quantity <= 0) {
            throw std::invalid_argument(
                "Order quantity must be positive"
            );
        }

        total_quantity_ += order.remaining_quantity;
        orders_.push_back(std::move(order));

        return std::prev(orders_.end());
    }

    void fill_front(Quantity quantity) {
        if (orders_.empty()) {
            throw std::out_of_range(
                "Cannot fill an empty price level"
            );
        }

        if (
            quantity <= 0 ||
            quantity > orders_.front().remaining_quantity
        ) {
            throw std::invalid_argument(
                "Invalid fill quantity"
            );
        }

        orders_.front().remaining_quantity -= quantity;
        total_quantity_ -= quantity;

        if (orders_.front().remaining_quantity == 0) {
            orders_.pop_front();
        }
    }

    void erase(iterator order_iterator) {
        total_quantity_ -=
            order_iterator->remaining_quantity;

        orders_.erase(order_iterator);
    }

private:
    Price price_;
    Quantity total_quantity_{0};
    OrderContainer orders_;
};

}  // namespace exchange