#include <cstdint>
#include <iostream>

#include "exchange/websocket_gateway.hpp"

int main() {
    const exchange::WebSocketGatewayConfig config {
        .websocket_port = 8080,
        .multicast_group = "239.255.0.1",
        .multicast_port = 9100,
        .performance_port = 9200
    };

    exchange::WebSocketGateway gateway(config);

    if (!gateway.start()) {
        std::cerr
            << "Failed to start WebSocket gateway\n";

        return 1;
    }

    std::cout
        << "ExchangeLab WebSocket gateway\n"
        << "==============================\n"
        << "WebSocket: ws://127.0.0.1:"
        << config.websocket_port
        << '\n'
        << "Market data source: "
        << config.multicast_group
        << ':'
        << config.multicast_port
        << '\n'
        << "Performance source: 127.0.0.1:"
        << config.performance_port
        << '\n';

    gateway.run();

    return 0;
}