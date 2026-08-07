#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "exchange/fix.hpp"
#include "exchange/protocol.hpp"

namespace exchange {

struct FixGatewayConfig {
    std::uint16_t listen_port {9878};
    std::string exchange_host {"127.0.0.1"};
    std::uint16_t exchange_port {9000};
    std::string sender_comp_id {"EXCHANGELAB"};
    std::uint32_t default_heartbeat_seconds {30};
};

namespace fixbridge {

struct NewOrderTranslation {
    protocol::NewOrderRequest request;
    std::string cl_ord_id;
};

[[nodiscard]] std::optional<NewOrderTranslation>
translate_new_order_single(
    const fix::Message& message,
    std::uint64_t numeric_order_id,
    std::uint64_t timestamp,
    std::string& error
);

[[nodiscard]] std::optional<protocol::TimeInForce>
translate_time_in_force(std::string_view fix_value) noexcept;

[[nodiscard]] std::optional<protocol::Side>
translate_side(std::string_view fix_value) noexcept;

[[nodiscard]] std::optional<protocol::OrderType>
translate_order_type(std::string_view fix_value) noexcept;

}  // namespace fixbridge

/*
 * A deliberately small FIX 4.4 order-entry gateway.
 *
 * Supported session messages:
 *   A Logon
 *   0 Heartbeat
 *   1 TestRequest
 *   5 Logout
 *
 * Supported application messages:
 *   D NewOrderSingle
 *   F OrderCancelRequest
 *   G OrderCancelReplaceRequest
 *
 * Responses:
 *   8 ExecutionReport
 *   9 OrderCancelReject
 *   3 Reject
 *
 * Every logged-on FIX TCP session gets its own binary connection to the core
 * exchange. That preserves the core RiskEngine's connection/session identity.
 */
class FixGateway {
public:
    explicit FixGateway(FixGatewayConfig config = {});
    ~FixGateway();

    FixGateway(const FixGateway&) = delete;
    FixGateway& operator=(const FixGateway&) = delete;
    FixGateway(FixGateway&&) = delete;
    FixGateway& operator=(FixGateway&&) = delete;

    bool start();
    void run();
    void stop();

    [[nodiscard]] const FixGatewayConfig&
    config() const noexcept;

private:
    void handle_client(int client_socket);

    FixGatewayConfig config_;
    std::atomic<bool> running_ {false};
    int listen_socket_ {-1};

    std::atomic<std::uint64_t> next_numeric_order_id_ {
        (std::uint64_t {1} << 62U)
    };

    std::atomic<std::uint64_t> next_exec_id_ {1};
    std::mutex threads_mutex_;
    std::vector<std::thread> client_threads_;
};

}  // namespace exchange