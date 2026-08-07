#include "exchange/fix_gateway.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace exchange {
namespace {

using namespace std::chrono_literals;

struct CoreMessage {
    protocol::MessageHeader header;
    std::vector<std::byte> body;
};

enum class PendingKind {
    New,
    Cancel,
    Replace
};

struct PendingRequest {
    PendingKind kind {PendingKind::New};
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::uint64_t replacement_total_quantity {0};
    std::uint64_t replacement_working_quantity {0};
    std::int64_t replacement_price {0};
};

struct OrderState {
    std::uint64_t numeric_order_id {0};
    protocol::Symbol symbol {protocol::default_symbol};
    protocol::Side side {protocol::Side::Buy};
    protocol::OrderType order_type {protocol::OrderType::Limit};
    protocol::TimeInForce time_in_force {
        protocol::TimeInForce::GoodTillCancel
    };

    std::string current_cl_ord_id;
    std::uint64_t order_quantity {0};
    std::uint64_t cumulative_quantity {0};
    std::uint64_t leaves_quantity {0};
    long double cumulative_notional {0.0L};
    std::int64_t price {0};
    char ord_status {'0'};
};

struct SessionContext {
    int client_socket {-1};
    int core_socket {-1};

    bool logged_on {false};
    bool closing {false};

    std::string client_comp_id;
    std::uint32_t heartbeat_seconds {30};

    std::uint64_t expected_inbound_sequence {1};
    std::uint64_t next_outbound_sequence {1};
    std::uint64_t next_core_sequence {1};

    std::string fix_receive_buffer;
    std::vector<std::byte> core_receive_buffer;

    std::unordered_map<std::uint64_t, OrderState> orders;
    std::unordered_map<std::string, std::uint64_t> clord_to_order;
    std::unordered_set<std::string> used_clord_ids;
    std::unordered_map<
        std::uint64_t,
        std::deque<PendingRequest>
    > pending;
    std::unordered_set<std::uint64_t> non_resting_awaiting_boundary;

    std::string last_self_trade_key;
    unsigned int self_trade_duplicate_messages_to_ignore {0};

    std::chrono::steady_clock::time_point last_fix_receive {
        std::chrono::steady_clock::now()
    };

    std::chrono::steady_clock::time_point last_fix_send {
        std::chrono::steady_clock::now()
    };
};

[[nodiscard]] std::uint64_t now_nanoseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

[[nodiscard]] bool send_all_bytes(
    int socket,
    const void* data,
    std::size_t size
) {
    const auto* bytes = static_cast<const std::byte*>(data);
    std::size_t sent_total = 0;

    while (sent_total < size) {
        const auto sent = ::send(
            socket,
            bytes + sent_total,
            size - sent_total,
            0
        );

        if (sent <= 0) {
            return false;
        }

        sent_total += static_cast<std::size_t>(sent);
    }

    return true;
}

[[nodiscard]] bool send_fix_wire(
    SessionContext& session,
    std::string_view wire
) {
    if (
        !send_all_bytes(
            session.client_socket,
            wire.data(),
            wire.size()
        )
    ) {
        return false;
    }

    session.last_fix_send = std::chrono::steady_clock::now();
    return true;
}

[[nodiscard]] std::string build_session_message(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::string_view message_type,
    const std::vector<fix::Field>& fields = {}
) {
    fix::Message message;
    message.add(35, std::string(message_type));
    message.add(
        34,
        std::to_string(session.next_outbound_sequence++)
    );
    message.add(49, config.sender_comp_id);
    message.add(
        56,
        session.client_comp_id.empty()
            ? "UNKNOWN"
            : session.client_comp_id
    );
    message.add(52, fix::utc_timestamp());

    for (const fix::Field& field : fields) {
        message.add(field.tag, field.value);
    }

    return fix::encode(message);
}

[[nodiscard]] bool send_session_message(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::string_view message_type,
    const std::vector<fix::Field>& fields = {}
) {
    return send_fix_wire(
        session,
        build_session_message(
            session,
            config,
            message_type,
            fields
        )
    );
}

[[nodiscard]] std::optional<std::string_view> required_field(
    const fix::Message& message,
    int tag,
    std::string& error
) {
    const auto value = message.get(tag);

    if (!value.has_value() || value->empty()) {
        error = "required FIX tag " + std::to_string(tag) + " is missing";
        return std::nullopt;
    }

    return value;
}

[[nodiscard]] std::string side_text(protocol::Side side) {
    return side == protocol::Side::Buy ? "1" : "2";
}

[[nodiscard]] std::string ord_type_text(protocol::OrderType type) {
    return type == protocol::OrderType::Limit ? "2" : "1";
}

[[nodiscard]] std::string tif_text(protocol::TimeInForce tif) {
    switch (tif) {
        case protocol::TimeInForce::GoodTillCancel:
            return "1";
        case protocol::TimeInForce::ImmediateOrCancel:
            return "3";
        case protocol::TimeInForce::FillOrKill:
            return "4";
    }

    return "1";
}

[[nodiscard]] std::string average_price_text(
    const OrderState& order
) {
    if (order.cumulative_quantity == 0) {
        return "0";
    }

    const long double average =
        order.cumulative_notional /
        static_cast<long double>(order.cumulative_quantity);

    const auto rounded = static_cast<std::int64_t>(average);

    if (average == static_cast<long double>(rounded)) {
        return std::to_string(rounded);
    }

    std::string value = std::to_string(
        static_cast<double>(average)
    );

    while (value.size() > 1 && value.back() == '0') {
        value.pop_back();
    }

    if (!value.empty() && value.back() == '.') {
        value.pop_back();
    }

    return value;
}

[[nodiscard]] bool send_execution_report(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::atomic<std::uint64_t>& next_exec_id,
    const OrderState& order,
    std::string_view cl_ord_id,
    std::string_view exec_type,
    char ord_status,
    std::optional<std::string_view> orig_cl_ord_id = std::nullopt,
    std::optional<std::uint64_t> last_quantity = std::nullopt,
    std::optional<std::int64_t> last_price = std::nullopt,
    std::optional<std::string_view> text = std::nullopt
) {
    std::vector<fix::Field> fields;
    fields.reserve(22);

    fields.push_back({37, std::to_string(order.numeric_order_id)});
    fields.push_back({11, std::string(cl_ord_id)});

    if (orig_cl_ord_id.has_value()) {
        fields.push_back({41, std::string(*orig_cl_ord_id)});
    }

    fields.push_back({17, std::to_string(next_exec_id.fetch_add(1))});
    fields.push_back({150, std::string(exec_type)});
    fields.push_back({39, std::string(1, ord_status)});
    fields.push_back({55, protocol::symbol_to_string(order.symbol)});
    fields.push_back({54, side_text(order.side)});
    fields.push_back({38, std::to_string(order.order_quantity)});
    fields.push_back({40, ord_type_text(order.order_type)});

    if (order.order_type == protocol::OrderType::Limit) {
        fields.push_back({44, std::to_string(order.price)});
    }

    fields.push_back({59, tif_text(order.time_in_force)});
    fields.push_back({14, std::to_string(order.cumulative_quantity)});
    fields.push_back({151, std::to_string(order.leaves_quantity)});
    fields.push_back({6, average_price_text(order)});

    if (last_quantity.has_value()) {
        fields.push_back({32, std::to_string(*last_quantity)});
    }

    if (last_price.has_value()) {
        fields.push_back({31, std::to_string(*last_price)});
    }

    if (text.has_value()) {
        fields.push_back({58, std::string(*text)});
    }

    return send_session_message(
        session,
        config,
        "8",
        fields
    );
}

[[nodiscard]] bool send_new_reject(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::atomic<std::uint64_t>& next_exec_id,
    std::string_view cl_ord_id,
    std::string_view symbol,
    std::string_view side,
    std::string_view quantity,
    std::string_view reason
) {
    const std::uint64_t exec_id = next_exec_id.fetch_add(1);

    return send_session_message(
        session,
        config,
        "8",
        {
            {37, "NONE"},
            {11, std::string(cl_ord_id)},
            {17, std::to_string(exec_id)},
            {150, "8"},
            {39, "8"},
            {55, std::string(symbol)},
            {54, std::string(side)},
            {38, std::string(quantity)},
            {14, "0"},
            {151, "0"},
            {6, "0"},
            {58, std::string(reason)}
        }
    );
}

[[nodiscard]] bool send_cancel_reject(
    SessionContext& session,
    const FixGatewayConfig& config,
    const OrderState* order,
    std::string_view cl_ord_id,
    std::string_view orig_cl_ord_id,
    PendingKind kind,
    std::string_view reason
) {
    return send_session_message(
        session,
        config,
        "9",
        {
            {
                37,
                order != nullptr
                    ? std::to_string(order->numeric_order_id)
                    : "NONE"
            },
            {11, std::string(cl_ord_id)},
            {41, std::string(orig_cl_ord_id)},
            {
                39,
                order != nullptr
                    ? std::string(1, order->ord_status)
                    : "8"
            },
            {
                434,
                kind == PendingKind::Cancel ? "1" : "2"
            },
            {102, "99"},
            {58, std::string(reason)}
        }
    );
}

[[nodiscard]] bool send_session_reject(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::uint64_t reference_sequence,
    std::string_view reason,
    std::optional<std::string_view> reference_message_type = std::nullopt
) {
    std::vector<fix::Field> fields {
        {45, std::to_string(reference_sequence)},
        {373, "99"},
        {58, std::string(reason)}
    };

    if (reference_message_type.has_value()) {
        fields.push_back({372, std::string(*reference_message_type)});
    }

    return send_session_message(
        session,
        config,
        "3",
        fields
    );
}

[[nodiscard]] int connect_to_exchange(
    const std::string& host,
    std::uint16_t port
) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = nullptr;
    const std::string port_text = std::to_string(port);

    if (
        ::getaddrinfo(
            host.c_str(),
            port_text.c_str(),
            &hints,
            &addresses
        ) != 0
    ) {
        return -1;
    }

    int connected_socket = -1;

    for (
        addrinfo* current = addresses;
        current != nullptr;
        current = current->ai_next
    ) {
        const int socket = ::socket(
            current->ai_family,
            current->ai_socktype,
            current->ai_protocol
        );

        if (socket < 0) {
            continue;
        }

        if (
            ::connect(
                socket,
                current->ai_addr,
                current->ai_addrlen
            ) == 0
        ) {
            connected_socket = socket;
            break;
        }

        ::close(socket);
    }

    ::freeaddrinfo(addresses);
    return connected_socket;
}

template <std::size_t BodySize>
[[nodiscard]] bool send_core_request(
    SessionContext& session,
    protocol::MessageType type,
    const std::array<std::byte, BodySize>& body
) {
    const protocol::MessageHeader header {
        .magic = protocol::protocol_magic,
        .version = protocol::protocol_version,
        .type = type,
        .body_size = static_cast<std::uint32_t>(body.size()),
        .sequence_number = session.next_core_sequence++
    };

    const auto encoded_header = protocol::encode_header(header);

    std::vector<std::byte> wire;
    wire.reserve(encoded_header.size() + body.size());
    wire.insert(wire.end(), encoded_header.begin(), encoded_header.end());
    wire.insert(wire.end(), body.begin(), body.end());

    return send_all_bytes(
        session.core_socket,
        wire.data(),
        wire.size()
    );
}

[[nodiscard]] bool extract_core_message(
    std::vector<std::byte>& buffer,
    CoreMessage& output
) {
    if (buffer.size() < protocol::header_size) {
        return false;
    }

    const std::span<const std::byte> header_span(
        buffer.data(),
        protocol::header_size
    );

    const auto header = protocol::decode_header(header_span);
    if (!header.has_value()) {
        buffer.clear();
        return false;
    }

    const std::size_t body_size =
        static_cast<std::size_t>(header->body_size);

    if (
        body_size >
        std::numeric_limits<std::size_t>::max() -
            protocol::header_size
    ) {
        buffer.clear();
        return false;
    }

    const std::size_t total_size = protocol::header_size + body_size;
    if (buffer.size() < total_size) {
        return false;
    }

    output.header = *header;
    output.body.assign(
        buffer.begin() + static_cast<std::ptrdiff_t>(protocol::header_size),
        buffer.begin() + static_cast<std::ptrdiff_t>(total_size)
    );

    buffer.erase(
        buffer.begin(),
        buffer.begin() + static_cast<std::ptrdiff_t>(total_size)
    );

    return true;
}

[[nodiscard]] std::optional<std::uint64_t> sequence_number(
    const fix::Message& message
) {
    const auto value = message.get(34);
    if (!value.has_value()) {
        return std::nullopt;
    }

    return fix::parse_u64(*value);
}

[[nodiscard]] bool validate_comp_ids(
    const fix::Message& message,
    const SessionContext& session,
    const FixGatewayConfig& config,
    std::string& error
) {
    const auto sender = message.get(49);
    const auto target = message.get(56);

    if (!sender.has_value() || !target.has_value()) {
        error = "SenderCompID (49) and TargetCompID (56) are required";
        return false;
    }

    if (*target != config.sender_comp_id) {
        error = "TargetCompID does not match gateway";
        return false;
    }

    if (
        session.logged_on &&
        *sender != session.client_comp_id
    ) {
        error = "SenderCompID changed during session";
        return false;
    }

    return true;
}

[[nodiscard]] bool ensure_unique_clordid(
    SessionContext& session,
    std::string_view cl_ord_id
) {
    return session.used_clord_ids.emplace(cl_ord_id).second;
}

[[nodiscard]] std::optional<std::uint64_t> lookup_order_id(
    const SessionContext& session,
    std::string_view cl_ord_id
) {
    const auto found = session.clord_to_order.find(
        std::string(cl_ord_id)
    );

    if (found == session.clord_to_order.end()) {
        return std::nullopt;
    }

    return found->second;
}

[[nodiscard]] bool handle_new_order(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::atomic<std::uint64_t>& next_numeric_order_id,
    std::atomic<std::uint64_t>& next_exec_id,
    const fix::Message& message
) {
    std::string error;

    const std::string cl_ord_id = message.get(11).has_value()
        ? std::string(*message.get(11))
        : "MISSING";

    const std::string symbol = message.get(55).has_value()
        ? std::string(*message.get(55))
        : "UNKNOWN";

    const std::string side = message.get(54).has_value()
        ? std::string(*message.get(54))
        : "?";

    const std::string quantity = message.get(38).has_value()
        ? std::string(*message.get(38))
        : "0";

    if (
        message.get(11).has_value() &&
        !ensure_unique_clordid(session, *message.get(11))
    ) {
        return send_new_reject(
            session,
            config,
            next_exec_id,
            cl_ord_id,
            symbol,
            side,
            quantity,
            "duplicate ClOrdID"
        );
    }

    const std::uint64_t numeric_order_id =
        next_numeric_order_id.fetch_add(1);

    auto translated = fixbridge::translate_new_order_single(
        message,
        numeric_order_id,
        now_nanoseconds(),
        error
    );

    if (!translated.has_value()) {
        return send_new_reject(
            session,
            config,
            next_exec_id,
            cl_ord_id,
            symbol,
            side,
            quantity,
            error
        );
    }

    OrderState order {
        .numeric_order_id = numeric_order_id,
        .symbol = translated->request.symbol,
        .side = translated->request.side,
        .order_type = translated->request.order_type,
        .time_in_force = translated->request.time_in_force,
        .current_cl_ord_id = translated->cl_ord_id,
        .order_quantity = translated->request.quantity,
        .cumulative_quantity = 0,
        .leaves_quantity = translated->request.quantity,
        .cumulative_notional = 0.0L,
        .price = translated->request.price,
        .ord_status = '0'
    };

    session.orders.emplace(numeric_order_id, order);
    session.clord_to_order.emplace(
        translated->cl_ord_id,
        numeric_order_id
    );

    session.pending[numeric_order_id].push_back(
        PendingRequest {
            .kind = PendingKind::New,
            .cl_ord_id = translated->cl_ord_id,
            .orig_cl_ord_id = {},
            .replacement_total_quantity = 0,
            .replacement_working_quantity = 0,
            .replacement_price = 0
        }
    );

    if (
        !send_core_request(
            session,
            protocol::MessageType::NewOrder,
            protocol::encode_new_order(translated->request)
        )
    ) {
        session.pending[numeric_order_id].pop_back();
        session.clord_to_order.erase(translated->cl_ord_id);
        session.orders.erase(numeric_order_id);

        return send_new_reject(
            session,
            config,
            next_exec_id,
            translated->cl_ord_id,
            protocol::symbol_to_string(translated->request.symbol),
            side_text(translated->request.side),
            std::to_string(translated->request.quantity),
            "core exchange connection failed"
        );
    }

    return true;
}

[[nodiscard]] bool handle_cancel(
    SessionContext& session,
    const FixGatewayConfig& config,
    const fix::Message& message
) {
    std::string error;

    const auto cl_ord_id = required_field(message, 11, error);
    const auto orig_cl_ord_id = required_field(message, 41, error);

    if (!cl_ord_id.has_value() || !orig_cl_ord_id.has_value()) {
        return send_cancel_reject(
            session,
            config,
            nullptr,
            cl_ord_id.has_value() ? *cl_ord_id : "MISSING",
            orig_cl_ord_id.has_value() ? *orig_cl_ord_id : "MISSING",
            PendingKind::Cancel,
            error
        );
    }

    if (!ensure_unique_clordid(session, *cl_ord_id)) {
        return send_cancel_reject(
            session,
            config,
            nullptr,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Cancel,
            "duplicate ClOrdID"
        );
    }

    const auto numeric_order_id = lookup_order_id(
        session,
        *orig_cl_ord_id
    );

    if (!numeric_order_id.has_value()) {
        return send_cancel_reject(
            session,
            config,
            nullptr,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Cancel,
            "unknown OrigClOrdID"
        );
    }

    auto order_it = session.orders.find(*numeric_order_id);
    if (
        order_it == session.orders.end() ||
        order_it->second.leaves_quantity == 0
    ) {
        return send_cancel_reject(
            session,
            config,
            order_it == session.orders.end() ? nullptr : &order_it->second,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Cancel,
            "order is no longer working"
        );
    }

    const protocol::CancelOrderRequest request {
        .order_id = *numeric_order_id,
        .timestamp = now_nanoseconds(),
        .symbol = order_it->second.symbol
    };

    session.pending[*numeric_order_id].push_back(
        PendingRequest {
            .kind = PendingKind::Cancel,
            .cl_ord_id = std::string(*cl_ord_id),
            .orig_cl_ord_id = std::string(*orig_cl_ord_id)
        }
    );

    if (
        !send_core_request(
            session,
            protocol::MessageType::CancelOrder,
            protocol::encode_cancel_order(request)
        )
    ) {
        session.pending[*numeric_order_id].pop_back();

        return send_cancel_reject(
            session,
            config,
            &order_it->second,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Cancel,
            "core exchange connection failed"
        );
    }

    return true;
}

[[nodiscard]] bool handle_replace(
    SessionContext& session,
    const FixGatewayConfig& config,
    const fix::Message& message
) {
    std::string error;

    const auto cl_ord_id = required_field(message, 11, error);
    const auto orig_cl_ord_id = required_field(message, 41, error);

    if (!cl_ord_id.has_value() || !orig_cl_ord_id.has_value()) {
        return send_cancel_reject(
            session,
            config,
            nullptr,
            cl_ord_id.has_value() ? *cl_ord_id : "MISSING",
            orig_cl_ord_id.has_value() ? *orig_cl_ord_id : "MISSING",
            PendingKind::Replace,
            error
        );
    }

    if (!ensure_unique_clordid(session, *cl_ord_id)) {
        return send_cancel_reject(
            session,
            config,
            nullptr,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Replace,
            "duplicate ClOrdID"
        );
    }

    const auto numeric_order_id = lookup_order_id(
        session,
        *orig_cl_ord_id
    );

    if (!numeric_order_id.has_value()) {
        return send_cancel_reject(
            session,
            config,
            nullptr,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Replace,
            "unknown OrigClOrdID"
        );
    }

    auto order_it = session.orders.find(*numeric_order_id);
    if (
        order_it == session.orders.end() ||
        order_it->second.leaves_quantity == 0
    ) {
        return send_cancel_reject(
            session,
            config,
            order_it == session.orders.end() ? nullptr : &order_it->second,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Replace,
            "order is no longer working"
        );
    }

    OrderState& order = order_it->second;

    const auto symbol = required_field(message, 55, error);
    const auto quantity_text = required_field(message, 38, error);
    const auto price_text = required_field(message, 44, error);

    if (
        !symbol.has_value() ||
        !quantity_text.has_value() ||
        !price_text.has_value()
    ) {
        return send_cancel_reject(
            session,
            config,
            &order,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Replace,
            error
        );
    }

    if (
        *symbol != protocol::symbol_to_string(order.symbol)
    ) {
        return send_cancel_reject(
            session,
            config,
            &order,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Replace,
            "Symbol cannot change on replace"
        );
    }

    if (order.order_type != protocol::OrderType::Limit) {
        return send_cancel_reject(
            session,
            config,
            &order,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Replace,
            "only working limit orders can be replaced"
        );
    }

    const auto total_quantity = fix::parse_u64(*quantity_text);
    const auto new_price = fix::parse_i64(*price_text);

    if (
        !total_quantity.has_value() ||
        *total_quantity == 0 ||
        *total_quantity > std::numeric_limits<std::uint32_t>::max()
    ) {
        return send_cancel_reject(
            session,
            config,
            &order,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Replace,
            "invalid OrderQty"
        );
    }

    if (!new_price.has_value() || *new_price <= 0) {
        return send_cancel_reject(
            session,
            config,
            &order,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Replace,
            "invalid Price"
        );
    }

    if (*total_quantity <= order.cumulative_quantity) {
        return send_cancel_reject(
            session,
            config,
            &order,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Replace,
            "replacement OrderQty must exceed CumQty"
        );
    }

    if (const auto side = message.get(54); side.has_value()) {
        const auto translated_side = fixbridge::translate_side(*side);
        if (
            !translated_side.has_value() ||
            *translated_side != order.side
        ) {
            return send_cancel_reject(
                session,
                config,
                &order,
                *cl_ord_id,
                *orig_cl_ord_id,
                PendingKind::Replace,
                "Side cannot change on replace"
            );
        }
    }

    const std::uint64_t working_quantity =
        *total_quantity - order.cumulative_quantity;

    const protocol::ReplaceOrderRequest request {
        .order_id = order.numeric_order_id,
        .timestamp = now_nanoseconds(),
        .new_price = *new_price,
        .new_quantity = working_quantity,
        .symbol = order.symbol
    };

    session.pending[order.numeric_order_id].push_back(
        PendingRequest {
            .kind = PendingKind::Replace,
            .cl_ord_id = std::string(*cl_ord_id),
            .orig_cl_ord_id = std::string(*orig_cl_ord_id),
            .replacement_total_quantity = *total_quantity,
            .replacement_working_quantity = working_quantity,
            .replacement_price = *new_price
        }
    );

    if (
        !send_core_request(
            session,
            protocol::MessageType::ReplaceOrder,
            protocol::encode_replace_order(request)
        )
    ) {
        session.pending[order.numeric_order_id].pop_back();

        return send_cancel_reject(
            session,
            config,
            &order,
            *cl_ord_id,
            *orig_cl_ord_id,
            PendingKind::Replace,
            "core exchange connection failed"
        );
    }

    return true;
}

[[nodiscard]] bool handle_core_order_response(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::atomic<std::uint64_t>& next_exec_id,
    const CoreMessage& message
) {
    const auto response = protocol::decode_order_response(
        message.body
    );

    if (!response.has_value()) {
        return true;
    }

    auto pending_it = session.pending.find(response->order_id);
    if (
        pending_it == session.pending.end() ||
        pending_it->second.empty()
    ) {
        return true;
    }

    PendingRequest pending = std::move(
        pending_it->second.front()
    );
    pending_it->second.pop_front();

    if (pending_it->second.empty()) {
        session.pending.erase(pending_it);
    }

    auto order_it = session.orders.find(response->order_id);
    if (order_it == session.orders.end()) {
        return true;
    }

    OrderState& order = order_it->second;
    const bool success =
        response->success != 0 &&
        message.header.type != protocol::MessageType::OrderRejected;

    if (pending.kind == PendingKind::New) {
        if (success) {
            order.ord_status = '0';

            if (
                order.order_type == protocol::OrderType::Market ||
                order.time_in_force !=
                    protocol::TimeInForce::GoodTillCancel
            ) {
                session.non_resting_awaiting_boundary.insert(
                    order.numeric_order_id
                );
            }

            return send_execution_report(
                session,
                config,
                next_exec_id,
                order,
                pending.cl_ord_id,
                "0",
                order.ord_status
            );
        }

        order.leaves_quantity = 0;
        order.ord_status = '8';

        const bool sent = send_execution_report(
            session,
            config,
            next_exec_id,
            order,
            pending.cl_ord_id,
            "8",
            '8',
            std::nullopt,
            std::nullopt,
            std::nullopt,
            "rejected by ExchangeLab core or risk engine"
        );

        session.clord_to_order.erase(order.current_cl_ord_id);
        session.orders.erase(order_it);
        return sent;
    }

    if (pending.kind == PendingKind::Cancel) {
        if (!success) {
            return send_cancel_reject(
                session,
                config,
                &order,
                pending.cl_ord_id,
                pending.orig_cl_ord_id,
                PendingKind::Cancel,
                "cancel rejected by ExchangeLab core"
            );
        }

        order.leaves_quantity = 0;
        order.ord_status = '4';

        return send_execution_report(
            session,
            config,
            next_exec_id,
            order,
            pending.cl_ord_id,
            "4",
            '4',
            pending.orig_cl_ord_id
        );
    }

    if (!success) {
        return send_cancel_reject(
            session,
            config,
            &order,
            pending.cl_ord_id,
            pending.orig_cl_ord_id,
            PendingKind::Replace,
            "replace rejected by ExchangeLab core or risk engine"
        );
    }

    order.current_cl_ord_id = pending.cl_ord_id;
    order.order_quantity = pending.replacement_total_quantity;
    order.leaves_quantity = pending.replacement_working_quantity;
    order.price = pending.replacement_price;
    order.ord_status =
        order.cumulative_quantity == 0 ? '0' : '1';

    session.clord_to_order.emplace(
        pending.cl_ord_id,
        order.numeric_order_id
    );

    return send_execution_report(
        session,
        config,
        next_exec_id,
        order,
        pending.cl_ord_id,
        "5",
        order.ord_status,
        pending.orig_cl_ord_id
    );
}

[[nodiscard]] std::string self_trade_key(
    const protocol::TradeExecution& execution
) {
    return
        std::to_string(execution.buy_order_id) + ":" +
        std::to_string(execution.sell_order_id) + ":" +
        std::to_string(execution.price) + ":" +
        std::to_string(execution.quantity);
}

[[nodiscard]] bool apply_trade_to_order(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::atomic<std::uint64_t>& next_exec_id,
    OrderState& order,
    const protocol::TradeExecution& execution
) {
    const std::uint64_t quantity = std::min(
        order.leaves_quantity,
        execution.quantity
    );

    order.cumulative_quantity += quantity;
    order.leaves_quantity -= quantity;
    order.cumulative_notional +=
        static_cast<long double>(execution.price) *
        static_cast<long double>(quantity);

    order.ord_status =
        order.leaves_quantity == 0 ? '2' : '1';

    return send_execution_report(
        session,
        config,
        next_exec_id,
        order,
        order.current_cl_ord_id,
        "F",
        order.ord_status,
        std::nullopt,
        quantity,
        execution.price
    );
}

[[nodiscard]] bool handle_core_trade(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::atomic<std::uint64_t>& next_exec_id,
    const CoreMessage& message
) {
    const auto execution = protocol::decode_trade_execution(
        message.body
    );

    if (!execution.has_value()) {
        return true;
    }

    auto buy_it = session.orders.find(execution->buy_order_id);
    auto sell_it = session.orders.find(execution->sell_order_id);

    const bool owns_buy = buy_it != session.orders.end();
    const bool owns_sell = sell_it != session.orders.end();

    if (!owns_buy && !owns_sell) {
        return true;
    }

    if (owns_buy && owns_sell) {
        const std::string key = self_trade_key(*execution);

        if (
            session.self_trade_duplicate_messages_to_ignore > 0 &&
            key == session.last_self_trade_key
        ) {
            --session.self_trade_duplicate_messages_to_ignore;
            return true;
        }

        if (
            !apply_trade_to_order(
                session,
                config,
                next_exec_id,
                buy_it->second,
                *execution
            )
        ) {
            return false;
        }

        if (
            !apply_trade_to_order(
                session,
                config,
                next_exec_id,
                sell_it->second,
                *execution
            )
        ) {
            return false;
        }

        session.last_self_trade_key = key;
        session.self_trade_duplicate_messages_to_ignore = 1;
        return true;
    }

    OrderState& order = owns_buy
        ? buy_it->second
        : sell_it->second;

    return apply_trade_to_order(
        session,
        config,
        next_exec_id,
        order,
        *execution
    );
}

[[nodiscard]] bool finalize_non_resting_orders(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::atomic<std::uint64_t>& next_exec_id,
    const CoreMessage& message
) {
    const auto update = protocol::decode_book_update(message.body);
    if (!update.has_value()) {
        return true;
    }

    std::vector<std::uint64_t> completed_boundaries;

    for (const std::uint64_t order_id : session.non_resting_awaiting_boundary) {
        auto order_it = session.orders.find(order_id);
        if (order_it == session.orders.end()) {
            completed_boundaries.push_back(order_id);
            continue;
        }

        OrderState& order = order_it->second;
        if (order.symbol != update->symbol) {
            continue;
        }

        if (order.leaves_quantity > 0) {
            order.leaves_quantity = 0;
            order.ord_status = '4';

            if (
                !send_execution_report(
                    session,
                    config,
                    next_exec_id,
                    order,
                    order.current_cl_ord_id,
                    "4",
                    '4',
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    "non-resting remainder canceled"
                )
            ) {
                return false;
            }
        }

        completed_boundaries.push_back(order_id);
    }

    for (const std::uint64_t order_id : completed_boundaries) {
        session.non_resting_awaiting_boundary.erase(order_id);
    }

    return true;
}

[[nodiscard]] bool process_core_message(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::atomic<std::uint64_t>& next_exec_id,
    const CoreMessage& message
) {
    switch (message.header.type) {
        case protocol::MessageType::OrderAccepted:
        case protocol::MessageType::OrderRejected:
        case protocol::MessageType::OrderCancelled:
        case protocol::MessageType::OrderReplaced:
            return handle_core_order_response(
                session,
                config,
                next_exec_id,
                message
            );

        case protocol::MessageType::TradeExecution:
            return handle_core_trade(
                session,
                config,
                next_exec_id,
                message
            );

        /* BookUpdate is also the lifecycle boundary for IOC/market
         * orders: the core sends response -> trades -> book update. */
        case protocol::MessageType::BookUpdate:
            return finalize_non_resting_orders(
                session,
                config,
                next_exec_id,
                message
            );

        /* Other public market data is intentionally ignored by the FIX
         * order-entry bridge. Clients use multicast/WebSocket for it. */
        case protocol::MessageType::Level3AddOrder:
        case protocol::MessageType::Level3OrderExecuted:
        case protocol::MessageType::Level3OrderDeleted:
            return true;

        default:
            return true;
    }
}

[[nodiscard]] bool process_fix_message(
    SessionContext& session,
    const FixGatewayConfig& config,
    std::atomic<std::uint64_t>& next_numeric_order_id,
    std::atomic<std::uint64_t>& next_exec_id,
    const fix::Message& message
) {
    const auto msg_type = message.get(35);
    const auto seq_num = sequence_number(message);

    if (!msg_type.has_value() || !seq_num.has_value()) {
        return send_session_reject(
            session,
            config,
            session.expected_inbound_sequence,
            "MsgType (35) and MsgSeqNum (34) are required"
        );
    }

    if (*seq_num != session.expected_inbound_sequence) {
        return send_session_reject(
            session,
            config,
            *seq_num,
            "unexpected MsgSeqNum; expected " +
                std::to_string(session.expected_inbound_sequence),
            *msg_type
        );
    }

    ++session.expected_inbound_sequence;

    std::string comp_error;
    if (!validate_comp_ids(message, session, config, comp_error)) {
        return send_session_reject(
            session,
            config,
            *seq_num,
            comp_error,
            *msg_type
        );
    }

    if (*msg_type == "A") {
        if (session.logged_on) {
            return send_session_reject(
                session,
                config,
                *seq_num,
                "duplicate Logon",
                *msg_type
            );
        }

        const auto sender = message.get(49);
        const auto encrypt_method = message.get(98);
        const auto heartbeat = message.get(108);

        if (
            !sender.has_value() ||
            !encrypt_method.has_value() ||
            *encrypt_method != "0" ||
            !heartbeat.has_value()
        ) {
            return send_session_reject(
                session,
                config,
                *seq_num,
                "Logon requires 98=0 and HeartBtInt (108)",
                *msg_type
            );
        }

        const auto heartbeat_seconds = fix::parse_u64(*heartbeat);
        if (
            !heartbeat_seconds.has_value() ||
            *heartbeat_seconds == 0 ||
            *heartbeat_seconds > 3600
        ) {
            return send_session_reject(
                session,
                config,
                *seq_num,
                "invalid HeartBtInt",
                *msg_type
            );
        }

        session.client_comp_id = std::string(*sender);
        session.heartbeat_seconds = static_cast<std::uint32_t>(
            *heartbeat_seconds
        );

        session.core_socket = connect_to_exchange(
            config.exchange_host,
            config.exchange_port
        );

        if (session.core_socket < 0) {
            session.closing = true;
            return send_session_message(
                session,
                config,
                "5",
                {{58, "ExchangeLab core is unavailable"}}
            );
        }

        session.logged_on = true;

        return send_session_message(
            session,
            config,
            "A",
            {
                {98, "0"},
                {108, std::to_string(session.heartbeat_seconds)},
                {141, "N"}
            }
        );
    }

    if (!session.logged_on) {
        return send_session_reject(
            session,
            config,
            *seq_num,
            "Logon is required before application messages",
            *msg_type
        );
    }

    if (*msg_type == "0") {
        return true;
    }

    if (*msg_type == "1") {
        const auto test_request_id = message.get(112);
        if (!test_request_id.has_value()) {
            return send_session_reject(
                session,
                config,
                *seq_num,
                "TestRequest requires tag 112",
                *msg_type
            );
        }

        return send_session_message(
            session,
            config,
            "0",
            {{112, std::string(*test_request_id)}}
        );
    }

    if (*msg_type == "5") {
        session.closing = true;
        return send_session_message(
            session,
            config,
            "5"
        );
    }

    if (*msg_type == "D") {
        return handle_new_order(
            session,
            config,
            next_numeric_order_id,
            next_exec_id,
            message
        );
    }

    if (*msg_type == "F") {
        return handle_cancel(session, config, message);
    }

    if (*msg_type == "G") {
        return handle_replace(session, config, message);
    }

    return send_session_reject(
        session,
        config,
        *seq_num,
        "unsupported MsgType",
        *msg_type
    );
}

}  // namespace

namespace fixbridge {

std::optional<protocol::Side> translate_side(
    std::string_view fix_value
) noexcept {
    if (fix_value == "1") {
        return protocol::Side::Buy;
    }

    if (fix_value == "2") {
        return protocol::Side::Sell;
    }

    return std::nullopt;
}

std::optional<protocol::OrderType> translate_order_type(
    std::string_view fix_value
) noexcept {
    if (fix_value == "1") {
        return protocol::OrderType::Market;
    }

    if (fix_value == "2") {
        return protocol::OrderType::Limit;
    }

    return std::nullopt;
}

std::optional<protocol::TimeInForce> translate_time_in_force(
    std::string_view fix_value
) noexcept {
    /* FIX values: 1=GTC, 3=IOC, 4=FOK. */
    if (fix_value == "1") {
        return protocol::TimeInForce::GoodTillCancel;
    }

    if (fix_value == "3") {
        return protocol::TimeInForce::ImmediateOrCancel;
    }

    if (fix_value == "4") {
        return protocol::TimeInForce::FillOrKill;
    }

    return std::nullopt;
}

std::optional<NewOrderTranslation> translate_new_order_single(
    const fix::Message& message,
    std::uint64_t numeric_order_id,
    std::uint64_t timestamp,
    std::string& error
) {
    const auto cl_ord_id = required_field(message, 11, error);
    const auto symbol_text = required_field(message, 55, error);
    const auto side_text_value = required_field(message, 54, error);
    const auto quantity_text = required_field(message, 38, error);
    const auto order_type_text = required_field(message, 40, error);

    if (
        !cl_ord_id.has_value() ||
        !symbol_text.has_value() ||
        !side_text_value.has_value() ||
        !quantity_text.has_value() ||
        !order_type_text.has_value()
    ) {
        return std::nullopt;
    }

    const protocol::Symbol symbol = protocol::make_symbol(*symbol_text);
    if (!protocol::is_valid_symbol(symbol)) {
        error = "invalid Symbol; use 1-8 uppercase A-Z/0-9/./- characters";
        return std::nullopt;
    }

    const auto side = translate_side(*side_text_value);
    if (!side.has_value()) {
        error = "unsupported Side; use 54=1 (Buy) or 54=2 (Sell)";
        return std::nullopt;
    }

    const auto order_type = translate_order_type(*order_type_text);
    if (!order_type.has_value()) {
        error = "unsupported OrdType; use 40=1 (Market) or 40=2 (Limit)";
        return std::nullopt;
    }

    const auto quantity = fix::parse_u64(*quantity_text);
    if (
        !quantity.has_value() ||
        *quantity == 0 ||
        *quantity > std::numeric_limits<std::uint32_t>::max()
    ) {
        error = "invalid OrderQty";
        return std::nullopt;
    }

    std::int64_t price = 0;

    if (*order_type == protocol::OrderType::Limit) {
        const auto price_text = required_field(message, 44, error);
        if (!price_text.has_value()) {
            return std::nullopt;
        }

        const auto parsed_price = fix::parse_i64(*price_text);
        if (!parsed_price.has_value() || *parsed_price <= 0) {
            error = "Price must be a positive integer tick value";
            return std::nullopt;
        }

        price = *parsed_price;
    }

    protocol::TimeInForce time_in_force =
        *order_type == protocol::OrderType::Market
            ? protocol::TimeInForce::ImmediateOrCancel
            : protocol::TimeInForce::GoodTillCancel;

    if (const auto tif = message.get(59); tif.has_value()) {
        const auto translated_tif = translate_time_in_force(*tif);
        if (!translated_tif.has_value()) {
            error = "unsupported TimeInForce; use 59=1 (GTC), 3 (IOC), or 4 (FOK)";
            return std::nullopt;
        }

        time_in_force = *translated_tif;
    }

    if (
        *order_type == protocol::OrderType::Market &&
        time_in_force == protocol::TimeInForce::GoodTillCancel
    ) {
        time_in_force = protocol::TimeInForce::ImmediateOrCancel;
    }

    return NewOrderTranslation {
        .request = protocol::NewOrderRequest {
            .order_id = numeric_order_id,
            .timestamp = timestamp,
            .price = price,
            .quantity = *quantity,
            .side = *side,
            .order_type = *order_type,
            .time_in_force = time_in_force,
            .symbol = symbol
        },
        .cl_ord_id = std::string(*cl_ord_id)
    };
}

}  // namespace fixbridge

FixGateway::FixGateway(FixGatewayConfig config)
    : config_(std::move(config)) {}

FixGateway::~FixGateway() {
    stop();
}

bool FixGateway::start() {
    if (running_.exchange(true)) {
        return true;
    }

    listen_socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_ < 0) {
        running_.store(false);
        return false;
    }

    int reuse = 1;
    ::setsockopt(
        listen_socket_,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse)
    );

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(config_.listen_port);

    if (
        ::bind(
            listen_socket_,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) != 0 ||
        ::listen(listen_socket_, 64) != 0
    ) {
        ::close(listen_socket_);
        listen_socket_ = -1;
        running_.store(false);
        return false;
    }

    return true;
}

void FixGateway::run() {
    if (!running_.load() && !start()) {
        return;
    }

    while (running_.load()) {
        sockaddr_in client_address {};
        socklen_t client_length = sizeof(client_address);

        const int client_socket = ::accept(
            listen_socket_,
            reinterpret_cast<sockaddr*>(&client_address),
            &client_length
        );

        if (client_socket < 0) {
            if (!running_.load()) {
                break;
            }

            continue;
        }

        std::lock_guard<std::mutex> lock(threads_mutex_);
        client_threads_.emplace_back(
            &FixGateway::handle_client,
            this,
            client_socket
        );
    }
}

void FixGateway::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (listen_socket_ >= 0) {
        ::shutdown(listen_socket_, SHUT_RDWR);
        ::close(listen_socket_);
        listen_socket_ = -1;
    }

    std::vector<std::thread> threads;

    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        threads.swap(client_threads_);
    }

    for (std::thread& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

const FixGatewayConfig& FixGateway::config() const noexcept {
    return config_;
}

void FixGateway::handle_client(int client_socket) {
    SessionContext session;
    session.client_socket = client_socket;
    session.heartbeat_seconds = config_.default_heartbeat_seconds;
    session.fix_receive_buffer.reserve(4096);
    session.core_receive_buffer.reserve(4096);

    bool healthy = true;

    while (
        healthy &&
        !session.closing &&
        running_.load()
    ) {
        pollfd descriptors[2] {};
        descriptors[0].fd = session.client_socket;
        descriptors[0].events = POLLIN;

        int descriptor_count = 1;

        if (session.core_socket >= 0) {
            descriptors[1].fd = session.core_socket;
            descriptors[1].events = POLLIN;
            descriptor_count = 2;
        }

        const int timeout_ms = 250;
        const int poll_result = ::poll(
            descriptors,
            static_cast<nfds_t>(descriptor_count),
            timeout_ms
        );

        if (poll_result < 0) {
            break;
        }

        if (
            descriptors[0].revents &
            (POLLERR | POLLHUP | POLLNVAL)
        ) {
            break;
        }

        if (descriptors[0].revents & POLLIN) {
            std::array<char, 4096> bytes {};
            const auto received = ::recv(
                session.client_socket,
                bytes.data(),
                bytes.size(),
                0
            );

            if (received <= 0) {
                break;
            }

            session.last_fix_receive = std::chrono::steady_clock::now();
            session.fix_receive_buffer.append(
                bytes.data(),
                static_cast<std::size_t>(received)
            );

            while (healthy) {
                auto extracted = fix::extract_one(
                    session.fix_receive_buffer
                );

                if (
                    extracted.status ==
                    fix::ExtractStatus::NeedMoreData
                ) {
                    break;
                }

                if (
                    extracted.status ==
                    fix::ExtractStatus::InvalidData
                ) {
                    healthy = false;
                    break;
                }

                const auto parsed = fix::parse(
                    extracted.wire_message
                );

                if (!parsed) {
                    healthy = false;
                    break;
                }

                healthy = process_fix_message(
                    session,
                    config_,
                    next_numeric_order_id_,
                    next_exec_id_,
                    *parsed.message
                );

                if (session.closing) {
                    break;
                }
            }
        }

        if (
            descriptor_count == 2 &&
            descriptors[1].revents &
                (POLLERR | POLLHUP | POLLNVAL)
        ) {
            (void)send_session_message(
                session,
                config_,
                "5",
                {{58, "ExchangeLab core connection closed"}}
            );
            break;
        }

        if (
            descriptor_count == 2 &&
            descriptors[1].revents & POLLIN
        ) {
            std::array<std::byte, 4096> bytes {};
            const auto received = ::recv(
                session.core_socket,
                bytes.data(),
                bytes.size(),
                0
            );

            if (received <= 0) {
                break;
            }

            session.core_receive_buffer.insert(
                session.core_receive_buffer.end(),
                bytes.begin(),
                bytes.begin() + received
            );

            CoreMessage core_message;

            while (
                extract_core_message(
                    session.core_receive_buffer,
                    core_message
                )
            ) {
                healthy = process_core_message(
                    session,
                    config_,
                    next_exec_id_,
                    core_message
                );

                if (!healthy) {
                    break;
                }
            }
        }

        if (session.logged_on && healthy) {
            const auto now = std::chrono::steady_clock::now();
            const auto heartbeat = std::chrono::seconds(
                session.heartbeat_seconds
            );

            if (now - session.last_fix_send >= heartbeat) {
                healthy = send_session_message(
                    session,
                    config_,
                    "0"
                );
            }

            /* A simple dead-session timeout. Full FIX resend/gap handling is
             * intentionally outside this milestone. */
            if (
                now - session.last_fix_receive >=
                heartbeat * 3
            ) {
                (void)send_session_message(
                    session,
                    config_,
                    "5",
                    {{58, "heartbeat timeout"}}
                );
                break;
            }
        }
    }

    if (session.core_socket >= 0) {
        ::shutdown(session.core_socket, SHUT_RDWR);
        ::close(session.core_socket);
    }

    ::shutdown(session.client_socket, SHUT_RDWR);
    ::close(session.client_socket);
}

}  // namespace exchange