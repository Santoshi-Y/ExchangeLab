#include "exchange/fix.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

namespace exchange::fix {
namespace {

[[nodiscard]] std::optional<std::size_t> parse_size(
    std::string_view text
) noexcept {
    std::size_t value = 0;

    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );

    if (
        result.ec != std::errc {} ||
        result.ptr != text.data() + text.size()
    ) {
        return std::nullopt;
    }

    return value;
}

[[nodiscard]] std::optional<int> parse_tag(
    std::string_view text
) noexcept {
    int value = 0;

    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );

    if (
        result.ec != std::errc {} ||
        result.ptr != text.data() + text.size() ||
        value <= 0
    ) {
        return std::nullopt;
    }

    return value;
}

[[nodiscard]] unsigned int checksum_before(
    std::string_view wire,
    std::size_t checksum_offset
) noexcept {
    unsigned int sum = 0;

    for (std::size_t index = 0; index < checksum_offset; ++index) {
        sum += static_cast<unsigned char>(wire[index]);
    }

    return sum % 256U;
}

[[nodiscard]] std::string three_digit_checksum(
    unsigned int value
) {
    std::array<char, 4> buffer {};
    buffer[0] = static_cast<char>('0' + ((value / 100U) % 10U));
    buffer[1] = static_cast<char>('0' + ((value / 10U) % 10U));
    buffer[2] = static_cast<char>('0' + (value % 10U));
    return std::string(buffer.data(), 3);
}

[[nodiscard]] bool starts_with_fix44(
    std::string_view wire
) noexcept {
    constexpr std::string_view prefix = "8=FIX.4.4\x01";
    return wire.starts_with(prefix);
}

}  // namespace

void Message::add(int tag, std::string value) {
    fields_.push_back(Field {
        .tag = tag,
        .value = std::move(value)
    });
}

void Message::set(int tag, std::string value) {
    for (Field& field : fields_) {
        if (field.tag == tag) {
            field.value = std::move(value);
            return;
        }
    }

    add(tag, std::move(value));
}

std::optional<std::string_view> Message::get(
    int tag
) const noexcept {
    for (const Field& field : fields_) {
        if (field.tag == tag) {
            return field.value;
        }
    }

    return std::nullopt;
}

bool Message::contains(int tag) const noexcept {
    return get(tag).has_value();
}

const std::vector<Field>& Message::fields() const noexcept {
    return fields_;
}

std::string encode(const Message& message) {
    std::string body;

    for (const Field& field : message.fields()) {
        if (field.tag == 8 || field.tag == 9 || field.tag == 10) {
            continue;
        }

        body += std::to_string(field.tag);
        body.push_back('=');
        body += field.value;
        body.push_back(soh);
    }

    std::string wire;
    wire.reserve(body.size() + 40);
    wire += "8=FIX.4.4";
    wire.push_back(soh);
    wire += "9=";
    wire += std::to_string(body.size());
    wire.push_back(soh);
    wire += body;

    const unsigned int checksum = checksum_before(
        wire,
        wire.size()
    );

    wire += "10=";
    wire += three_digit_checksum(checksum);
    wire.push_back(soh);

    return wire;
}

ParseResult parse(std::string_view wire_message) {
    ParseResult result;

    if (!starts_with_fix44(wire_message)) {
        result.error = "BeginString must be FIX.4.4";
        return result;
    }

    const std::size_t begin_end = wire_message.find(soh);
    if (begin_end == std::string_view::npos) {
        result.error = "truncated BeginString";
        return result;
    }

    const std::size_t body_length_start = begin_end + 1;
    if (!wire_message.substr(body_length_start).starts_with("9=")) {
        result.error = "BodyLength must be the second field";
        return result;
    }

    const std::size_t body_length_end = wire_message.find(
        soh,
        body_length_start
    );

    if (body_length_end == std::string_view::npos) {
        result.error = "truncated BodyLength";
        return result;
    }

    const std::string_view body_length_text = wire_message.substr(
        body_length_start + 2,
        body_length_end - (body_length_start + 2)
    );

    const auto body_length = parse_size(body_length_text);
    if (!body_length.has_value()) {
        result.error = "invalid BodyLength";
        return result;
    }

    const std::size_t body_start = body_length_end + 1;
    if (*body_length > wire_message.size() - body_start) {
        result.error = "BodyLength exceeds message size";
        return result;
    }

    const std::size_t checksum_offset = body_start + *body_length;

    if (
        checksum_offset + 7 != wire_message.size() ||
        !wire_message.substr(checksum_offset).starts_with("10=") ||
        wire_message.back() != soh
    ) {
        result.error = "BodyLength does not end at CheckSum";
        return result;
    }

    const std::string_view checksum_text = wire_message.substr(
        checksum_offset + 3,
        3
    );

    const auto checksum_value = parse_size(checksum_text);
    if (
        !checksum_value.has_value() ||
        *checksum_value > 255 ||
        wire_message[checksum_offset + 6] != soh
    ) {
        result.error = "invalid CheckSum field";
        return result;
    }

    if (
        checksum_before(wire_message, checksum_offset) !=
        *checksum_value
    ) {
        result.error = "CheckSum mismatch";
        return result;
    }

    Message parsed;
    std::size_t cursor = body_start;

    while (cursor < checksum_offset) {
        const std::size_t field_end = wire_message.find(soh, cursor);

        if (
            field_end == std::string_view::npos ||
            field_end >= checksum_offset
        ) {
            result.error = "truncated FIX field";
            return result;
        }

        const std::string_view field = wire_message.substr(
            cursor,
            field_end - cursor
        );

        const std::size_t equals = field.find('=');
        if (equals == std::string_view::npos || equals == 0) {
            result.error = "invalid FIX field";
            return result;
        }

        const auto tag = parse_tag(field.substr(0, equals));
        if (!tag.has_value()) {
            result.error = "invalid FIX tag";
            return result;
        }

        parsed.add(
            *tag,
            std::string(field.substr(equals + 1))
        );

        cursor = field_end + 1;
    }

    const auto msg_type = parsed.get(35);
    if (!msg_type.has_value() || msg_type->empty()) {
        result.error = "MsgType (35) is required";
        return result;
    }

    result.message = std::move(parsed);
    return result;
}

ExtractResult extract_one(std::string& receive_buffer) {
    ExtractResult result;

    if (receive_buffer.empty()) {
        return result;
    }

    constexpr std::string_view prefix = "8=FIX.4.4\x01";

    if (!std::string_view(receive_buffer).starts_with(prefix)) {
        const std::size_t next = receive_buffer.find("8=FIX.4.4\x01");

        if (next == std::string::npos) {
            if (receive_buffer.size() > prefix.size()) {
                result.status = ExtractStatus::InvalidData;
                result.error = "stream does not begin with FIX.4.4";
            }
            return result;
        }

        receive_buffer.erase(0, next);
    }

    const std::size_t begin_end = receive_buffer.find(soh);
    if (begin_end == std::string::npos) {
        return result;
    }

    const std::size_t body_length_start = begin_end + 1;
    if (receive_buffer.size() < body_length_start + 2) {
        return result;
    }

    if (receive_buffer.compare(body_length_start, 2, "9=") != 0) {
        result.status = ExtractStatus::InvalidData;
        result.error = "BodyLength must be the second field";
        return result;
    }

    const std::size_t body_length_end = receive_buffer.find(
        soh,
        body_length_start
    );

    if (body_length_end == std::string::npos) {
        return result;
    }

    const auto body_length = parse_size(
        std::string_view(receive_buffer).substr(
            body_length_start + 2,
            body_length_end - (body_length_start + 2)
        )
    );

    if (!body_length.has_value()) {
        result.status = ExtractStatus::InvalidData;
        result.error = "invalid BodyLength";
        return result;
    }

    const std::size_t body_start = body_length_end + 1;

    if (
        *body_length >
        std::numeric_limits<std::size_t>::max() - body_start - 7
    ) {
        result.status = ExtractStatus::InvalidData;
        result.error = "BodyLength overflow";
        return result;
    }

    const std::size_t total_size = body_start + *body_length + 7;

    if (receive_buffer.size() < total_size) {
        return result;
    }

    const std::size_t checksum_offset = body_start + *body_length;
    if (receive_buffer.compare(checksum_offset, 3, "10=") != 0) {
        result.status = ExtractStatus::InvalidData;
        result.error = "BodyLength does not point to CheckSum";
        return result;
    }

    result.wire_message = receive_buffer.substr(0, total_size);
    receive_buffer.erase(0, total_size);
    result.status = ExtractStatus::MessageReady;
    return result;
}

std::optional<std::uint64_t> parse_u64(
    std::string_view text
) noexcept {
    std::uint64_t value = 0;

    if (text.empty()) {
        return std::nullopt;
    }

    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );

    if (
        parsed.ec != std::errc {} ||
        parsed.ptr != text.data() + text.size()
    ) {
        return std::nullopt;
    }

    return value;
}

std::optional<std::int64_t> parse_i64(
    std::string_view text
) noexcept {
    std::int64_t value = 0;

    if (text.empty()) {
        return std::nullopt;
    }

    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );

    if (
        parsed.ec != std::errc {} ||
        parsed.ptr != text.data() + text.size()
    ) {
        return std::nullopt;
    }

    return value;
}

std::string utc_timestamp() {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto milliseconds_part = duration_cast<milliseconds>(
        now.time_since_epoch()
    ) % 1000;

    const std::time_t time = system_clock::to_time_t(now);
    std::tm utc {};

#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif

    std::ostringstream output;
    output
        << std::put_time(&utc, "%Y%m%d-%H:%M:%S")
        << '.'
        << std::setfill('0')
        << std::setw(3)
        << milliseconds_part.count();

    return output.str();
}

std::string printable(std::string_view wire_message) {
    std::string output(wire_message);
    std::replace(output.begin(), output.end(), soh, '|');
    return output;
}

}  // namespace exchange::fix