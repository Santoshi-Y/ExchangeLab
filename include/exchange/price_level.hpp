#pragma once

#include <cstddef>
#include <list>
#include <memory_resource>
#include <stdexcept>
#include <utility>

#include "exchange/order.hpp"

namespace exchange {

class PriceLevel {
public:
    using OrderContainer = std::pmr::list<Order>;
    using iterator = OrderContainer::iterator;
    using const_iterator = OrderContainer::const_iterator;

    explicit PriceLevel(
        Price price,
        std::pmr::memory_resource* memory_resource =
            std::pmr::get_default_resource()
    )
        : price_(price),
          total_quantity_(0),
          orders_(memory_resource) {}

    [[nodiscard]] Price price() const noexcept {
        return price_;
    }

    [[nodiscard]] Quantity total_quantity() const noexcept {
        return total_quantity_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return orders_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return orders_.size();
    }

    // Preserve the original ExchangeLab PriceLevel API used throughout
    // the existing test suite and order-book code.
    [[nodiscard]] std::size_t order_count() const noexcept {
        return orders_.size();
    }

    [[nodiscard]] const Order& front() const {
        if (orders_.empty()) {
            throw std::out_of_range("Price level is empty");
        }

        return orders_.front();
    }

    [[nodiscard]] Order& front() {
        if (orders_.empty()) {
            throw std::out_of_range("Price level is empty");
        }

        return orders_.front();
    }

    iterator add_order(Order order) {
        if (order.remaining_quantity == 0) {
            throw std::invalid_argument(
                "Order quantity must be positive"
            );
        }

        if (order.price != price_) {
            throw std::invalid_argument(
                "Order price does not match price level"
            );
        }

        total_quantity_ += order.remaining_quantity;
        orders_.push_back(std::move(order));

        auto iterator = orders_.end();
        --iterator;
        return iterator;
    }

    void erase(iterator order_iterator) {
        if (order_iterator == orders_.end()) {
            throw std::invalid_argument(
                "Cannot erase end iterator"
            );
        }

        if (order_iterator->remaining_quantity > total_quantity_) {
            throw std::logic_error(
                "Price level quantity is inconsistent"
            );
        }

        total_quantity_ -= order_iterator->remaining_quantity;
        orders_.erase(order_iterator);
    }

    void fill_front(Quantity quantity) {
        if (orders_.empty()) {
            throw std::out_of_range(
                "Cannot fill an empty price level"
            );
        }

        if (quantity == 0) {
            throw std::invalid_argument(
                "Fill quantity must be positive"
            );
        }

        Order& order = orders_.front();

        if (quantity > order.remaining_quantity) {
            throw std::invalid_argument(
                "Fill quantity exceeds remaining order quantity"
            );
        }

        order.remaining_quantity -= quantity;
        total_quantity_ -= quantity;

        if (order.is_filled()) {
            orders_.pop_front();
        }
    }

private:
    Price price_;
    Quantity total_quantity_;
    OrderContainer orders_;
};

}  // namespace exchange