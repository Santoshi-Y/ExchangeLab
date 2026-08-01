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
    TradeExecution = 104,
    BookUpdate = 105
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

struct BookUpdate {
    std::uint8_t has_bid {};
    std::int64_t best_bid {};
    std::uint64_t bid_quantity {};

    std::uint8_t has_ask {};
    std::int64_t best_ask {};
    std::uint64_t ask_quantity {};

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

    const auto unsigned_value =
        static_cast<Unsigned>(value);

    for (
        std::size_t index = 0;
        index < sizeof(Integer);
        ++index
    ) {
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

    for (
        std::size_t index = 0;
        index < sizeof(Integer);
        ++index
    ) {
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
constexpr std::size_t order_response_body_size = 17;
constexpr std::size_t trade_execution_body_size = 40;
constexpr std::size_t book_update_body_size = 42;

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

    detail::write_integer(
        output,
        offset,
        header.body_size
    );

    detail::write_integer(
        output,
        offset,
        header.sequence_number
    );

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
        detail::read_integer<std::uint32_t>(
            input,
            offset
        );

    const auto version =
        detail::read_integer<std::uint16_t>(
            input,
            offset
        );

    const auto type =
        detail::read_integer<std::uint16_t>(
            input,
            offset
        );

    const auto body_size =
        detail::read_integer<std::uint32_t>(
            input,
            offset
        );

    const auto sequence_number =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    if (
        !magic ||
        !version ||
        !type ||
        !body_size ||
        !sequence_number
    ) {
        return std::nullopt;
    }

    if (
        *magic != protocol_magic ||
        *version != protocol_version
    ) {
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

inline std::array<std::byte, new_order_body_size>
encode_new_order(
    const NewOrderRequest& request
) {
    std::array<std::byte, new_order_body_size> output {};
    std::size_t offset = 0;

    detail::write_integer(
        output,
        offset,
        request.order_id
    );

    detail::write_integer(
        output,
        offset,
        request.timestamp
    );

    detail::write_integer(
        output,
        offset,
        request.price
    );

    detail::write_integer(
        output,
        offset,
        request.quantity
    );

    detail::write_integer(
        output,
        offset,
        static_cast<std::uint8_t>(request.side)
    );

    detail::write_integer(
        output,
        offset,
        static_cast<std::uint8_t>(
            request.order_type
        )
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
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    const auto timestamp =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    const auto price =
        detail::read_integer<std::int64_t>(
            input,
            offset
        );

    const auto quantity =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    const auto side =
        detail::read_integer<std::uint8_t>(
            input,
            offset
        );

    const auto order_type =
        detail::read_integer<std::uint8_t>(
            input,
            offset
        );

    if (
        !order_id ||
        !timestamp ||
        !price ||
        !quantity ||
        !side ||
        !order_type
    ) {
        return std::nullopt;
    }

    if (
        *side != static_cast<std::uint8_t>(Side::Buy) &&
        *side != static_cast<std::uint8_t>(Side::Sell)
    ) {
        return std::nullopt;
    }

    if (
        *order_type !=
            static_cast<std::uint8_t>(OrderType::Limit) &&
        *order_type !=
            static_cast<std::uint8_t>(OrderType::Market)
    ) {
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
        .order_type =
            static_cast<OrderType>(*order_type)
    };
}

inline std::array<std::byte, order_response_body_size>
encode_order_response(
    const OrderResponse& response
) {
    std::array<
        std::byte,
        order_response_body_size
    > output {};

    std::size_t offset = 0;

    detail::write_integer(
        output,
        offset,
        response.order_id
    );

    detail::write_integer(
        output,
        offset,
        response.sequence_number
    );

    detail::write_integer(
        output,
        offset,
        response.success
    );

    return output;
}

inline std::optional<OrderResponse>
decode_order_response(
    std::span<const std::byte> input
) {
    if (input.size() != order_response_body_size) {
        return std::nullopt;
    }

    std::size_t offset = 0;

    const auto order_id =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    const auto sequence_number =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    const auto success =
        detail::read_integer<std::uint8_t>(
            input,
            offset
        );

    if (
        !order_id ||
        !sequence_number ||
        !success
    ) {
        return std::nullopt;
    }

    if (*success != 0 && *success != 1) {
        return std::nullopt;
    }

    return OrderResponse {
        .order_id = *order_id,
        .sequence_number = *sequence_number,
        .success = *success
    };
}

inline std::array<
    std::byte,
    trade_execution_body_size
> encode_trade_execution(
    const TradeExecution& execution
) {
    std::array<
        std::byte,
        trade_execution_body_size
    > output {};

    std::size_t offset = 0;

    detail::write_integer(
        output,
        offset,
        execution.buy_order_id
    );

    detail::write_integer(
        output,
        offset,
        execution.sell_order_id
    );

    detail::write_integer(
        output,
        offset,
        execution.price
    );

    detail::write_integer(
        output,
        offset,
        execution.quantity
    );

    detail::write_integer(
        output,
        offset,
        execution.sequence_number
    );

    return output;
}

inline std::optional<TradeExecution>
decode_trade_execution(
    std::span<const std::byte> input
) {
    if (input.size() != trade_execution_body_size) {
        return std::nullopt;
    }

    std::size_t offset = 0;

    const auto buy_order_id =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    const auto sell_order_id =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    const auto price =
        detail::read_integer<std::int64_t>(
            input,
            offset
        );

    const auto quantity =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    const auto sequence_number =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    if (
        !buy_order_id ||
        !sell_order_id ||
        !price ||
        !quantity ||
        !sequence_number
    ) {
        return std::nullopt;
    }

    if (*quantity == 0) {
        return std::nullopt;
    }

    return TradeExecution {
        .buy_order_id = *buy_order_id,
        .sell_order_id = *sell_order_id,
        .price = *price,
        .quantity = *quantity,
        .sequence_number = *sequence_number
    };
}

inline std::array<
    std::byte,
    book_update_body_size
> encode_book_update(
    const BookUpdate& update
) {
    std::array<
        std::byte,
        book_update_body_size
    > output {};

    std::size_t offset = 0;

    detail::write_integer(
        output,
        offset,
        update.has_bid
    );

    detail::write_integer(
        output,
        offset,
        update.best_bid
    );

    detail::write_integer(
        output,
        offset,
        update.bid_quantity
    );

    detail::write_integer(
        output,
        offset,
        update.has_ask
    );

    detail::write_integer(
        output,
        offset,
        update.best_ask
    );

    detail::write_integer(
        output,
        offset,
        update.ask_quantity
    );

    detail::write_integer(
        output,
        offset,
        update.sequence_number
    );

    return output;
}

inline std::optional<BookUpdate> decode_book_update(
    std::span<const std::byte> input
) {
    if (input.size() != book_update_body_size) {
        return std::nullopt;
    }

    std::size_t offset = 0;

    const auto has_bid =
        detail::read_integer<std::uint8_t>(
            input,
            offset
        );

    const auto best_bid =
        detail::read_integer<std::int64_t>(
            input,
            offset
        );

    const auto bid_quantity =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    const auto has_ask =
        detail::read_integer<std::uint8_t>(
            input,
            offset
        );

    const auto best_ask =
        detail::read_integer<std::int64_t>(
            input,
            offset
        );

    const auto ask_quantity =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    const auto sequence_number =
        detail::read_integer<std::uint64_t>(
            input,
            offset
        );

    if (
        !has_bid ||
        !best_bid ||
        !bid_quantity ||
        !has_ask ||
        !best_ask ||
        !ask_quantity ||
        !sequence_number
    ) {
        return std::nullopt;
    }

    if (
        (*has_bid != 0 && *has_bid != 1) ||
        (*has_ask != 0 && *has_ask != 1)
    ) {
        return std::nullopt;
    }

    if (
        *has_bid == 0 &&
        (*best_bid != 0 || *bid_quantity != 0)
    ) {
        return std::nullopt;
    }

    if (
        *has_ask == 0 &&
        (*best_ask != 0 || *ask_quantity != 0)
    ) {
        return std::nullopt;
    }

    return BookUpdate {
        .has_bid = *has_bid,
        .best_bid = *best_bid,
        .bid_quantity = *bid_quantity,
        .has_ask = *has_ask,
        .best_ask = *best_ask,
        .ask_quantity = *ask_quantity,
        .sequence_number = *sequence_number
    };
}

}  // namespace exchange::protocol