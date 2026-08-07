#include "exchange/websocket_gateway.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#error "Windows support not implemented yet."
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "exchange/protocol.hpp"

namespace exchange {

namespace {

constexpr std::string_view websocket_guid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr std::size_t receive_buffer_size = 2048;

std::uint32_t rotate_left(
    std::uint32_t value,
    unsigned int amount
) {
    return
        (value << amount) |
        (value >> (32U - amount));
}

std::array<std::byte, 20> sha1(
    std::span<const std::byte> input
) {
    std::vector<std::byte> message(
        input.begin(),
        input.end()
    );

    const std::uint64_t bit_length =
        static_cast<std::uint64_t>(
            message.size()
        ) * 8U;

    message.push_back(
        static_cast<std::byte>(0x80U)
    );

    while ((message.size() % 64U) != 56U) {
        message.push_back(std::byte {0});
    }

    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(
            static_cast<std::byte>(
                (bit_length >>
                    static_cast<unsigned int>(shift)) &
                0xFFU
            )
        );
    }

    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;

    for (
        std::size_t chunk = 0;
        chunk < message.size();
        chunk += 64U
    ) {
        std::array<std::uint32_t, 80> words {};

        for (std::size_t index = 0; index < 16U; ++index) {
            const std::size_t offset =
                chunk + (index * 4U);

            words[index] =
                (static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(
                        message[offset]
                    )
                ) << 24U) |
                (static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(
                        message[offset + 1U]
                    )
                ) << 16U) |
                (static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(
                        message[offset + 2U]
                    )
                ) << 8U) |
                static_cast<std::uint32_t>(
                    std::to_integer<std::uint8_t>(
                        message[offset + 3U]
                    )
                );
        }

        for (std::size_t index = 16U; index < 80U; ++index) {
            words[index] = rotate_left(
                words[index - 3U] ^
                words[index - 8U] ^
                words[index - 14U] ^
                words[index - 16U],
                1U
            );
        }

        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;

        for (std::size_t index = 0; index < 80U; ++index) {
            std::uint32_t function = 0;
            std::uint32_t constant = 0;

            if (index < 20U) {
                function =
                    (b & c) |
                    ((~b) & d);
                constant = 0x5A827999U;
            } else if (index < 40U) {
                function = b ^ c ^ d;
                constant = 0x6ED9EBA1U;
            } else if (index < 60U) {
                function =
                    (b & c) |
                    (b & d) |
                    (c & d);
                constant = 0x8F1BBCDCU;
            } else {
                function = b ^ c ^ d;
                constant = 0xCA62C1D6U;
            }

            const std::uint32_t temporary =
                rotate_left(a, 5U) +
                function +
                e +
                constant +
                words[index];

            e = d;
            d = c;
            c = rotate_left(b, 30U);
            b = a;
            a = temporary;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::byte, 20> digest {};
    const std::array<std::uint32_t, 5> hashes {
        h0, h1, h2, h3, h4
    };

    for (std::size_t index = 0; index < hashes.size(); ++index) {
        digest[(index * 4U)] =
            static_cast<std::byte>(
                (hashes[index] >> 24U) & 0xFFU
            );
        digest[(index * 4U) + 1U] =
            static_cast<std::byte>(
                (hashes[index] >> 16U) & 0xFFU
            );
        digest[(index * 4U) + 2U] =
            static_cast<std::byte>(
                (hashes[index] >> 8U) & 0xFFU
            );
        digest[(index * 4U) + 3U] =
            static_cast<std::byte>(
                hashes[index] & 0xFFU
            );
    }

    return digest;
}

std::string base64_encode(
    std::span<const std::byte> input
) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string output;
    output.reserve(
        ((input.size() + 2U) / 3U) * 4U
    );

    for (
        std::size_t offset = 0;
        offset < input.size();
        offset += 3U
    ) {
        const std::uint32_t first =
            std::to_integer<std::uint8_t>(
                input[offset]
            );

        const bool has_second =
            offset + 1U < input.size();

        const bool has_third =
            offset + 2U < input.size();

        const std::uint32_t second =
            has_second
                ? std::to_integer<std::uint8_t>(
                    input[offset + 1U]
                )
                : 0U;

        const std::uint32_t third =
            has_third
                ? std::to_integer<std::uint8_t>(
                    input[offset + 2U]
                )
                : 0U;

        const std::uint32_t combined =
            (first << 16U) |
            (second << 8U) |
            third;

        output.push_back(
            alphabet[(combined >> 18U) & 0x3FU]
        );
        output.push_back(
            alphabet[(combined >> 12U) & 0x3FU]
        );

        output.push_back(
            has_second
                ? alphabet[(combined >> 6U) & 0x3FU]
                : '='
        );

        output.push_back(
            has_third
                ? alphabet[combined & 0x3FU]
                : '='
        );
    }

    return output;
}

std::optional<std::string> header_value(
    const std::string& request,
    std::string_view header_name
) {
    std::istringstream stream(request);
    std::string line;

    while (std::getline(stream, line)) {
        if (
            !line.empty() &&
            line.back() == '\r'
        ) {
            line.pop_back();
        }

        const std::size_t colon =
            line.find(':');

        if (colon == std::string::npos) {
            continue;
        }

        const std::string_view name(
            line.data(),
            colon
        );

        if (name != header_name) {
            continue;
        }

        std::size_t start = colon + 1U;

        while (
            start < line.size() &&
            (line[start] == ' ' ||
             line[start] == '\t')
        ) {
            ++start;
        }

        return line.substr(start);
    }

    return std::nullopt;
}

bool send_all(
    int socket,
    std::span<const std::byte> bytes
) {
    std::size_t sent_total = 0;

    while (sent_total < bytes.size()) {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif

        const auto sent = ::send(
            socket,
            bytes.data() + sent_total,
            bytes.size() - sent_total,
            flags
        );

        if (sent <= 0) {
            return false;
        }

        sent_total +=
            static_cast<std::size_t>(sent);
    }

    return true;
}

std::vector<std::byte> websocket_text_frame(
    const std::string& text
) {
    std::vector<std::byte> frame;

    /*
     * FIN=1, opcode=1 (text).
     * Server-to-client frames are never masked.
     */
    frame.push_back(
        static_cast<std::byte>(0x81U)
    );

    const std::size_t length = text.size();

    if (length <= 125U) {
        frame.push_back(
            static_cast<std::byte>(length)
        );
    } else if (length <= 65535U) {
        frame.push_back(
            static_cast<std::byte>(126U)
        );

        frame.push_back(
            static_cast<std::byte>(
                (length >> 8U) & 0xFFU
            )
        );

        frame.push_back(
            static_cast<std::byte>(
                length & 0xFFU
            )
        );
    } else {
        frame.push_back(
            static_cast<std::byte>(127U)
        );

        const std::uint64_t length64 =
            static_cast<std::uint64_t>(length);

        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(
                static_cast<std::byte>(
                    (length64 >>
                        static_cast<unsigned int>(shift)) &
                    0xFFU
                )
            );
        }
    }

    frame.insert(
        frame.end(),
        reinterpret_cast<const std::byte*>(
            text.data()
        ),
        reinterpret_cast<const std::byte*>(
            text.data() + text.size()
        )
    );

    return frame;
}

std::string json_escape(
    std::string_view value
) {
    std::string output;

    for (const char character : value) {
        switch (character) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                output.push_back(character);
                break;
        }
    }

    return output;
}

}  // namespace

namespace websocket_detail {

std::string websocket_accept_key(
    const std::string& client_key
) {
    const std::string source =
        client_key +
        std::string(websocket_guid);

    const auto digest = sha1(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(
                source.data()
            ),
            source.size()
        )
    );

    return base64_encode(digest);
}

}  // namespace websocket_detail

WebSocketGateway::WebSocketGateway(
    WebSocketGatewayConfig config
)
    : config_(std::move(config)) {}

WebSocketGateway::~WebSocketGateway() {
    stop();
}

bool WebSocketGateway::start() {
    if (running_.load()) {
        return true;
    }

    if (!open_websocket_listener()) {
        return false;
    }

    if (!open_multicast_receiver()) {
        ::close(listen_socket_);
        listen_socket_ = -1;
        return false;
    }

    running_.store(true);

    try {
        accept_thread_ = std::thread(
            &WebSocketGateway::accept_loop,
            this
        );

        multicast_thread_ = std::thread(
            &WebSocketGateway::multicast_loop,
            this
        );
    } catch (...) {
        stop();
        return false;
    }

    return true;
}

void WebSocketGateway::run() {
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    if (multicast_thread_.joinable()) {
        multicast_thread_.join();
    }
}

void WebSocketGateway::stop() noexcept {
    running_.store(false);

    if (listen_socket_ >= 0) {
        ::shutdown(listen_socket_, SHUT_RDWR);
        ::close(listen_socket_);
        listen_socket_ = -1;
    }

    if (multicast_socket_ >= 0) {
        ::shutdown(multicast_socket_, SHUT_RDWR);
        ::close(multicast_socket_);
        multicast_socket_ = -1;
    }

    {
        std::lock_guard<std::mutex> lock(
            clients_mutex_
        );

        for (const int socket : clients_) {
            ::shutdown(socket, SHUT_RDWR);
            ::close(socket);
        }

        clients_.clear();
    }

    const std::thread::id current =
        std::this_thread::get_id();

    if (
        accept_thread_.joinable() &&
        accept_thread_.get_id() != current
    ) {
        accept_thread_.join();
    }

    if (
        multicast_thread_.joinable() &&
        multicast_thread_.get_id() != current
    ) {
        multicast_thread_.join();
    }
}

bool WebSocketGateway::is_running() const noexcept {
    return running_.load();
}

bool WebSocketGateway::open_websocket_listener() {
    listen_socket_ = ::socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (listen_socket_ < 0) {
        return false;
    }

    int enable = 1;

    static_cast<void>(
        ::setsockopt(
            listen_socket_,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enable,
            sizeof(enable)
        )
    );

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr =
        htonl(INADDR_ANY);
    address.sin_port =
        htons(config_.websocket_port);

    if (
        ::bind(
            listen_socket_,
            reinterpret_cast<sockaddr*>(
                &address
            ),
            sizeof(address)
        ) != 0
    ) {
        ::close(listen_socket_);
        listen_socket_ = -1;
        return false;
    }

    if (::listen(listen_socket_, 16) != 0) {
        ::close(listen_socket_);
        listen_socket_ = -1;
        return false;
    }

    return true;
}

bool WebSocketGateway::open_multicast_receiver() {
    multicast_socket_ = ::socket(
        AF_INET,
        SOCK_DGRAM,
        0
    );

    if (multicast_socket_ < 0) {
        return false;
    }

    int enable = 1;

    if (
        ::setsockopt(
            multicast_socket_,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enable,
            sizeof(enable)
        ) != 0
    ) {
        ::close(multicast_socket_);
        multicast_socket_ = -1;
        return false;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr =
        htonl(INADDR_ANY);
    address.sin_port =
        htons(config_.multicast_port);

    if (
        ::bind(
            multicast_socket_,
            reinterpret_cast<sockaddr*>(
                &address
            ),
            sizeof(address)
        ) != 0
    ) {
        ::close(multicast_socket_);
        multicast_socket_ = -1;
        return false;
    }

    ip_mreq membership {};
    membership.imr_interface.s_addr =
        htonl(INADDR_ANY);

    if (
        ::inet_pton(
            AF_INET,
            config_.multicast_group.c_str(),
            &membership.imr_multiaddr
        ) != 1
    ) {
        ::close(multicast_socket_);
        multicast_socket_ = -1;
        return false;
    }

    if (
        ::setsockopt(
            multicast_socket_,
            IPPROTO_IP,
            IP_ADD_MEMBERSHIP,
            &membership,
            sizeof(membership)
        ) != 0
    ) {
        ::close(multicast_socket_);
        multicast_socket_ = -1;
        return false;
    }

    return true;
}

void WebSocketGateway::accept_loop() {
    while (running_.load()) {
        sockaddr_in client_address {};
        socklen_t client_length =
            sizeof(client_address);

        const int client_socket =
            ::accept(
                listen_socket_,
                reinterpret_cast<sockaddr*>(
                    &client_address
                ),
                &client_length
            );

        if (client_socket < 0) {
            if (running_.load()) {
                std::cerr
                    << "WebSocket accept failed\n";
            }

            break;
        }

        if (!perform_handshake(client_socket)) {
            ::close(client_socket);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(
                clients_mutex_
            );

            clients_.push_back(client_socket);
        }

        std::cout
            << "WebSocket client connected\n";
    }
}

void WebSocketGateway::multicast_loop() {
    std::array<std::byte, receive_buffer_size>
        buffer {};

    while (running_.load()) {
        const auto received = ::recv(
            multicast_socket_,
            buffer.data(),
            buffer.size(),
            0
        );

        if (received <= 0) {
            if (running_.load()) {
                std::cerr
                    << "Multicast receive failed\n";
            }

            break;
        }

        const std::vector<std::byte> datagram(
            buffer.begin(),
            buffer.begin() +
                static_cast<std::ptrdiff_t>(
                    received
                )
        );

        const std::string json =
            market_data_to_json(datagram);

        if (!json.empty()) {
            broadcast_text(json);
        }
    }
}

bool WebSocketGateway::perform_handshake(
    int client_socket
) {
    std::string request;
    std::array<char, 1024> buffer {};

    while (
        request.find("\r\n\r\n") ==
        std::string::npos
    ) {
        const auto received = ::recv(
            client_socket,
            buffer.data(),
            buffer.size(),
            0
        );

        if (received <= 0) {
            return false;
        }

        request.append(
            buffer.data(),
            static_cast<std::size_t>(
                received
            )
        );

        if (request.size() > 16U * 1024U) {
            return false;
        }
    }

    const auto key =
        header_value(
            request,
            "Sec-WebSocket-Key"
        );

    if (!key.has_value()) {
        return false;
    }

    const std::string accept_key =
        websocket_detail::
            websocket_accept_key(*key);

    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        accept_key +
        "\r\n\r\n";

    return send_all(
        client_socket,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(
                response.data()
            ),
            response.size()
        )
    );
}

void WebSocketGateway::broadcast_text(
    const std::string& text
) {
    const std::vector<std::byte> frame =
        websocket_text_frame(text);

    std::lock_guard<std::mutex> lock(
        clients_mutex_
    );

    auto iterator = clients_.begin();

    while (iterator != clients_.end()) {
        if (send_all(*iterator, frame)) {
            ++iterator;
            continue;
        }

        ::shutdown(*iterator, SHUT_RDWR);
        ::close(*iterator);

        iterator = clients_.erase(iterator);

        std::cout
            << "WebSocket client disconnected\n";
    }
}

std::string WebSocketGateway::market_data_to_json(
    const std::vector<std::byte>& datagram
) const {
    if (
        datagram.size() <
        protocol::header_size
    ) {
        return {};
    }

    const std::span<const std::byte>
        header_bytes {
            datagram.data(),
            protocol::header_size
        };

    const auto header =
        protocol::decode_header(header_bytes);

    if (!header.has_value()) {
        return {};
    }

    const std::size_t expected_size =
        protocol::header_size +
        static_cast<std::size_t>(
            header->body_size
        );

    if (datagram.size() != expected_size) {
        return {};
    }

    const std::span<const std::byte> body {
        datagram.data() +
            protocol::header_size,
        static_cast<std::size_t>(
            header->body_size
        )
    };

    std::ostringstream json;

    switch (header->type) {
        case protocol::MessageType::BookUpdate: {
            const auto update =
                protocol::decode_book_update(body);

            if (!update.has_value()) {
                return {};
            }

            json
                << "{\"type\":\"book\","
                << "\"symbol\":\""
                << protocol::symbol_to_string(update->symbol)
                << "\","
                << "\"sequence\":"
                << update->sequence_number
                << ",\"hasBid\":"
                << (update->has_bid != 0 ? "true" : "false")
                << ",\"bestBid\":"
                << update->best_bid
                << ",\"bidQuantity\":"
                << update->bid_quantity
                << ",\"hasAsk\":"
                << (update->has_ask != 0 ? "true" : "false")
                << ",\"bestAsk\":"
                << update->best_ask
                << ",\"askQuantity\":"
                << update->ask_quantity
                << '}';

            return json.str();
        }

        case protocol::MessageType::Level3AddOrder: {
            const auto event =
                protocol::decode_level3_add_order(body);

            if (!event.has_value()) {
                return {};
            }

            json
                << "{\"type\":\"add\","
                << "\"symbol\":\""
                << protocol::symbol_to_string(event->symbol)
                << "\","
                << "\"sequence\":"
                << event->sequence_number
                << ",\"orderId\":"
                << event->order_id
                << ",\"timestamp\":"
                << event->timestamp
                << ",\"price\":"
                << event->price
                << ",\"quantity\":"
                << event->quantity
                << ",\"side\":\""
                << (
                    event->side ==
                    protocol::Side::Buy
                        ? "BUY"
                        : "SELL"
                )
                << "\"}";

            return json.str();
        }

        case protocol::MessageType::Level3OrderExecuted: {
            const auto event =
                protocol::
                    decode_level3_order_executed(
                        body
                    );

            if (!event.has_value()) {
                return {};
            }

            json
                << "{\"type\":\"execute\","
                << "\"symbol\":\""
                << protocol::symbol_to_string(event->symbol)
                << "\","
                << "\"sequence\":"
                << event->sequence_number
                << ",\"buyOrderId\":"
                << event->buy_order_id
                << ",\"sellOrderId\":"
                << event->sell_order_id
                << ",\"price\":"
                << event->price
                << ",\"quantity\":"
                << event->quantity
                << '}';

            return json.str();
        }

        case protocol::MessageType::Level3OrderDeleted: {
            const auto event =
                protocol::
                    decode_level3_order_deleted(
                        body
                    );

            if (!event.has_value()) {
                return {};
            }

            json
                << "{\"type\":\"delete\","
                << "\"symbol\":\""
                << protocol::symbol_to_string(event->symbol)
                << "\","
                << "\"sequence\":"
                << event->sequence_number
                << ",\"orderId\":"
                << event->order_id
                << '}';

            return json.str();
        }

        default:
            return {};
    }
}

}  // namespace exchange