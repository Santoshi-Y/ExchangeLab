#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace exchange::protocol {

constexpr std::uint32_t protocol_magic = 0x45584C42;  // "EXLB"
constexpr std::uint16_t protocol_version = 1;

enum class MessageType : std::uint16_t {
    NewOrder = 1,
    CancelOrder = 2,
    ReplaceOrder = 3,

    OrderAccepted = 100,
    OrderRejected = 101,
    OrderCancelled = 102,
    OrderReplaced = 103,
    TradeExecution = 104
};

enum class Side : std::uint8_t {
    Buy = 1,
    Sell = 2
};

enum class OrderType : std::uint8_t {
    Limit = 1,
    Market = 2
};

struct MessageHeader {
    std::uint32_t magic {};
    std::uint16_t version {};
    MessageType type {};
    std::uint32_t body_size {};
    std::uint64_t sequence_number {};
};

struct NewOrderRequest {
    std::uint64_t order_id {};
    std::uint64_t timestamp {};
    std::int64_t price {};
    std::uint64_t quantity {};
    Side side {};
    OrderType order_type {};
};

struct CancelOrderRequest {
    std::uint64_t order_id {};
    std::uint64_t timestamp {};
};

struct ReplaceOrderRequest {
    std::uint64_t order_id {};
    std::uint64_t timestamp {};
    std::int64_t new_price {};
    std::uint64_t new_quantity {};
};

struct OrderResponse {
    std::uint64_t order_id {};
    std::uint64_t sequence_number {};
    std::uint8_t success {};
};

struct TradeExecution {
    std::uint64_t buy_order_id {};
    std::uint64_t sell_order_id {};
    std::int64_t price {};
    std::uint64_t quantity {};
    std::uint64_t sequence_number {};
};

namespace detail {

template <typename Integer>
requires std::is_integral_v<Integer>
void write_integer(
    std::span<std::byte> output,
    std::size_t& offset,
    Integer value
) {
    using Unsigned = std::make_unsigned_t<Integer>;
    const auto unsigned_value = static_cast<Unsigned>(value);

    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output[offset++] = static_cast<std::byte>(
            (unsigned_value >> (index * 8U)) & 0xFFU
        );
    }
}

template <typename Integer>
requires std::is_integral_v<Integer>
std::optional<Integer> read_integer(
    std::span<const std::byte> input,
    std::size_t& offset
) {
    if (offset + sizeof(Integer) > input.size()) {
        return std::nullopt;
    }

    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned value = 0;

    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        const auto byte_value =
            std::to_integer<Unsigned>(input[offset++]);

        value |= byte_value << (index * 8U);
    }

    return static_cast<Integer>(value);
}

}  // namespace detail

constexpr std::size_t header_size = 20;
constexpr std::size_t new_order_body_size = 34;
constexpr std::size_t cancel_order_body_size = 16;
constexpr std::size_t replace_order_body_size = 32;

inline std::array<std::byte, header_size> encode_header(
    const MessageHeader& header
) {
    std::array<std::byte, header_size> output {};
    std::size_t offset = 0;

    detail::write_integer(output, offset, header.magic);
    detail::write_integer(output, offset, header.version);
    detail::write_integer(
        output,
        offset,
        static_cast<std::uint16_t>(header.type)
    );
    detail::write_integer(output, offset, header.body_size);
    detail::write_integer(output, offset, header.sequence_number);

    return output;
}

inline std::optional<MessageHeader> decode_header(
    std::span<const std::byte> input
) {
    if (input.size() != header_size) {
        return std::nullopt;
    }

    std::size_t offset = 0;

    const auto magic =
        detail::read_integer<std::uint32_t>(input, offset);
    const auto version =
        detail::read_integer<std::uint16_t>(input, offset);
    const auto type =
        detail::read_integer<std::uint16_t>(input, offset);
    const auto body_size =
        detail::read_integer<std::uint32_t>(input, offset);
    const auto sequence_number =
        detail::read_integer<std::uint64_t>(input, offset);

    if (!magic || !version || !type || !body_size || !sequence_number) {
        return std::nullopt;
    }

    if (*magic != protocol_magic || *version != protocol_version) {
        return std::nullopt;
    }

    return MessageHeader {
        .magic = *magic,
        .version = *version,
        .type = static_cast<MessageType>(*type),
        .body_size = *body_size,
        .sequence_number = *sequence_number
    };
}

inline std::array<std::byte, new_order_body_size> encode_new_order(
    const NewOrderRequest& request
) {
    std::array<std::byte, new_order_body_size> output {};
    std::size_t offset = 0;

    detail::write_integer(output, offset, request.order_id);
    detail::write_integer(output, offset, request.timestamp);
    detail::write_integer(output, offset, request.price);
    detail::write_integer(output, offset, request.quantity);
    detail::write_integer(
        output,
        offset,
        static_cast<std::uint8_t>(request.side)
    );
    detail::write_integer(
        output,
        offset,
        static_cast<std::uint8_t>(request.order_type)
    );

    return output;
}

inline std::optional<NewOrderRequest> decode_new_order(
    std::span<const std::byte> input
) {
    if (input.size() != new_order_body_size) {
        return std::nullopt;
    }

    std::size_t offset = 0;

    const auto order_id =
        detail::read_integer<std::uint64_t>(input, offset);
    const auto timestamp =
        detail::read_integer<std::uint64_t>(input, offset);
    const auto price =
        detail::read_integer<std::int64_t>(input, offset);
    const auto quantity =
        detail::read_integer<std::uint64_t>(input, offset);
    const auto side =
        detail::read_integer<std::uint8_t>(input, offset);
    const auto order_type =
        detail::read_integer<std::uint8_t>(input, offset);

    if (!order_id || !timestamp || !price || !quantity ||
        !side || !order_type) {
        return std::nullopt;
    }

    if (*side != static_cast<std::uint8_t>(Side::Buy) &&
        *side != static_cast<std::uint8_t>(Side::Sell)) {
        return std::nullopt;
    }

    if (*order_type != static_cast<std::uint8_t>(OrderType::Limit) &&
        *order_type != static_cast<std::uint8_t>(OrderType::Market)) {
        return std::nullopt;
    }

    if (*quantity == 0) {
        return std::nullopt;
    }

    return NewOrderRequest {
        .order_id = *order_id,
        .timestamp = *timestamp,
        .price = *price,
        .quantity = *quantity,
        .side = static_cast<Side>(*side),
        .order_type = static_cast<OrderType>(*order_type)
    };
}

}  // namespace exchange::protocol