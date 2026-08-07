#include <array>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "exchange/fix.hpp"

namespace {

[[nodiscard]] int connect_to_gateway(std::uint16_t port) {
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
        return -1;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (
        ::inet_pton(
            AF_INET,
            "127.0.0.1",
            &address.sin_addr
        ) != 1 ||
        ::connect(
            socket,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) != 0
    ) {
        ::close(socket);
        return -1;
    }

    return socket;
}

[[nodiscard]] bool send_all(
    int socket,
    std::string_view wire
) {
    std::size_t sent_total = 0;

    while (sent_total < wire.size()) {
        const auto sent = ::send(
            socket,
            wire.data() + sent_total,
            wire.size() - sent_total,
            0
        );

        if (sent <= 0) {
            return false;
        }

        sent_total += static_cast<std::size_t>(sent);
    }

    std::cout
        << "C -> G  "
        << exchange::fix::printable(wire)
        << '\n';

    return true;
}

[[nodiscard]] std::optional<std::string> receive_one(
    int socket,
    std::string& buffer
) {
    while (true) {
        auto extracted = exchange::fix::extract_one(buffer);

        if (
            extracted.status ==
            exchange::fix::ExtractStatus::MessageReady
        ) {
            return extracted.wire_message;
        }

        if (
            extracted.status ==
            exchange::fix::ExtractStatus::InvalidData
        ) {
            return std::nullopt;
        }

        std::array<char, 4096> bytes {};
        const auto received = ::recv(
            socket,
            bytes.data(),
            bytes.size(),
            0
        );

        if (received <= 0) {
            return std::nullopt;
        }

        buffer.append(
            bytes.data(),
            static_cast<std::size_t>(received)
        );
    }
}

[[nodiscard]] exchange::fix::Message base_message(
    std::string_view type,
    std::uint64_t sequence
) {
    exchange::fix::Message message;
    message.add(35, std::string(type));
    message.add(34, std::to_string(sequence));
    message.add(49, "FIXDEMO");
    message.add(56, "EXCHANGELAB");
    message.add(52, exchange::fix::utc_timestamp());
    return message;
}

[[nodiscard]] bool send_and_receive(
    int socket,
    const exchange::fix::Message& message,
    std::string& receive_buffer
) {
    const std::string wire = exchange::fix::encode(message);
    if (!send_all(socket, wire)) {
        return false;
    }

    const auto response = receive_one(socket, receive_buffer);
    if (!response.has_value()) {
        return false;
    }

    std::cout
        << "G -> C  "
        << exchange::fix::printable(*response)
        << "\n\n";

    return true;
}

}  // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    constexpr std::uint16_t port = 9878;
    const int socket = connect_to_gateway(port);

    if (socket < 0) {
        std::cerr
            << "Could not connect to FIX gateway on 127.0.0.1:"
            << port
            << '\n';
        return 1;
    }

    std::string receive_buffer;
    std::uint64_t sequence = 1;

    auto logon = base_message("A", sequence++);
    logon.add(98, "0");
    logon.add(108, "30");

    if (!send_and_receive(socket, logon, receive_buffer)) {
        ::close(socket);
        return 1;
    }

    auto new_order = base_message("D", sequence++);
    new_order.add(11, "FIXDEMO-1");
    new_order.add(55, "AAPL");
    new_order.add(54, "1");
    new_order.add(38, "10");
    new_order.add(40, "2");
    new_order.add(44, "100");
    new_order.add(59, "1");
    new_order.add(60, exchange::fix::utc_timestamp());

    if (!send_and_receive(socket, new_order, receive_buffer)) {
        ::close(socket);
        return 1;
    }

    auto replace = base_message("G", sequence++);
    replace.add(11, "FIXDEMO-2");
    replace.add(41, "FIXDEMO-1");
    replace.add(55, "AAPL");
    replace.add(54, "1");
    replace.add(38, "12");
    replace.add(40, "2");
    replace.add(44, "101");
    replace.add(59, "1");
    replace.add(60, exchange::fix::utc_timestamp());

    if (!send_and_receive(socket, replace, receive_buffer)) {
        ::close(socket);
        return 1;
    }

    auto cancel = base_message("F", sequence++);
    cancel.add(11, "FIXDEMO-3");
    cancel.add(41, "FIXDEMO-2");
    cancel.add(55, "AAPL");
    cancel.add(54, "1");
    cancel.add(60, exchange::fix::utc_timestamp());

    if (!send_and_receive(socket, cancel, receive_buffer)) {
        ::close(socket);
        return 1;
    }

    /* The default RiskEngine max order quantity is 100,000. This proves
     * that a FIX order passes through the same pre-trade risk path. */
    auto risk_reject = base_message("D", sequence++);
    risk_reject.add(11, "FIXDEMO-RISK");
    risk_reject.add(55, "MSFT");
    risk_reject.add(54, "1");
    risk_reject.add(38, "100001");
    risk_reject.add(40, "2");
    risk_reject.add(44, "410");
    risk_reject.add(59, "1");
    risk_reject.add(60, exchange::fix::utc_timestamp());

    if (!send_and_receive(socket, risk_reject, receive_buffer)) {
        ::close(socket);
        return 1;
    }

    auto logout = base_message("5", sequence++);
    if (!send_and_receive(socket, logout, receive_buffer)) {
        ::close(socket);
        return 1;
    }

    ::shutdown(socket, SHUT_RDWR);
    ::close(socket);

    return 0;
}