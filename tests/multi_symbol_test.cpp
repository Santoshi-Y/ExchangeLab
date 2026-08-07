#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "exchange/exchange_server.hpp"
#include "exchange/journal.hpp"
#include "exchange/protocol.hpp"
#include "exchange/replay.hpp"

namespace {

using namespace std::chrono_literals;

struct ReceivedMessage {
    exchange::protocol::MessageHeader header;
    std::vector<std::byte> body;
};

template <std::size_t HeaderSize, std::size_t BodySize>
std::vector<std::byte> combine_message(
    const std::array<std::byte, HeaderSize>& header,
    const std::array<std::byte, BodySize>& body
) {
    std::vector<std::byte> message;
    message.reserve(HeaderSize + BodySize);
    message.insert(message.end(), header.begin(), header.end());
    message.insert(message.end(), body.begin(), body.end());
    return message;
}

template <std::size_t BodySize>
std::vector<std::byte> make_message(
    exchange::protocol::MessageType type,
    const std::array<std::byte, BodySize>& body,
    std::uint64_t sequence_number
) {
    const exchange::protocol::MessageHeader header {
        .magic = exchange::protocol::protocol_magic,
        .version = exchange::protocol::protocol_version,
        .type = type,
        .body_size = static_cast<std::uint32_t>(body.size()),
        .sequence_number = sequence_number
    };

    return combine_message(
        exchange::protocol::encode_header(header),
        body
    );
}

std::vector<std::byte> make_new_order_message(
    std::uint64_t order_id,
    std::uint64_t timestamp,
    std::int64_t price,
    std::uint64_t quantity,
    exchange::protocol::Side side,
    const exchange::protocol::Symbol& symbol
) {
    const exchange::protocol::NewOrderRequest request {
        .order_id = order_id,
        .timestamp = timestamp,
        .price = price,
        .quantity = quantity,
        .side = side,
        .order_type = exchange::protocol::OrderType::Limit,
        .time_in_force =
            exchange::protocol::TimeInForce::GoodTillCancel,
        .symbol = symbol
    };

    return make_message(
        exchange::protocol::MessageType::NewOrder,
        exchange::protocol::encode_new_order(request),
        timestamp
    );
}

bool send_all(
    int socket,
    std::span<const std::byte> bytes
) {
    std::size_t total_sent = 0;

    while (total_sent < bytes.size()) {
        const auto sent = ::send(
            socket,
            bytes.data() + total_sent,
            bytes.size() - total_sent,
            0
        );

        if (sent <= 0) {
            return false;
        }

        total_sent += static_cast<std::size_t>(sent);
    }

    return true;
}

bool receive_exact(
    int socket,
    std::span<std::byte> output
) {
    std::size_t total_received = 0;

    while (total_received < output.size()) {
        const auto received = ::recv(
            socket,
            output.data() + total_received,
            output.size() - total_received,
            0
        );

        if (received <= 0) {
            return false;
        }

        total_received +=
            static_cast<std::size_t>(received);
    }

    return true;
}

int connect_to_server(std::uint16_t port) {
    for (int attempt = 0; attempt < 50; ++attempt) {
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
            ) != 1
        ) {
            ::close(socket);
            return -1;
        }

        if (
            ::connect(
                socket,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)
            ) == 0
        ) {
            return socket;
        }

        ::close(socket);
        std::this_thread::sleep_for(10ms);
    }

    return -1;
}

bool send_order(
    int socket,
    const exchange::protocol::NewOrderRequest& request,
    std::uint64_t sequence_number
) {
    const auto body =
        exchange::protocol::encode_new_order(request);

    return send_all(
        socket,
        make_message(
            exchange::protocol::MessageType::NewOrder,
            body,
            sequence_number
        )
    );
}

std::optional<ReceivedMessage>
receive_message(int socket) {
    std::array<
        std::byte,
        exchange::protocol::header_size
    > header_bytes {};

    if (!receive_exact(socket, header_bytes)) {
        return std::nullopt;
    }

    const auto header =
        exchange::protocol::decode_header(header_bytes);

    if (!header.has_value()) {
        return std::nullopt;
    }

    std::vector<std::byte> body(
        static_cast<std::size_t>(header->body_size)
    );

    if (!body.empty() && !receive_exact(socket, body)) {
        return std::nullopt;
    }

    return ReceivedMessage {
        .header = *header,
        .body = std::move(body)
    };
}

std::optional<ReceivedMessage> receive_until_type(
    int socket,
    exchange::protocol::MessageType expected,
    std::size_t maximum_messages = 16
) {
    for (std::size_t index = 0; index < maximum_messages; ++index) {
        auto message = receive_message(socket);

        if (!message.has_value()) {
            return std::nullopt;
        }

        if (message->header.type == expected) {
            return message;
        }
    }

    return std::nullopt;
}

void close_client(int socket) {
    if (socket >= 0) {
        ::shutdown(socket, SHUT_RDWR);
        ::close(socket);
    }
}

}  // namespace

TEST(
    MultiSymbolProtocolTest,
    RoundTripsSymbolOnOrderAndMarketData
) {
    const auto aapl =
        exchange::protocol::make_symbol("AAPL");

    const exchange::protocol::NewOrderRequest request {
        .order_id = 99,
        .timestamp = 7,
        .price = 205,
        .quantity = 15,
        .side = exchange::protocol::Side::Buy,
        .order_type = exchange::protocol::OrderType::Limit,
        .time_in_force =
            exchange::protocol::TimeInForce::GoodTillCancel,
        .symbol = aapl
    };

    const auto decoded_order =
        exchange::protocol::decode_new_order(
            exchange::protocol::encode_new_order(request)
        );

    ASSERT_TRUE(decoded_order.has_value());
    EXPECT_EQ(
        exchange::protocol::symbol_to_string(
            decoded_order->symbol
        ),
        "AAPL"
    );

    const exchange::protocol::BookUpdate update {
        .has_bid = 1,
        .best_bid = 204,
        .bid_quantity = 10,
        .has_ask = 1,
        .best_ask = 205,
        .ask_quantity = 12,
        .sequence_number = 88,
        .symbol = aapl
    };

    const auto decoded_update =
        exchange::protocol::decode_book_update(
            exchange::protocol::encode_book_update(update)
        );

    ASSERT_TRUE(decoded_update.has_value());
    EXPECT_EQ(
        exchange::protocol::symbol_to_string(
            decoded_update->symbol
        ),
        "AAPL"
    );
}

TEST(
    MultiSymbolReplayTest,
    ReconstructsIndependentBooks
) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "exchange_lab_multi_symbol_replay.bin";

    std::filesystem::remove(path);

    const auto aapl =
        exchange::protocol::make_symbol("AAPL");

    const auto msft =
        exchange::protocol::make_symbol("MSFT");

    {
        exchange::ExchangeJournal journal(path, true);

        journal.append(
            make_new_order_message(
                1,
                1,
                105,
                10,
                exchange::protocol::Side::Sell,
                aapl
            )
        );

        /* Same order ID is valid on a different symbol. */
        journal.append(
            make_new_order_message(
                1,
                2,
                410,
                25,
                exchange::protocol::Side::Buy,
                msft
            )
        );

        journal.flush();
    }

    exchange::ExchangeReplayer replayer;
    ASSERT_TRUE(replayer.replay(path));

    EXPECT_EQ(replayer.summary().symbols, 2U);
    EXPECT_TRUE(replayer.has_symbol("AAPL"));
    EXPECT_TRUE(replayer.has_symbol("MSFT"));

    const exchange::OrderBook& aapl_book =
        replayer.order_book("AAPL");

    const exchange::OrderBook& msft_book =
        replayer.order_book("MSFT");

    ASSERT_TRUE(aapl_book.has_asks());
    EXPECT_FALSE(aapl_book.has_bids());
    EXPECT_EQ(aapl_book.best_ask(), 105);
    EXPECT_EQ(aapl_book.order_count(), 1U);

    ASSERT_TRUE(msft_book.has_bids());
    EXPECT_FALSE(msft_book.has_asks());
    EXPECT_EQ(msft_book.best_bid(), 410);
    EXPECT_EQ(msft_book.order_count(), 1U);

    std::filesystem::remove(path);
}

TEST(
    MultiSymbolServerTest,
    SameOrderIdDoesNotCrossAcrossSymbols
) {
    constexpr std::uint16_t port = 19030;

    exchange::ExchangeServer server(port);
    ASSERT_TRUE(server.start());

    std::thread server_thread([&server]() {
        server.run();
    });

    const int client_socket = connect_to_server(port);
    ASSERT_GE(client_socket, 0);

    const auto aapl =
        exchange::protocol::make_symbol("AAPL");

    const auto msft =
        exchange::protocol::make_symbol("MSFT");

    const exchange::protocol::NewOrderRequest aapl_sell {
        .order_id = 42,
        .timestamp = 1,
        .price = 100,
        .quantity = 10,
        .side = exchange::protocol::Side::Sell,
        .order_type = exchange::protocol::OrderType::Limit,
        .time_in_force =
            exchange::protocol::TimeInForce::GoodTillCancel,
        .symbol = aapl
    };

    ASSERT_TRUE(send_order(client_socket, aapl_sell, 1));

    const auto first_response = receive_until_type(
        client_socket,
        exchange::protocol::MessageType::OrderAccepted
    );

    ASSERT_TRUE(first_response.has_value());

    const auto first_book_message = receive_until_type(
        client_socket,
        exchange::protocol::MessageType::BookUpdate
    );

    ASSERT_TRUE(first_book_message.has_value());

    const auto first_book =
        exchange::protocol::decode_book_update(
            first_book_message->body
        );

    ASSERT_TRUE(first_book.has_value());
    EXPECT_EQ(
        exchange::protocol::symbol_to_string(
            first_book->symbol
        ),
        "AAPL"
    );
    EXPECT_EQ(first_book->has_bid, 0);
    EXPECT_EQ(first_book->has_ask, 1);
    EXPECT_EQ(first_book->best_ask, 100);

    const exchange::protocol::NewOrderRequest msft_buy {
        .order_id = 42,
        .timestamp = 2,
        .price = 100,
        .quantity = 10,
        .side = exchange::protocol::Side::Buy,
        .order_type = exchange::protocol::OrderType::Limit,
        .time_in_force =
            exchange::protocol::TimeInForce::GoodTillCancel,
        .symbol = msft
    };

    ASSERT_TRUE(send_order(client_socket, msft_buy, 2));

    const auto second_response = receive_until_type(
        client_socket,
        exchange::protocol::MessageType::OrderAccepted
    );

    ASSERT_TRUE(second_response.has_value());

    const auto decoded_response =
        exchange::protocol::decode_order_response(
            second_response->body
        );

    ASSERT_TRUE(decoded_response.has_value());
    EXPECT_EQ(decoded_response->order_id, 42);
    EXPECT_EQ(decoded_response->success, 1);

    const auto second_book_message = receive_until_type(
        client_socket,
        exchange::protocol::MessageType::BookUpdate
    );

    ASSERT_TRUE(second_book_message.has_value());

    const auto second_book =
        exchange::protocol::decode_book_update(
            second_book_message->body
        );

    ASSERT_TRUE(second_book.has_value());
    EXPECT_EQ(
        exchange::protocol::symbol_to_string(
            second_book->symbol
        ),
        "MSFT"
    );

    /*
     * If the AAPL sell and MSFT buy shared one book,
     * this update would be empty after a trade. Instead the
     * MSFT bid rests independently.
     */
    EXPECT_EQ(second_book->has_bid, 1);
    EXPECT_EQ(second_book->best_bid, 100);
    EXPECT_EQ(second_book->bid_quantity, 10);
    EXPECT_EQ(second_book->has_ask, 0);

    close_client(client_socket);
    server.stop();
    server_thread.join();
}