#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "exchange/exchange_server.hpp"
#include "exchange/protocol.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

constexpr std::uint16_t benchmark_port = 19101;
constexpr std::size_t default_warmup_count = 1'000;
constexpr std::size_t default_sample_count = 10'000;

struct ReceivedMessage {
    exchange::protocol::MessageHeader header;
    std::vector<std::byte> body;
};

struct LatencySummary {
    std::uint64_t minimum_ns;
    double mean_ns;
    std::uint64_t p50_ns;
    std::uint64_t p95_ns;
    std::uint64_t p99_ns;
    std::uint64_t maximum_ns;
};

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

        total_sent +=
            static_cast<std::size_t>(sent);
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

std::optional<ReceivedMessage> receive_message(
    int socket
) {
    std::array<
        std::byte,
        exchange::protocol::header_size
    > header_bytes {};

    if (!receive_exact(socket, header_bytes)) {
        return std::nullopt;
    }

    const auto header =
        exchange::protocol::decode_header(
            header_bytes
        );

    if (!header.has_value()) {
        return std::nullopt;
    }

    std::vector<std::byte> body(
        static_cast<std::size_t>(
            header->body_size
        )
    );

    if (
        !body.empty() &&
        !receive_exact(socket, body)
    ) {
        return std::nullopt;
    }

    return ReceivedMessage {
        .header = *header,
        .body = std::move(body)
    };
}

bool receive_order_acceptance(
    int socket,
    std::uint64_t expected_order_id
) {
    const auto message = receive_message(socket);

    if (!message.has_value()) {
        return false;
    }

    if (
        message->header.type !=
        exchange::protocol::MessageType::OrderAccepted
    ) {
        return false;
    }

    const auto response =
        exchange::protocol::decode_order_response(
            message->body
        );

    if (!response.has_value()) {
        return false;
    }

    return
        response->order_id == expected_order_id &&
        response->success == 1;
}

bool receive_trade_execution(
    int socket,
    std::uint64_t expected_buy_order_id,
    std::uint64_t expected_sell_order_id
) {
    const auto message = receive_message(socket);

    if (!message.has_value()) {
        return false;
    }

    if (
        message->header.type !=
        exchange::protocol::MessageType::TradeExecution
    ) {
        return false;
    }

    const auto execution =
        exchange::protocol::decode_trade_execution(
            message->body
        );

    if (!execution.has_value()) {
        return false;
    }

    return
        execution->buy_order_id ==
            expected_buy_order_id &&
        execution->sell_order_id ==
            expected_sell_order_id &&
        execution->price == 100 &&
        execution->quantity == 1;
}

bool receive_book_update(int socket) {
    const auto message = receive_message(socket);

    if (!message.has_value()) {
        return false;
    }

    if (
        message->header.type !=
        exchange::protocol::MessageType::BookUpdate
    ) {
        return false;
    }

    return exchange::protocol::decode_book_update(
        message->body
    ).has_value();
}

int connect_to_server(std::uint16_t port) {
    for (
        int attempt = 0;
        attempt < 100;
        ++attempt
    ) {
        const int client_socket =
            ::socket(AF_INET, SOCK_STREAM, 0);

        if (client_socket < 0) {
            return -1;
        }

        int enabled = 1;

        ::setsockopt(
            client_socket,
            IPPROTO_TCP,
            TCP_NODELAY,
            &enabled,
            sizeof(enabled)
        );

#ifdef SO_NOSIGPIPE
        ::setsockopt(
            client_socket,
            SOL_SOCKET,
            SO_NOSIGPIPE,
            &enabled,
            sizeof(enabled)
        );
#endif

        timeval timeout {};
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        ::setsockopt(
            client_socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)
        );

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
            ::close(client_socket);
            return -1;
        }

        if (
            ::connect(
                client_socket,
                reinterpret_cast<sockaddr*>(
                    &address
                ),
                sizeof(address)
            ) == 0
        ) {
            return client_socket;
        }

        ::close(client_socket);

        std::this_thread::sleep_for(
            std::chrono::milliseconds(10)
        );
    }

    return -1;
}

template <
    std::size_t HeaderSize,
    std::size_t BodySize
>
std::vector<std::byte> combine_message(
    const std::array<std::byte, HeaderSize>& header,
    const std::array<std::byte, BodySize>& body
) {
    std::vector<std::byte> message;

    message.reserve(HeaderSize + BodySize);

    message.insert(
        message.end(),
        header.begin(),
        header.end()
    );

    message.insert(
        message.end(),
        body.begin(),
        body.end()
    );

    return message;
}

std::vector<std::byte> make_limit_order_message(
    std::uint64_t order_id,
    std::uint64_t sequence_number,
    exchange::protocol::Side side
) {
    const exchange::protocol::NewOrderRequest request {
        .order_id = order_id,
        .timestamp = sequence_number,
        .price = 100,
        .quantity = 1,
        .side = side,
        .order_type =
            exchange::protocol::OrderType::Limit
    };

    const auto body =
        exchange::protocol::encode_new_order(
            request
        );

    const exchange::protocol::MessageHeader header {
        .magic =
            exchange::protocol::protocol_magic,
        .version =
            exchange::protocol::protocol_version,
        .type =
            exchange::protocol::MessageType::NewOrder,
        .body_size =
            static_cast<std::uint32_t>(
                body.size()
            ),
        .sequence_number = sequence_number
    };

    const auto encoded_header =
        exchange::protocol::encode_header(header);

    return combine_message(
        encoded_header,
        body
    );
}

bool submit_resting_sell(
    int seller_socket,
    int buyer_socket,
    std::uint64_t sell_order_id,
    std::uint64_t sequence_number
) {
    const std::vector<std::byte> message =
        make_limit_order_message(
            sell_order_id,
            sequence_number,
            exchange::protocol::Side::Sell
        );

    if (!send_all(seller_socket, message)) {
        return false;
    }

    if (
        !receive_order_acceptance(
            seller_socket,
            sell_order_id
        )
    ) {
        return false;
    }

    /*
     * A resting sell changes the top of book.
     * Both connected clients receive that BookUpdate.
     */
    if (!receive_book_update(seller_socket)) {
        return false;
    }

    if (!receive_book_update(buyer_socket)) {
        return false;
    }

    return true;
}

bool run_execution_request(
    int buyer_socket,
    int seller_socket,
    std::uint64_t buy_order_id,
    std::uint64_t sell_order_id,
    std::uint64_t sequence_number,
    std::uint64_t* latency_ns
) {
    const std::vector<std::byte> message =
        make_limit_order_message(
            buy_order_id,
            sequence_number,
            exchange::protocol::Side::Buy
        );

    const Clock::time_point start =
        Clock::now();

    if (!send_all(buyer_socket, message)) {
        return false;
    }

    if (
        !receive_order_acceptance(
            buyer_socket,
            buy_order_id
        )
    ) {
        return false;
    }

    /*
     * Stop after the buyer receives its execution report.
     * This measures:
     *
     * client send
     * -> TCP
     * -> server receive/decode
     * -> matching
     * -> execution encoding
     * -> server send
     * -> buyer receives TradeExecution
     */
    if (
        !receive_trade_execution(
            buyer_socket,
            buy_order_id,
            sell_order_id
        )
    ) {
        return false;
    }

    const Clock::time_point completed =
        Clock::now();

    /*
     * Drain the seller's execution report.
     */
    if (
        !receive_trade_execution(
            seller_socket,
            buy_order_id,
            sell_order_id
        )
    ) {
        return false;
    }

    /*
     * The execution empties the book, producing a
     * BookUpdate for both clients.
     */
    if (!receive_book_update(buyer_socket)) {
        return false;
    }

    if (!receive_book_update(seller_socket)) {
        return false;
    }

    if (latency_ns != nullptr) {
        *latency_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    Nanoseconds
                >(
                    completed - start
                ).count()
            );
    }

    return true;
}

std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted,
    double probability
) {
    if (sorted.empty()) {
        return 0;
    }

    const double rank =
        std::ceil(
            probability *
            static_cast<double>(sorted.size())
        );

    const std::size_t index =
        static_cast<std::size_t>(
            std::max(1.0, rank) - 1.0
        );

    return sorted[
        std::min(
            index,
            sorted.size() - 1
        )
    ];
}

LatencySummary summarize(
    std::vector<std::uint64_t> samples
) {
    std::sort(
        samples.begin(),
        samples.end()
    );

    const std::uint64_t total =
        std::accumulate(
            samples.begin(),
            samples.end(),
            std::uint64_t {0}
        );

    return LatencySummary {
        .minimum_ns = samples.front(),
        .mean_ns =
            static_cast<double>(total) /
            static_cast<double>(samples.size()),
        .p50_ns = percentile(samples, 0.50),
        .p95_ns = percentile(samples, 0.95),
        .p99_ns = percentile(samples, 0.99),
        .maximum_ns = samples.back()
    };
}

void print_latency(
    const std::string& label,
    std::uint64_t nanoseconds
) {
    const double microseconds =
        static_cast<double>(nanoseconds) /
        1'000.0;

    std::cout
        << std::left
        << std::setw(10)
        << label
        << std::right
        << std::setw(12)
        << nanoseconds
        << " ns"
        << std::setw(12)
        << std::fixed
        << std::setprecision(3)
        << microseconds
        << " us\n";
}

void print_summary(
    const LatencySummary& summary,
    std::size_t sample_count
) {
    std::cout
        << "\nExchangeLab End-to-End Execution Latency\n"
        << "========================================\n"
        << "Samples: "
        << sample_count
        << "\n\n";

    print_latency(
        "Minimum",
        summary.minimum_ns
    );

    print_latency(
        "Mean",
        static_cast<std::uint64_t>(
            summary.mean_ns
        )
    );

    print_latency("p50", summary.p50_ns);
    print_latency("p95", summary.p95_ns);
    print_latency("p99", summary.p99_ns);
    print_latency("Maximum", summary.maximum_ns);

    const double tail_ratio =
        summary.p50_ns == 0
            ? 0.0
            : static_cast<double>(
                summary.p99_ns
            ) /
            static_cast<double>(
                summary.p50_ns
            );

    std::cout
        << "\np99 / p50: "
        << std::fixed
        << std::setprecision(2)
        << tail_ratio
        << "x\n";
}

bool write_csv(
    const std::filesystem::path& output_path,
    const std::vector<std::uint64_t>& samples
) {
    const std::filesystem::path parent =
        output_path.parent_path();

    if (!parent.empty()) {
        std::filesystem::create_directories(
            parent
        );
    }

    std::ofstream output(output_path);

    if (!output.is_open()) {
        return false;
    }

    output
        << "sample,latency_ns,latency_us\n";

    for (
        std::size_t index = 0;
        index < samples.size();
        ++index
    ) {
        output
            << index
            << ','
            << samples[index]
            << ','
            << std::fixed
            << std::setprecision(3)
            << (
                static_cast<double>(
                    samples[index]
                ) /
                1'000.0
            )
            << '\n';
    }

    return true;
}

std::size_t parse_count(
    const char* text,
    std::size_t fallback
) {
    try {
        const auto parsed =
            std::stoull(text);

        if (parsed == 0) {
            return fallback;
        }

        return static_cast<std::size_t>(
            parsed
        );
    } catch (...) {
        return fallback;
    }
}

void close_client(int socket) {
    if (socket >= 0) {
        ::shutdown(socket, SHUT_RDWR);
        ::close(socket);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t sample_count =
        argc >= 2
            ? parse_count(
                argv[1],
                default_sample_count
            )
            : default_sample_count;

    const std::size_t warmup_count =
        argc >= 3
            ? parse_count(
                argv[2],
                default_warmup_count
            )
            : default_warmup_count;

    const std::filesystem::path output_path =
        argc >= 4
            ? std::filesystem::path(argv[3])
            : std::filesystem::path(
                "benchmark-results/"
                "end_to_end_execution.csv"
            );

    exchange::ExchangeServer server(
        benchmark_port
    );

    if (!server.start()) {
        std::cerr
            << "Failed to start benchmark server on port "
            << benchmark_port
            << '\n';

        return 1;
    }

    std::thread server_thread([&server]() {
        server.run();
    });

    const int seller_socket =
        connect_to_server(benchmark_port);

    const int buyer_socket =
        connect_to_server(benchmark_port);

    if (
        seller_socket < 0 ||
        buyer_socket < 0
    ) {
        std::cerr
            << "Failed to connect benchmark clients\n";

        close_client(seller_socket);
        close_client(buyer_socket);

        server.stop();
        server_thread.join();

        return 1;
    }

    std::uint64_t next_order_id = 1;
    std::uint64_t next_sequence_number = 1;

    std::cout
        << "Running "
        << warmup_count
        << " warmup executions...\n";

    for (
        std::size_t index = 0;
        index < warmup_count;
        ++index
    ) {
        const std::uint64_t sell_order_id =
            next_order_id++;

        const std::uint64_t buy_order_id =
            next_order_id++;

        if (
            !submit_resting_sell(
                seller_socket,
                buyer_socket,
                sell_order_id,
                next_sequence_number++
            )
        ) {
            std::cerr
                << "Warmup sell failed at index "
                << index
                << '\n';

            close_client(seller_socket);
            close_client(buyer_socket);

            server.stop();
            server_thread.join();

            return 1;
        }

        if (
            !run_execution_request(
                buyer_socket,
                seller_socket,
                buy_order_id,
                sell_order_id,
                next_sequence_number++,
                nullptr
            )
        ) {
            std::cerr
                << "Warmup execution failed at index "
                << index
                << '\n';

            close_client(seller_socket);
            close_client(buyer_socket);

            server.stop();
            server_thread.join();

            return 1;
        }
    }

    std::cout
        << "Collecting "
        << sample_count
        << " execution-latency samples...\n";

    std::vector<std::uint64_t> samples;
    samples.reserve(sample_count);

    for (
        std::size_t index = 0;
        index < sample_count;
        ++index
    ) {
        const std::uint64_t sell_order_id =
            next_order_id++;

        const std::uint64_t buy_order_id =
            next_order_id++;

        if (
            !submit_resting_sell(
                seller_socket,
                buyer_socket,
                sell_order_id,
                next_sequence_number++
            )
        ) {
            std::cerr
                << "Resting sell failed at sample "
                << index
                << '\n';

            close_client(seller_socket);
            close_client(buyer_socket);

            server.stop();
            server_thread.join();

            return 1;
        }

        std::uint64_t latency_ns = 0;

        if (
            !run_execution_request(
                buyer_socket,
                seller_socket,
                buy_order_id,
                sell_order_id,
                next_sequence_number++,
                &latency_ns
            )
        ) {
            std::cerr
                << "Timed execution failed at sample "
                << index
                << '\n';

            close_client(seller_socket);
            close_client(buyer_socket);

            server.stop();
            server_thread.join();

            return 1;
        }

        samples.push_back(latency_ns);
    }

    close_client(seller_socket);
    close_client(buyer_socket);

    server.stop();
    server_thread.join();

    const LatencySummary summary =
        summarize(samples);

    print_summary(
        summary,
        samples.size()
    );

    if (!write_csv(output_path, samples)) {
        std::cerr
            << "Could not write results to "
            << output_path
            << '\n';

        return 1;
    }

    std::cout
        << "\nSaved samples to: "
        << output_path
        << '\n';

    return 0;
}