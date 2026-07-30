#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "exchange/matching_engine.hpp"
#include "exchange/order_book.hpp"
#include "exchange/protocol.hpp"
#include "exchange/tcp_server.hpp"

namespace exchange {

class ExchangeServer {
public:
    explicit ExchangeServer(std::uint16_t port);

    ~ExchangeServer();

    ExchangeServer(const ExchangeServer&) = delete;
    ExchangeServer& operator=(const ExchangeServer&) = delete;

    ExchangeServer(ExchangeServer&&) = delete;
    ExchangeServer& operator=(ExchangeServer&&) = delete;

    bool start();

    void run();

    void stop();

private:
    void handle_client(int client_socket);

    void process_receive_buffer(
        int client_socket,
        std::vector<std::byte>& receive_buffer
    );

    void handle_message(
        int client_socket,
        const protocol::MessageHeader& header,
        std::span<const std::byte> body
    );

    void handle_new_order(
        int client_socket,
        const protocol::MessageHeader& header,
        std::span<const std::byte> body
    );

    void send_order_response(
        int client_socket,
        std::uint64_t order_id,
        bool success
    );

    void register_client(int client_socket);

    void unregister_client(int client_socket);

    TcpServer server_;

    OrderBook book_;
    MatchingEngine engine_;

    std::mutex engine_mutex_;

    std::mutex clients_mutex_;
    std::vector<int> client_sockets_;

    std::mutex threads_mutex_;
    std::vector<std::thread> client_threads_;

    std::atomic<bool> running_;
    std::atomic<std::uint64_t> next_sequence_number_;
};

}  // namespace exchange