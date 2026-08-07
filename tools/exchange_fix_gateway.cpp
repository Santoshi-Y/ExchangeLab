#include <csignal>
#include <cstdint>
#include <iostream>

#include "exchange/fix_gateway.hpp"

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    const exchange::FixGatewayConfig config {
        .listen_port = 9878,
        .exchange_host = "127.0.0.1",
        .exchange_port = 9000,
        .sender_comp_id = "EXCHANGELAB",
        .default_heartbeat_seconds = 30
    };

    exchange::FixGateway gateway(config);

    if (!gateway.start()) {
        std::cerr
            << "Failed to start FIX gateway on port "
            << config.listen_port
            << '\n';
        return 1;
    }

    std::cout
        << "ExchangeLab FIX 4.4 gateway\n"
        << "===========================\n"
        << "FIX order entry: 127.0.0.1:"
        << config.listen_port
        << '\n'
        << "Core exchange: "
        << config.exchange_host
        << ':'
        << config.exchange_port
        << '\n'
        << "SenderCompID: "
        << config.sender_comp_id
        << '\n'
        << "Supported: Logon/Heartbeat/TestRequest/Logout, "
        << "NewOrderSingle, Cancel, CancelReplace\n";

    gateway.run();
    return 0;
}