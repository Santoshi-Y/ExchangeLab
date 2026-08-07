#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace exchange {

struct WebSocketGatewayConfig {
    std::uint16_t websocket_port {8080};
    std::string multicast_group {"239.255.0.1"};
    std::uint16_t multicast_port {9100};
};

class WebSocketGateway {
public:
    explicit WebSocketGateway(
        WebSocketGatewayConfig config = {}
    );

    ~WebSocketGateway();

    WebSocketGateway(const WebSocketGateway&) = delete;
    WebSocketGateway& operator=(const WebSocketGateway&) = delete;
    WebSocketGateway(WebSocketGateway&&) = delete;
    WebSocketGateway& operator=(WebSocketGateway&&) = delete;

    [[nodiscard]] bool start();
    void run();
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

private:
    [[nodiscard]] bool open_websocket_listener();
    [[nodiscard]] bool open_multicast_receiver();

    void accept_loop();
    void multicast_loop();

    [[nodiscard]] bool perform_handshake(
        int client_socket
    );

    void broadcast_text(
        const std::string& text
    );

    [[nodiscard]] std::string
    market_data_to_json(
        const std::vector<std::byte>& datagram
    ) const;

    WebSocketGatewayConfig config_;

    int listen_socket_ {-1};
    int multicast_socket_ {-1};

    std::atomic<bool> running_ {false};

    std::thread accept_thread_;
    std::thread multicast_thread_;

    std::mutex clients_mutex_;
    std::vector<int> clients_;
};

namespace websocket_detail {

/*
 * RFC 6455 helper exposed for unit testing.
 */
[[nodiscard]] std::string websocket_accept_key(
    const std::string& client_key
);

}  // namespace websocket_detail

}  // namespace exchange