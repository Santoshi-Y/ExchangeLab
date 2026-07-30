#include <cstdint>
#include <iostream>

#include "exchange/exchange_server.hpp"

int main() {
    constexpr std::uint16_t port = 9000;

    exchange::ExchangeServer server(port);

    if (!server.start()) {
        std::cerr
            << "Failed to start exchange server on port "
            << port
            << '\n';

        return 1;
    }

    std::cout
        << "ExchangeLab listening on port "
        << port
        << "...\n";

    server.run();

    std::cout << "Client disconnected.\n";

    return 0;
}