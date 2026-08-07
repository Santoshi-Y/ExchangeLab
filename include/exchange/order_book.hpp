#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "exchange/memory_pool.hpp"
#include "exchange/price_level.hpp"

namespace exchange {

/*
 * OrderBook stores its hot-path container nodes in a per-book PMR pool.
 *
 * The State indirection is deliberate. std::pmr containers are tied to their
 * memory_resource, while journal recovery needs OrderBook::swap(). Swapping
 * the single State pointer moves the pool and every container together, so
 * iterators in order_locations_ remain valid and allocator ownership stays
 * correct.
 */
class OrderBook {
public:
    using BidLevels = std::pmr::map<
        Price,
        PriceLevel,
        std::greater<Price>
    >;

    using AskLevels = std::pmr::map<
        Price,
        PriceLevel,
        std::less<Price>
    >;

    OrderBook()
        : state_(std::make_unique<State>()) {}

    ~OrderBook() = default;

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    void add_order(Order order) {
        if (contains(order.id)) {
            throw std::invalid_argument(
                "Duplicate order ID"
            );
        }

        if (order.type != OrderType::Limit) {
            throw std::invalid_argument(
                "Only limit orders may rest on the order book"
            );
        }

        if (order.remaining_quantity == 0) {
            throw std::invalid_argument(
                "Order quantity must be positive"
            );
        }

        const OrderId order_id = order.id;
        const Side side = order.side;
        const Price price = order.price;

        if (side == Side::Buy) {
            auto level_result = state_->bids.try_emplace(
                price,
                price,
                state_->memory_pool.resource()
            );

            auto level_iterator = level_result.first;

            auto order_iterator =
                level_iterator->second.add_order(
                    std::move(order)
                );

            state_->order_locations.emplace(
                order_id,
                OrderLocation {
                    .side = side,
                    .price = price,
                    .order_iterator = order_iterator
                }
            );

            return;
        }

        auto level_result = state_->asks.try_emplace(
            price,
            price,
            state_->memory_pool.resource()
        );

        auto level_iterator = level_result.first;

        auto order_iterator =
            level_iterator->second.add_order(
                std::move(order)
            );

        state_->order_locations.emplace(
            order_id,
            OrderLocation {
                .side = side,
                .price = price,
                .order_iterator = order_iterator
            }
        );
    }

    [[nodiscard]] bool cancel_order(OrderId order_id) {
        return extract_order(order_id).has_value();
    }

    [[nodiscard]] std::optional<Order> extract_order(
        OrderId order_id
    ) {
        const auto location_iterator =
            state_->order_locations.find(order_id);

        if (
            location_iterator ==
            state_->order_locations.end()
        ) {
            return std::nullopt;
        }

        const OrderLocation location =
            location_iterator->second;

        Order extracted_order = *location.order_iterator;

        if (location.side == Side::Buy) {
            auto level_iterator =
                state_->bids.find(location.price);

            if (level_iterator == state_->bids.end()) {
                throw std::logic_error(
                    "Bid order index is inconsistent"
                );
            }

            level_iterator->second.erase(
                location.order_iterator
            );

            if (level_iterator->second.empty()) {
                state_->bids.erase(level_iterator);
            }
        } else {
            auto level_iterator =
                state_->asks.find(location.price);

            if (level_iterator == state_->asks.end()) {
                throw std::logic_error(
                    "Ask order index is inconsistent"
                );
            }

            level_iterator->second.erase(
                location.order_iterator
            );

            if (level_iterator->second.empty()) {
                state_->asks.erase(level_iterator);
            }
        }

        state_->order_locations.erase(location_iterator);

        return extracted_order;
    }

    [[nodiscard]] bool contains(
        OrderId order_id
    ) const noexcept {
        return state_->order_locations.contains(order_id);
    }

    [[nodiscard]] const Order* find_order(
        OrderId order_id
    ) const noexcept {
        const auto location_iterator =
            state_->order_locations.find(order_id);

        if (
            location_iterator ==
            state_->order_locations.end()
        ) {
            return nullptr;
        }

        return &*location_iterator->second.order_iterator;
    }

    [[nodiscard]] Order* find_order(
        OrderId order_id
    ) noexcept {
        const auto location_iterator =
            state_->order_locations.find(order_id);

        if (
            location_iterator ==
            state_->order_locations.end()
        ) {
            return nullptr;
        }

        return &*location_iterator->second.order_iterator;
    }

    [[nodiscard]] std::size_t order_count() const noexcept {
        return state_->order_locations.size();
    }

    void swap(OrderBook& other) noexcept {
        state_.swap(other.state_);
    }

    [[nodiscard]] bool empty() const noexcept {
        return state_->bids.empty() && state_->asks.empty();
    }

    [[nodiscard]] bool has_bids() const noexcept {
        return !state_->bids.empty();
    }

    [[nodiscard]] bool has_asks() const noexcept {
        return !state_->asks.empty();
    }

    [[nodiscard]] Price best_bid() const {
        if (state_->bids.empty()) {
            throw std::out_of_range(
                "Order book has no bids"
            );
        }

        return state_->bids.begin()->first;
    }

    [[nodiscard]] Price best_ask() const {
        if (state_->asks.empty()) {
            throw std::out_of_range(
                "Order book has no asks"
            );
        }

        return state_->asks.begin()->first;
    }

    [[nodiscard]] PriceLevel& best_bid_level() {
        if (state_->bids.empty()) {
            throw std::out_of_range(
                "Order book has no bids"
            );
        }

        return state_->bids.begin()->second;
    }

    [[nodiscard]] const PriceLevel&
    best_bid_level() const {
        if (state_->bids.empty()) {
            throw std::out_of_range(
                "Order book has no bids"
            );
        }

        return state_->bids.begin()->second;
    }

    [[nodiscard]] PriceLevel& best_ask_level() {
        if (state_->asks.empty()) {
            throw std::out_of_range(
                "Order book has no asks"
            );
        }

        return state_->asks.begin()->second;
    }

    [[nodiscard]] const PriceLevel&
    best_ask_level() const {
        if (state_->asks.empty()) {
            throw std::out_of_range(
                "Order book has no asks"
            );
        }

        return state_->asks.begin()->second;
    }

    [[nodiscard]] std::uint64_t executable_quantity(
        const Order& incoming
    ) const noexcept {
        std::uint64_t available = 0;

        if (incoming.side == Side::Buy) {
            for (
                const auto& [price, level] :
                state_->asks
            ) {
                if (
                    incoming.type == OrderType::Limit &&
                    price > incoming.price
                ) {
                    break;
                }

                available += static_cast<std::uint64_t>(
                    level.total_quantity()
                );

                if (
                    available >=
                    static_cast<std::uint64_t>(
                        incoming.remaining_quantity
                    )
                ) {
                    break;
                }
            }

            return available;
        }

        for (
            const auto& [price, level] : state_->bids
        ) {
            if (
                incoming.type == OrderType::Limit &&
                price < incoming.price
            ) {
                break;
            }

            available += static_cast<std::uint64_t>(
                level.total_quantity()
            );

            if (
                available >=
                static_cast<std::uint64_t>(
                    incoming.remaining_quantity
                )
            ) {
                break;
            }
        }

        return available;
    }

    void remove_best_bid_if_empty() {
        if (
            !state_->bids.empty() &&
            state_->bids.begin()->second.empty()
        ) {
            state_->bids.erase(state_->bids.begin());
        }
    }

    void remove_best_ask_if_empty() {
        if (
            !state_->asks.empty() &&
            state_->asks.begin()->second.empty()
        ) {
            state_->asks.erase(state_->asks.begin());
        }
    }

    void remove_from_index(OrderId order_id) {
        state_->order_locations.erase(order_id);
    }

    [[nodiscard]] MemoryPoolStats
    memory_pool_stats() const noexcept {
        return state_->memory_pool.stats();
    }

private:
    struct OrderLocation {
        Side side;
        Price price;
        PriceLevel::iterator order_iterator;
    };

    struct State {
        State()
            : memory_pool(),
              bids(
                  std::greater<Price> {},
                  memory_pool.resource()
              ),
              asks(
                  std::less<Price> {},
                  memory_pool.resource()
              ),
              order_locations(
                  0,
                  std::hash<OrderId> {},
                  std::equal_to<OrderId> {},
                  memory_pool.resource()
              ) {}

        MemoryPool memory_pool;
        BidLevels bids;
        AskLevels asks;
        std::pmr::unordered_map<
            OrderId,
            OrderLocation
        > order_locations;
    };

    std::unique_ptr<State> state_;
};

}  // namespace exchange
