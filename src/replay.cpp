#include "exchange/replay.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <vector>

#include "exchange/order.hpp"
#include "exchange/protocol.hpp"
#include "exchange/types.hpp"

namespace exchange {

namespace {

constexpr std::size_t record_length_size =
    sizeof(std::uint32_t);

constexpr std::uint32_t maximum_record_size =
    1024U * 1024U;

std::uint32_t decode_record_length(
    const std::array<
        std::byte,
        record_length_size
    >& bytes
) {
    std::uint32_t value = 0;

    for (
        std::size_t index = 0;
        index < bytes.size();
        ++index
    ) {
        value |=
            static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(
                    bytes[index]
                )
            ) << (index * 8U);
    }

    return value;
}

Side convert_side(protocol::Side side) {
    return side == protocol::Side::Buy
        ? Side::Buy
        : Side::Sell;
}

OrderType convert_order_type(
    protocol::OrderType type
) {
    return type == protocol::OrderType::Limit
        ? OrderType::Limit
        : OrderType::Market;
}

}  // namespace

ExchangeReplayer::ExchangeReplayer() {
    reset();
}

bool ExchangeReplayer::replay(
    const std::filesystem::path& journal_path
) {
    reset();

    std::ifstream input(
        journal_path,
        std::ios::binary
    );

    if (!input.is_open()) {
        return false;
    }

    while (true) {
        std::array<
            std::byte,
            record_length_size
        > length_bytes {};

        input.read(
            reinterpret_cast<char*>(
                length_bytes.data()
            ),
            static_cast<std::streamsize>(
                length_bytes.size()
            )
        );

        if (input.eof()) {
            break;
        }

        if (!input) {
            ++summary_.rejected_messages;
            return false;
        }

        const std::uint32_t record_size =
            decode_record_length(length_bytes);

        if (
            record_size < protocol::header_size ||
            record_size > maximum_record_size
        ) {
            ++summary_.rejected_messages;
            return false;
        }

        std::vector<std::byte> record(
            static_cast<std::size_t>(
                record_size
            )
        );

        input.read(
            reinterpret_cast<char*>(
                record.data()
            ),
            static_cast<std::streamsize>(
                record.size()
            )
        );

        if (!input) {
            ++summary_.rejected_messages;
            return false;
        }

        ++summary_.journal_records;

        const std::span<const std::byte>
            header_bytes {
                record.data(),
                protocol::header_size
            };

        const auto header =
            protocol::decode_header(
                header_bytes
            );

        if (!header.has_value()) {
            ++summary_.rejected_messages;
            continue;
        }

        const std::size_t expected_size =
            protocol::header_size +
            static_cast<std::size_t>(
                header->body_size
            );

        if (record.size() != expected_size) {
            ++summary_.rejected_messages;
            continue;
        }

        const std::span<const std::byte> body {
            record.data() + protocol::header_size,
            static_cast<std::size_t>(
                header->body_size
            )
        };

        if (
            header->type !=
            protocol::MessageType::NewOrder
        ) {
            ++summary_.unsupported_messages;
            continue;
        }

        const auto request =
            protocol::decode_new_order(body);

        if (!request.has_value()) {
            ++summary_.rejected_messages;
            continue;
        }

        if (
            request->quantity >
            static_cast<std::uint64_t>(
                std::numeric_limits<Quantity>::max()
            )
        ) {
            ++summary_.rejected_messages;
            continue;
        }

        const Quantity quantity =
            static_cast<Quantity>(
                request->quantity
            );

        const Order order {
            .id = request->order_id,
            .side = convert_side(request->side),
            .type =
                convert_order_type(
                    request->order_type
                ),
            .time_in_force =
                request->order_type ==
                        protocol::OrderType::Market
                    ? TimeInForce::ImmediateOrCancel
                    : TimeInForce::GoodTillCancel,
            .price = request->price,
            .initial_quantity = quantity,
            .remaining_quantity = quantity,
            .timestamp = request->timestamp
        };

        try {
            engine_.process_order_into(
                *book_,
                order,
                trade_buffer_
            );

            ++summary_.new_orders;

            summary_.trades +=
                static_cast<std::uint64_t>(
                    trade_buffer_.size()
                );
        } catch (...) {
            ++summary_.rejected_messages;
        }
    }

    return true;
}

const ReplaySummary&
ExchangeReplayer::summary() const noexcept {
    return summary_;
}

const OrderBook&
ExchangeReplayer::order_book() const noexcept {
    return *book_;
}

void ExchangeReplayer::reset() {
    book_ = std::make_unique<OrderBook>();
    trade_buffer_.clear();
    summary_ = ReplaySummary {};
}

}  // namespace exchange