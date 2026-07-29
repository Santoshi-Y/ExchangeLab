#pragma once

#include "exchange/types.hpp"

namespace exchange {

struct Order {
    OrderId id;
    Side side;
    OrderType type;
    TimeInForce time_in_force;
    Price price;
    Quantity initial_quantity;
    Quantity remaining_quantity;
    Timestamp timestamp;

    [[nodiscard]] bool is_filled() const noexcept {
        return remaining_quantity == 0;
    }
};

}  // namespace exchange