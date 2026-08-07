#include <cstdint>
#include <filesystem>
#include <iostream>

#include "exchange/exchange_server.hpp"
#include "exchange/protocol.hpp"
#include "exchange/risk_engine.hpp"

int main() {
    constexpr std::uint16_t port = 9000;

    const std::filesystem::path journal_path =
        "exchange.log";

    const exchange::MulticastConfig multicast {
        .group = "239.255.0.1",
        .port = 9100,
        .ttl = 1
    };

    const exchange::RiskLimits risk_limits {};

    exchange::ExchangeServer server(
        port,
        journal_path,
        multicast,
        risk_limits
    );

    if (!server.start()) {
        std::cerr
            << "Failed to start ExchangeLab on port "
            << port
            << '\n';

        return 1;
    }

    const exchange::RecoveryState& recovery =
        server.recovery_state();

    if (recovery.attempted) {
        std::cout
            << "Journal recovery complete\n"
            << "  Records: "
            << recovery.journal_records
            << '\n'
            << "  New orders: "
            << recovery.new_orders
            << '\n'
            << "  Trades: "
            << recovery.trades
            << '\n'
            << "  Rejected records: "
            << recovery.rejected_messages
            << '\n'
            << "  Unsupported records: "
            << recovery.unsupported_messages
            << '\n'
            << "  Instruments: "
            << recovery.instruments
            << '\n'
            << "  Remaining orders: "
            << recovery.remaining_orders
            << '\n';
    }

    const exchange::RiskLimits& active_risk =
        server.risk_limits();

    std::cout
        << "ExchangeLab listening on port "
        << port
        << '\n'
        << "Protocol version: "
        << exchange::protocol::protocol_version
        << '\n'
        << "Journal: "
        << journal_path
        << '\n'
        << "Market data multicast: "
        << multicast.group
        << ':'
        << multicast.port
        << '\n'
        << "Risk limits:\n"
        << "  Max order quantity: "
        << active_risk.max_order_quantity
        << '\n'
        << "  Max order notional: "
        << active_risk.max_order_notional
        << '\n'
        << "  Max open orders/session: "
        << active_risk.max_open_orders
        << '\n'
        << "  Max open quantity/session: "
        << active_risk.max_open_quantity
        << '\n'
        << "  Max position/symbol: +/-"
        << active_risk.max_position_per_symbol
        << '\n';

    server.run();

    return 0;
}