#include <cstdint>
#include <filesystem>
#include <iostream>

#include "exchange/exchange_server.hpp"

int main() {
    constexpr std::uint16_t port = 9000;

    const std::filesystem::path journal_path =
        "exchange.log";

    exchange::ExchangeServer server(
        port,
        journal_path
    );

    if (!server.start()) {
        std::cerr
            << "Failed to start ExchangeLab on port "
            << port
            << '\n';

        return 1;
    }

    std::cout
        << "ExchangeLab listening on port "
        << port
        << '\n'
        << "Journal: "
        << journal_path
        << '\n';

    server.run();

    return 0;
}