#include <iostream>

#include "exchange/risk_engine.hpp"

namespace {

void print_decision(
    const char* label,
    const exchange::RiskDecision& decision
) {
    std::cout
        << label
        << ": "
        << (decision.accepted ? "ACCEPT" : "REJECT");

    if (!decision.accepted) {
        std::cout
            << " ("
            << exchange::to_string(decision.reason)
            << ')';
    }

    std::cout << '\n';
}

void print_snapshot(
    const exchange::ClientRiskSnapshot& snapshot
) {
    std::cout
        << "  position=" << snapshot.position
        << " open_orders=" << snapshot.open_orders
        << " gross_open_quantity=" << snapshot.gross_open_quantity
        << " open_buy_quantity=" << snapshot.open_buy_quantity
        << " open_sell_quantity=" << snapshot.open_sell_quantity
        << '\n';
}

}  // namespace

int main() {
    exchange::RiskLimits limits;
    limits.max_order_quantity = 100;
    limits.max_order_notional = 50'000;
    limits.max_open_orders = 4;
    limits.max_open_quantity = 200;
    limits.max_position_per_symbol = 100;

    exchange::RiskEngine risk(limits);

    constexpr exchange::RiskClientId client_id = 1;

    const exchange::RiskOrderRequest first_order {
        .client_id = client_id,
        .symbol = "AAPL",
        .order_id = 1,
        .side = exchange::Side::Buy,
        .order_type = exchange::OrderType::Limit,
        .time_in_force = exchange::TimeInForce::GoodTillCancel,
        .price = 100,
        .quantity = 70,
        .reference_price = std::nullopt
    };

    print_decision(
        "BUY 70 AAPL",
        risk.check_new_order(first_order)
    );

    risk.on_order_resting(
        client_id,
        "AAPL",
        1,
        exchange::Side::Buy,
        100,
        70
    );

    std::cout << "After resting order:\n";
    print_snapshot(risk.snapshot(client_id, "AAPL"));

    exchange::RiskOrderRequest second_order = first_order;
    second_order.order_id = 2;
    second_order.price = 101;
    second_order.quantity = 40;

    print_decision(
        "BUY another 40 AAPL",
        risk.check_new_order(second_order)
    );

    risk.on_trade(
        "AAPL",
        client_id,
        exchange::invalid_risk_client_id,
        1,
        9000,
        20
    );

    std::cout << "After buying 20 shares:\n";
    print_snapshot(risk.snapshot(client_id, "AAPL"));

    risk.set_global_kill_switch(true);

    exchange::RiskOrderRequest msft_order = first_order;
    msft_order.symbol = "MSFT";
    msft_order.order_id = 3;
    msft_order.quantity = 10;

    print_decision(
        "BUY 10 MSFT with kill switch enabled",
        risk.check_new_order(msft_order)
    );

    risk.on_order_cancelled(
        client_id,
        "AAPL",
        1
    );

    std::cout << "After cancelling remaining AAPL exposure:\n";
    print_snapshot(risk.snapshot(client_id, "AAPL"));

    return 0;
}