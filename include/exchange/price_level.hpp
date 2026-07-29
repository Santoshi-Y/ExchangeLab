#pragma once

#include <cstddef>
#include <deque>

#include "exchange/order.hpp"

namespace exchange {

class PriceLevel {
public:
    explicit PriceLevel(Price price) noexcept
        : price_{price} {}

    void add_order(const Order& order) {
        orders_.push_back(order);
        total_quantity_ += order.remaining_quantity;
    }

    [[nodiscard]] bool empty() const noexcept {
        return orders_.empty();
    }

    [[nodiscard]] std::size_t order_count() const noexcept {
        return orders_.size();
    }

    [[nodiscard]] Price price() const noexcept {
        return price_;
    }

    [[nodiscard]] Quantity total_quantity() const noexcept {
        return total_quantity_;
    }

    [[nodiscard]] const Order& front() const {
        return orders_.front();
    }

private:
    Price price_;
    Quantity total_quantity_{0};
    std::deque<Order> orders_;
};

}  // namespace exchange