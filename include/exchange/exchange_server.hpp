#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "exchange/matching_engine.hpp"
#include "exchange/order_book.hpp"
#include "exchange/protocol.hpp"
#include "exchange/tcp_server.hpp"

namespace exchange {

class ExchangeServer {
public:
    explicit ExchangeServer(std::uint16_t port);

    ~ExchangeServer() = default;

    ExchangeServer(const ExchangeServer&) = delete;
    ExchangeServer& operator=(const ExchangeServer&) = delete;

    ExchangeServer(ExchangeServer&&) = delete;
    ExchangeServer& operator=(ExchangeServer&&) = delete;

    bool start();

    void run();

    void stop();

private:
    void process_receive_buffer();

    void handle_message(
        const protocol::MessageHeader& header,
        std::span<const std::byte> body
    );

    void handle_new_order(
        const protocol::MessageHeader& header,
        std::span<const std::byte> body
    );

    void send_order_response(
        std::uint64_t order_id,
        bool success
    );

    TcpServer server_;
    OrderBook book_;
    MatchingEngine engine_;

    std::vector<std::byte> receive_buffer_;

    std::uint64_t next_sequence_number_;
};

}  // namespace exchange