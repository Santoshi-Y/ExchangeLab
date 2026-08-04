#include "exchange/journal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace exchange {

namespace {

constexpr std::size_t record_length_size =
    sizeof(std::uint32_t);

std::array<std::byte, record_length_size>
encode_record_length(std::uint32_t length) {
    std::array<
        std::byte,
        record_length_size
    > bytes {};

    for (
        std::size_t index = 0;
        index < record_length_size;
        ++index
    ) {
        bytes[index] = static_cast<std::byte>(
            (length >> (index * 8U)) & 0xFFU
        );
    }

    return bytes;
}

}  // namespace

ExchangeJournal::ExchangeJournal(
    const std::filesystem::path& path,
    bool truncate
) {
    const std::filesystem::path parent =
        path.parent_path();

    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ios::openmode mode =
        std::ios::binary |
        std::ios::out;

    if (truncate) {
        mode |= std::ios::trunc;
    } else {
        mode |= std::ios::app;
    }

    output_.open(path, mode);

    if (!output_.is_open()) {
        throw std::runtime_error(
            "Could not open exchange journal"
        );
    }
}

ExchangeJournal::~ExchangeJournal() {
    try {
        flush();
    } catch (...) {
    }
}

bool ExchangeJournal::is_open() const noexcept {
    return output_.is_open();
}

void ExchangeJournal::append(
    std::span<const std::byte> message
) {
    if (
        message.size() >
        std::numeric_limits<std::uint32_t>::max()
    ) {
        throw std::length_error(
            "Journal message is too large"
        );
    }

    const auto encoded_length =
        encode_record_length(
            static_cast<std::uint32_t>(
                message.size()
            )
        );

    std::lock_guard<std::mutex> lock(mutex_);

    output_.write(
        reinterpret_cast<const char*>(
            encoded_length.data()
        ),
        static_cast<std::streamsize>(
            encoded_length.size()
        )
    );

    if (!message.empty()) {
        output_.write(
            reinterpret_cast<const char*>(
                message.data()
            ),
            static_cast<std::streamsize>(
                message.size()
            )
        );
    }

    // Make the completed record visible to replay immediately.
    output_.flush();

    if (!output_) {
        throw std::runtime_error(
            "Failed to write and flush journal record"
        );
    }
}

void ExchangeJournal::flush() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!output_.is_open()) {
        return;
    }

    output_.flush();

    if (!output_) {
        throw std::runtime_error(
            "Failed to flush exchange journal"
        );
    }
}

}  // namespace exchange