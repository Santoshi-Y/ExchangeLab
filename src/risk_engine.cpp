#include "exchange/risk_engine.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <string>

namespace exchange {

namespace {

RiskDecision accept() noexcept {
    return {
        .accepted = true,
        .reason = RiskRejectReason::None
    };
}

RiskDecision reject(
    RiskRejectReason reason
) noexcept {
    return {
        .accepted = false,
        .reason = reason
    };
}

}  // namespace

const char* to_string(
    RiskRejectReason reason
) noexcept {
    switch (reason) {
        case RiskRejectReason::None:
            return "none";
        case RiskRejectReason::GlobalKillSwitch:
            return "global kill switch";
        case RiskRejectReason::ClientKillSwitch:
            return "client kill switch";
        case RiskRejectReason::MaxOrderQuantity:
            return "max order quantity";
        case RiskRejectReason::MaxOrderNotional:
            return "max order notional";
        case RiskRejectReason::MaxOpenOrders:
            return "max open orders";
        case RiskRejectReason::MaxOpenQuantity:
            return "max open quantity";
        case RiskRejectReason::MaxPosition:
            return "max position";
        case RiskRejectReason::UnknownWorkingOrder:
            return "unknown working order";
    }

    return "unknown risk rejection";
}

RiskEngine::RiskEngine(
    RiskLimits limits
)
    : limits_(limits) {}

const RiskLimits& RiskEngine::limits() const noexcept {
    return limits_;
}

RiskDecision RiskEngine::check_new_order(
    const RiskOrderRequest& request
) const {
    if (global_kill_switch_) {
        return reject(
            RiskRejectReason::GlobalKillSwitch
        );
    }

    const ClientState* client =
        find_client(request.client_id);

    if (
        client != nullptr &&
        client->kill_switch
    ) {
        return reject(
            RiskRejectReason::ClientKillSwitch
        );
    }

    const RiskDecision common =
        check_common_order_limits(
            client,
            request.price,
            request.quantity,
            request.reference_price
        );

    if (!common.accepted) {
        return common;
    }

    const SymbolState* symbol_state = nullptr;

    if (client != nullptr) {
        symbol_state = find_symbol_state(
            *client,
            request.symbol
        );
    }

    const std::uint64_t current_open_buy =
        symbol_state == nullptr
            ? 0
            : symbol_state->open_buy_quantity;

    const std::uint64_t current_open_sell =
        symbol_state == nullptr
            ? 0
            : symbol_state->open_sell_quantity;

    if (
        position_limit_exceeded(
            symbol_state,
            request.side,
            request.quantity,
            current_open_buy,
            current_open_sell
        )
    ) {
        return reject(
            RiskRejectReason::MaxPosition
        );
    }

    if (
        may_rest(
            request.order_type,
            request.time_in_force
        )
    ) {
        const std::size_t open_orders =
            client == nullptr
                ? 0
                : client->open_orders;

        if (
            open_orders >=
            limits_.max_open_orders
        ) {
            return reject(
                RiskRejectReason::MaxOpenOrders
            );
        }

        const std::uint64_t gross_open_quantity =
            client == nullptr
                ? 0
                : client->gross_open_quantity;

        if (
            saturating_add(
                gross_open_quantity,
                request.quantity
            ) > limits_.max_open_quantity
        ) {
            return reject(
                RiskRejectReason::MaxOpenQuantity
            );
        }
    }

    return accept();
}

RiskDecision RiskEngine::check_replace(
    RiskClientId client_id,
    std::string_view symbol,
    OrderId order_id,
    Price new_price,
    Quantity new_quantity
) const {
    if (global_kill_switch_) {
        return reject(
            RiskRejectReason::GlobalKillSwitch
        );
    }

    const ClientState* client =
        find_client(client_id);

    if (
        client != nullptr &&
        client->kill_switch
    ) {
        return reject(
            RiskRejectReason::ClientKillSwitch
        );
    }

    const WorkingOrderKey key {
        .client_id = client_id,
        .symbol = std::string(symbol),
        .order_id = order_id
    };

    const auto working_iterator =
        working_orders_.find(key);

    if (working_iterator == working_orders_.end()) {
        return reject(
            RiskRejectReason::UnknownWorkingOrder
        );
    }

    const WorkingOrder& existing =
        working_iterator->second;

    const RiskDecision common =
        check_common_order_limits(
            client,
            new_price,
            new_quantity,
            std::nullopt
        );

    if (!common.accepted) {
        return common;
    }

    if (client == nullptr) {
        return reject(
            RiskRejectReason::UnknownWorkingOrder
        );
    }

    if (client->open_orders > limits_.max_open_orders) {
        return reject(
            RiskRejectReason::MaxOpenOrders
        );
    }

    std::uint64_t projected_gross =
        client->gross_open_quantity;

    if (
        projected_gross >=
        existing.remaining_quantity
    ) {
        projected_gross -=
            existing.remaining_quantity;
    } else {
        projected_gross = 0;
    }

    projected_gross = saturating_add(
        projected_gross,
        new_quantity
    );

    if (
        projected_gross >
        limits_.max_open_quantity
    ) {
        return reject(
            RiskRejectReason::MaxOpenQuantity
        );
    }

    const SymbolState* symbol_state =
        find_symbol_state(
            *client,
            symbol
        );

    if (symbol_state == nullptr) {
        return reject(
            RiskRejectReason::UnknownWorkingOrder
        );
    }

    std::uint64_t adjusted_open_buy =
        symbol_state->open_buy_quantity;

    std::uint64_t adjusted_open_sell =
        symbol_state->open_sell_quantity;

    if (existing.side == Side::Buy) {
        adjusted_open_buy =
            adjusted_open_buy >= existing.remaining_quantity
                ? adjusted_open_buy - existing.remaining_quantity
                : 0;
    } else {
        adjusted_open_sell =
            adjusted_open_sell >= existing.remaining_quantity
                ? adjusted_open_sell - existing.remaining_quantity
                : 0;
    }

    if (
        position_limit_exceeded(
            symbol_state,
            existing.side,
            new_quantity,
            adjusted_open_buy,
            adjusted_open_sell
        )
    ) {
        return reject(
            RiskRejectReason::MaxPosition
        );
    }

    return accept();
}

void RiskEngine::on_order_resting(
    RiskClientId client_id,
    std::string_view symbol,
    OrderId order_id,
    Side side,
    Price price,
    Quantity remaining_quantity
) {
    if (
        client_id == invalid_risk_client_id ||
        remaining_quantity == 0
    ) {
        return;
    }

    const WorkingOrderKey key {
        .client_id = client_id,
        .symbol = std::string(symbol),
        .order_id = order_id
    };

    erase_working_order(key);

    ClientState& client =
        client_for(client_id);

    SymbolState& symbol_state =
        client.symbols.try_emplace(
            std::string(symbol)
        ).first->second;

    ++client.open_orders;
    client.gross_open_quantity = saturating_add(
        client.gross_open_quantity,
        remaining_quantity
    );

    if (side == Side::Buy) {
        symbol_state.open_buy_quantity = saturating_add(
            symbol_state.open_buy_quantity,
            remaining_quantity
        );
    } else {
        symbol_state.open_sell_quantity = saturating_add(
            symbol_state.open_sell_quantity,
            remaining_quantity
        );
    }

    working_orders_.emplace(
        key,
        WorkingOrder {
            .side = side,
            .price = price,
            .remaining_quantity = remaining_quantity
        }
    );
}

void RiskEngine::on_order_cancelled(
    RiskClientId client_id,
    std::string_view symbol,
    OrderId order_id
) {
    if (client_id == invalid_risk_client_id) {
        return;
    }

    erase_working_order({
        .client_id = client_id,
        .symbol = std::string(symbol),
        .order_id = order_id
    });
}

void RiskEngine::on_trade(
    std::string_view symbol,
    RiskClientId buyer_client_id,
    RiskClientId seller_client_id,
    OrderId buy_order_id,
    OrderId sell_order_id,
    Quantity quantity
) {
    if (quantity == 0) {
        return;
    }

    if (buyer_client_id != invalid_risk_client_id) {
        ClientState& buyer =
            client_for(buyer_client_id);

        SymbolState& state =
            buyer.symbols.try_emplace(
                std::string(symbol)
            ).first->second;

        apply_position_delta(
            state,
            Side::Buy,
            quantity
        );

        reduce_working_order(
            buyer_client_id,
            symbol,
            buy_order_id,
            quantity
        );
    }

    if (seller_client_id != invalid_risk_client_id) {
        ClientState& seller =
            client_for(seller_client_id);

        SymbolState& state =
            seller.symbols.try_emplace(
                std::string(symbol)
            ).first->second;

        apply_position_delta(
            state,
            Side::Sell,
            quantity
        );

        reduce_working_order(
            seller_client_id,
            symbol,
            sell_order_id,
            quantity
        );
    }
}

void RiskEngine::set_global_kill_switch(
    bool enabled
) noexcept {
    global_kill_switch_ = enabled;
}

bool RiskEngine::global_kill_switch() const noexcept {
    return global_kill_switch_;
}

void RiskEngine::set_client_kill_switch(
    RiskClientId client_id,
    bool enabled
) {
    if (client_id == invalid_risk_client_id) {
        return;
    }

    client_for(client_id).kill_switch = enabled;
}

ClientRiskSnapshot RiskEngine::snapshot(
    RiskClientId client_id,
    std::string_view symbol
) const {
    ClientRiskSnapshot snapshot;
    snapshot.global_kill_switch = global_kill_switch_;

    const ClientState* client =
        find_client(client_id);

    if (client == nullptr) {
        return snapshot;
    }

    snapshot.client_kill_switch =
        client->kill_switch;
    snapshot.open_orders =
        client->open_orders;
    snapshot.gross_open_quantity =
        client->gross_open_quantity;

    const SymbolState* symbol_state =
        find_symbol_state(*client, symbol);

    if (symbol_state == nullptr) {
        return snapshot;
    }

    snapshot.position =
        symbol_state->position;
    snapshot.open_buy_quantity =
        symbol_state->open_buy_quantity;
    snapshot.open_sell_quantity =
        symbol_state->open_sell_quantity;

    return snapshot;
}

std::size_t RiskEngine::WorkingOrderKeyHash::operator()(
    const WorkingOrderKey& key
) const noexcept {
    std::size_t seed =
        std::hash<RiskClientId> {}(
            key.client_id
        );

    const std::size_t symbol_hash =
        std::hash<std::string> {}(
            key.symbol
        );

    const std::size_t order_hash =
        std::hash<OrderId> {}(
            key.order_id
        );

    seed ^= symbol_hash +
        0x9e3779b9U +
        (seed << 6U) +
        (seed >> 2U);

    seed ^= order_hash +
        0x9e3779b9U +
        (seed << 6U) +
        (seed >> 2U);

    return seed;
}

const RiskEngine::ClientState*
RiskEngine::find_client(
    RiskClientId client_id
) const noexcept {
    const auto iterator =
        clients_.find(client_id);

    return iterator == clients_.end()
        ? nullptr
        : &iterator->second;
}

RiskEngine::ClientState& RiskEngine::client_for(
    RiskClientId client_id
) {
    return clients_.try_emplace(
        client_id
    ).first->second;
}

const RiskEngine::SymbolState*
RiskEngine::find_symbol_state(
    const ClientState& client,
    std::string_view symbol
) const noexcept {
    const auto iterator =
        client.symbols.find(
            std::string(symbol)
        );

    return iterator == client.symbols.end()
        ? nullptr
        : &iterator->second;
}

RiskDecision RiskEngine::check_common_order_limits(
    const ClientState*,
    Price price,
    Quantity quantity,
    std::optional<Price> reference_price
) const noexcept {
    if (quantity > limits_.max_order_quantity) {
        return reject(
            RiskRejectReason::MaxOrderQuantity
        );
    }

    Price notional_price = price;

    if (reference_price.has_value()) {
        notional_price = *reference_price;
    }

    const std::uint64_t price_magnitude =
        absolute_price(notional_price);

    if (
        price_magnitude != 0 &&
        static_cast<std::uint64_t>(quantity) >
            limits_.max_order_notional /
                price_magnitude
    ) {
        return reject(
            RiskRejectReason::MaxOrderNotional
        );
    }

    return accept();
}

bool RiskEngine::position_limit_exceeded(
    const SymbolState* symbol_state,
    Side side,
    Quantity additional_quantity,
    std::uint64_t open_buy_quantity,
    std::uint64_t open_sell_quantity
) const noexcept {
    const std::int64_t position =
        symbol_state == nullptr
            ? 0
            : symbol_state->position;

    if (side == Side::Buy) {
        const std::uint64_t buy_exposure =
            saturating_add(
                open_buy_quantity,
                additional_quantity
            );

        std::uint64_t projected_long = 0;

        if (position >= 0) {
            projected_long = saturating_add(
                static_cast<std::uint64_t>(position),
                buy_exposure
            );
        } else {
            const std::uint64_t short_position =
                absolute_position(position);

            if (buy_exposure > short_position) {
                projected_long =
                    buy_exposure - short_position;
            }
        }

        return projected_long >
            limits_.max_position_per_symbol;
    }

    const std::uint64_t sell_exposure =
        saturating_add(
            open_sell_quantity,
            additional_quantity
        );

    std::uint64_t projected_short = 0;

    if (position <= 0) {
        projected_short = saturating_add(
            absolute_position(position),
            sell_exposure
        );
    } else {
        const auto long_position =
            static_cast<std::uint64_t>(position);

        if (sell_exposure > long_position) {
            projected_short =
                sell_exposure - long_position;
        }
    }

    return projected_short >
        limits_.max_position_per_symbol;
}

void RiskEngine::erase_working_order(
    const WorkingOrderKey& key
) {
    const auto iterator =
        working_orders_.find(key);

    if (iterator == working_orders_.end()) {
        return;
    }

    const WorkingOrder order =
        iterator->second;

    auto client_iterator =
        clients_.find(key.client_id);

    if (client_iterator != clients_.end()) {
        ClientState& client =
            client_iterator->second;

        if (client.open_orders > 0) {
            --client.open_orders;
        }

        client.gross_open_quantity =
            client.gross_open_quantity >=
                    order.remaining_quantity
                ? client.gross_open_quantity -
                    order.remaining_quantity
                : 0;

        auto symbol_iterator =
            client.symbols.find(key.symbol);

        if (symbol_iterator != client.symbols.end()) {
            SymbolState& state =
                symbol_iterator->second;

            if (order.side == Side::Buy) {
                state.open_buy_quantity =
                    state.open_buy_quantity >=
                            order.remaining_quantity
                        ? state.open_buy_quantity -
                            order.remaining_quantity
                        : 0;
            } else {
                state.open_sell_quantity =
                    state.open_sell_quantity >=
                            order.remaining_quantity
                        ? state.open_sell_quantity -
                            order.remaining_quantity
                        : 0;
            }
        }
    }

    working_orders_.erase(iterator);
}

void RiskEngine::reduce_working_order(
    RiskClientId client_id,
    std::string_view symbol,
    OrderId order_id,
    Quantity executed_quantity
) {
    const WorkingOrderKey key {
        .client_id = client_id,
        .symbol = std::string(symbol),
        .order_id = order_id
    };

    auto iterator = working_orders_.find(key);

    if (iterator == working_orders_.end()) {
        return;
    }

    WorkingOrder& order = iterator->second;

    const Quantity reduction =
        std::min(
            order.remaining_quantity,
            executed_quantity
        );

    ClientState& client =
        client_for(client_id);

    SymbolState& state =
        client.symbols.try_emplace(
            std::string(symbol)
        ).first->second;

    client.gross_open_quantity =
        client.gross_open_quantity >= reduction
            ? client.gross_open_quantity - reduction
            : 0;

    if (order.side == Side::Buy) {
        state.open_buy_quantity =
            state.open_buy_quantity >= reduction
                ? state.open_buy_quantity - reduction
                : 0;
    } else {
        state.open_sell_quantity =
            state.open_sell_quantity >= reduction
                ? state.open_sell_quantity - reduction
                : 0;
    }

    order.remaining_quantity -= reduction;

    if (order.remaining_quantity == 0) {
        if (client.open_orders > 0) {
            --client.open_orders;
        }

        working_orders_.erase(iterator);
    }
}

void RiskEngine::apply_position_delta(
    SymbolState& state,
    Side side,
    Quantity quantity
) noexcept {
    const auto quantity64 =
        static_cast<std::int64_t>(quantity);

    if (side == Side::Buy) {
        if (
            state.position >
            std::numeric_limits<std::int64_t>::max() -
                quantity64
        ) {
            state.position =
                std::numeric_limits<std::int64_t>::max();
        } else {
            state.position += quantity64;
        }

        return;
    }

    if (
        state.position <
        std::numeric_limits<std::int64_t>::min() +
            quantity64
    ) {
        state.position =
            std::numeric_limits<std::int64_t>::min();
    } else {
        state.position -= quantity64;
    }
}

bool RiskEngine::may_rest(
    OrderType order_type,
    TimeInForce time_in_force
) noexcept {
    return
        order_type == OrderType::Limit &&
        time_in_force ==
            TimeInForce::GoodTillCancel;
}

std::uint64_t RiskEngine::saturating_add(
    std::uint64_t left,
    std::uint64_t right
) noexcept {
    const std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();

    if (right > maximum - left) {
        return maximum;
    }

    return left + right;
}

std::uint64_t RiskEngine::absolute_position(
    std::int64_t value
) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }

    if (
        value ==
        std::numeric_limits<std::int64_t>::min()
    ) {
        return std::uint64_t {1} << 63U;
    }

    return static_cast<std::uint64_t>(-value);
}

std::uint64_t RiskEngine::absolute_price(
    Price value
) noexcept {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }

    if (
        value ==
        std::numeric_limits<Price>::min()
    ) {
        return std::uint64_t {1} << 63U;
    }

    return static_cast<std::uint64_t>(-value);
}

}  // namespace exchange