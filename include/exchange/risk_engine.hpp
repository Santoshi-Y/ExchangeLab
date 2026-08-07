#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "exchange/types.hpp"

namespace exchange {

using RiskClientId = std::uint64_t;
inline constexpr RiskClientId invalid_risk_client_id = 0;

enum class RiskRejectReason : std::uint8_t {
    None = 0,
    GlobalKillSwitch,
    ClientKillSwitch,
    MaxOrderQuantity,
    MaxOrderNotional,
    MaxOpenOrders,
    MaxOpenQuantity,
    MaxPosition,
    UnknownWorkingOrder
};

[[nodiscard]] const char* to_string(
    RiskRejectReason reason
) noexcept;

struct RiskLimits {
    Quantity max_order_quantity {100'000};
    std::uint64_t max_order_notional {10'000'000'000ULL};
    std::size_t max_open_orders {10'000};
    std::uint64_t max_open_quantity {1'000'000ULL};
    std::uint64_t max_position_per_symbol {1'000'000ULL};
};

struct RiskOrderRequest {
    RiskClientId client_id {invalid_risk_client_id};
    std::string_view symbol;
    OrderId order_id {};
    Side side {Side::Buy};
    OrderType order_type {OrderType::Limit};
    TimeInForce time_in_force {TimeInForce::GoodTillCancel};
    Price price {};
    Quantity quantity {};

    /*
     * Used for market-order notional checks. For a buy this is normally
     * the current best ask; for a sell it is normally the best bid.
     */
    std::optional<Price> reference_price;
};

struct RiskDecision {
    bool accepted {true};
    RiskRejectReason reason {RiskRejectReason::None};
};

struct ClientRiskSnapshot {
    bool global_kill_switch {false};
    bool client_kill_switch {false};

    std::int64_t position {0};
    std::uint64_t open_buy_quantity {0};
    std::uint64_t open_sell_quantity {0};

    std::size_t open_orders {0};
    std::uint64_t gross_open_quantity {0};
};

/*
 * O(1)-average pre-trade risk engine.
 *
 * The ExchangeServer serializes matching and risk updates with its engine
 * mutex, so this class intentionally does not add another mutex to the hot
 * path. Callers using RiskEngine directly should provide their own external
 * synchronization when sharing one instance across threads.
 */
class RiskEngine {
public:
    explicit RiskEngine(
        RiskLimits limits = {}
    );

    [[nodiscard]] const RiskLimits& limits() const noexcept;

    [[nodiscard]] RiskDecision check_new_order(
        const RiskOrderRequest& request
    ) const;

    [[nodiscard]] RiskDecision check_replace(
        RiskClientId client_id,
        std::string_view symbol,
        OrderId order_id,
        Price new_price,
        Quantity new_quantity
    ) const;

    void on_order_resting(
        RiskClientId client_id,
        std::string_view symbol,
        OrderId order_id,
        Side side,
        Price price,
        Quantity remaining_quantity
    );

    void on_order_cancelled(
        RiskClientId client_id,
        std::string_view symbol,
        OrderId order_id
    );

    void on_trade(
        std::string_view symbol,
        RiskClientId buyer_client_id,
        RiskClientId seller_client_id,
        OrderId buy_order_id,
        OrderId sell_order_id,
        Quantity quantity
    );

    void set_global_kill_switch(bool enabled) noexcept;
    [[nodiscard]] bool global_kill_switch() const noexcept;

    void set_client_kill_switch(
        RiskClientId client_id,
        bool enabled
    );

    [[nodiscard]] ClientRiskSnapshot snapshot(
        RiskClientId client_id,
        std::string_view symbol
    ) const;

private:
    struct SymbolState {
        std::int64_t position {0};
        std::uint64_t open_buy_quantity {0};
        std::uint64_t open_sell_quantity {0};
    };

    struct ClientState {
        bool kill_switch {false};
        std::size_t open_orders {0};
        std::uint64_t gross_open_quantity {0};
        std::unordered_map<std::string, SymbolState> symbols;
    };

    struct WorkingOrderKey {
        RiskClientId client_id {invalid_risk_client_id};
        std::string symbol;
        OrderId order_id {};

        [[nodiscard]] bool operator==(
            const WorkingOrderKey& other
        ) const noexcept = default;
    };

    struct WorkingOrderKeyHash {
        [[nodiscard]] std::size_t operator()(
            const WorkingOrderKey& key
        ) const noexcept;
    };

    struct WorkingOrder {
        Side side {Side::Buy};
        Price price {};
        Quantity remaining_quantity {};
    };

    [[nodiscard]] const ClientState* find_client(
        RiskClientId client_id
    ) const noexcept;

    [[nodiscard]] ClientState& client_for(
        RiskClientId client_id
    );

    [[nodiscard]] const SymbolState* find_symbol_state(
        const ClientState& client,
        std::string_view symbol
    ) const noexcept;

    [[nodiscard]] RiskDecision check_common_order_limits(
        const ClientState* client,
        Price price,
        Quantity quantity,
        std::optional<Price> reference_price
    ) const noexcept;

    [[nodiscard]] bool position_limit_exceeded(
        const SymbolState* symbol_state,
        Side side,
        Quantity additional_quantity,
        std::uint64_t open_buy_quantity,
        std::uint64_t open_sell_quantity
    ) const noexcept;

    void erase_working_order(
        const WorkingOrderKey& key
    );

    void reduce_working_order(
        RiskClientId client_id,
        std::string_view symbol,
        OrderId order_id,
        Quantity executed_quantity
    );

    static void apply_position_delta(
        SymbolState& state,
        Side side,
        Quantity quantity
    ) noexcept;

    [[nodiscard]] static bool may_rest(
        OrderType order_type,
        TimeInForce time_in_force
    ) noexcept;

    [[nodiscard]] static std::uint64_t saturating_add(
        std::uint64_t left,
        std::uint64_t right
    ) noexcept;

    [[nodiscard]] static std::uint64_t absolute_position(
        std::int64_t value
    ) noexcept;

    [[nodiscard]] static std::uint64_t absolute_price(
        Price value
    ) noexcept;

    RiskLimits limits_;
    bool global_kill_switch_ {false};

    std::unordered_map<RiskClientId, ClientState> clients_;
    std::unordered_map<
        WorkingOrderKey,
        WorkingOrder,
        WorkingOrderKeyHash
    > working_orders_;
};

}  // namespace exchange