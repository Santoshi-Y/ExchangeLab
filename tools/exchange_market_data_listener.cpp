#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "exchange/protocol.hpp"

namespace {

constexpr std::size_t maximum_datagram_size = 65'507;

void print_message(
    const exchange::protocol::MessageHeader& header,
    std::span<const std::byte> body
) {
    using exchange::protocol::MessageType;

    switch (header.type) {
        case MessageType::BookUpdate: {
            const auto update =
                exchange::protocol::decode_book_update(body);

            if (!update.has_value()) {
                std::cout << "Invalid BookUpdate\n";
                return;
            }

            std::cout
                << "SEQ " << header.sequence_number
                << " BOOK ";

            if (update->has_bid != 0) {
                std::cout
                    << "bid=" << update->best_bid
                    << " x " << update->bid_quantity
                    << ' ';
            } else {
                std::cout << "bid=none ";
            }

            if (update->has_ask != 0) {
                std::cout
                    << "ask=" << update->best_ask
                    << " x " << update->ask_quantity;
            } else {
                std::cout << "ask=none";
            }

            std::cout << '\n';
            break;
        }

        case MessageType::Level3AddOrder: {
            const auto event =
                exchange::protocol::decode_level3_add_order(body);

            if (!event.has_value()) {
                std::cout << "Invalid Level3AddOrder\n";
                return;
            }

            std::cout
                << "SEQ " << header.sequence_number
                << " L3 ADD id=" << event->order_id
                << ' '
                << (event->side == exchange::protocol::Side::Buy
                        ? "BUY "
                        : "SELL ")
                << event->price
                << " x " << event->quantity
                << '\n';
            break;
        }

        case MessageType::Level3OrderExecuted: {
            const auto event =
                exchange::protocol::decode_level3_order_executed(body);

            if (!event.has_value()) {
                std::cout << "Invalid Level3OrderExecuted\n";
                return;
            }

            std::cout
                << "SEQ " << header.sequence_number
                << " L3 EXEC buy=" << event->buy_order_id
                << " sell=" << event->sell_order_id
                << " price=" << event->price
                << " quantity=" << event->quantity
                << '\n';
            break;
        }

        case MessageType::Level3OrderDeleted: {
            const auto event =
                exchange::protocol::decode_level3_order_deleted(body);

            if (!event.has_value()) {
                std::cout << "Invalid Level3OrderDeleted\n";
                return;
            }

            std::cout
                << "SEQ " << header.sequence_number
                << " L3 DELETE id=" << event->order_id
                << '\n';
            break;
        }

        default:
            std::cout
                << "SEQ " << header.sequence_number
                << " message type="
                << static_cast<std::uint16_t>(header.type)
                << '\n';
            break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string group = "239.255.0.1";
    std::uint16_t port = 9100;

    if (argc >= 2) {
        group = argv[1];
    }

    if (argc >= 3) {
        const unsigned long parsed =
            std::stoul(argv[2]);

        if (parsed > 65'535UL) {
            std::cerr << "Invalid UDP port\n";
            return 1;
        }

        port = static_cast<std::uint16_t>(parsed);
    }

    if (argc > 3) {
        std::cerr
            << "Usage: " << argv[0]
            << " [multicast_group] [port]\n";
        return 1;
    }

    const int socket_fd =
        ::socket(AF_INET, SOCK_DGRAM, 0);

    if (socket_fd < 0) {
        std::cerr << "Could not create UDP socket\n";
        return 1;
    }

    int reuse = 1;
    if (
        ::setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        ) != 0
    ) {
        std::cerr << "Could not set SO_REUSEADDR\n";
        ::close(socket_fd);
        return 1;
    }

    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);

    if (
        ::bind(
            socket_fd,
            reinterpret_cast<sockaddr*>(&local),
            sizeof(local)
        ) != 0
    ) {
        std::cerr << "Could not bind UDP socket\n";
        ::close(socket_fd);
        return 1;
    }

    ip_mreq membership {};

    if (
        ::inet_pton(
            AF_INET,
            group.c_str(),
            &membership.imr_multiaddr
        ) != 1
    ) {
        std::cerr << "Invalid multicast group\n";
        ::close(socket_fd);
        return 1;
    }

    membership.imr_interface.s_addr =
        htonl(INADDR_ANY);

    if (
        ::setsockopt(
            socket_fd,
            IPPROTO_IP,
            IP_ADD_MEMBERSHIP,
            &membership,
            sizeof(membership)
        ) != 0
    ) {
        std::cerr << "Could not join multicast group\n";
        ::close(socket_fd);
        return 1;
    }

    std::cout
        << "Listening for ExchangeLab market data on "
        << group
        << ':'
        << port
        << '\n';

    std::vector<std::byte> buffer(
        maximum_datagram_size
    );

    while (true) {
        const auto received = ::recvfrom(
            socket_fd,
            buffer.data(),
            buffer.size(),
            0,
            nullptr,
            nullptr
        );

        if (received <= 0) {
            std::cerr << "Multicast receive failed\n";
            break;
        }

        const std::size_t size =
            static_cast<std::size_t>(received);

        if (size < exchange::protocol::header_size) {
            std::cout << "Truncated datagram\n";
            continue;
        }

        const std::span<const std::byte> datagram {
            buffer.data(),
            size
        };

        const auto header =
            exchange::protocol::decode_header(
                datagram.first(
                    exchange::protocol::header_size
                )
            );

        if (!header.has_value()) {
            std::cout << "Invalid protocol header\n";
            continue;
        }

        const std::size_t expected_size =
            exchange::protocol::header_size +
            static_cast<std::size_t>(
                header->body_size
            );

        if (size != expected_size) {
            std::cout << "Invalid datagram size\n";
            continue;
        }

        print_message(
            *header,
            datagram.subspan(
                exchange::protocol::header_size
            )
        );
    }

    ::close(socket_fd);
    return 0;
}