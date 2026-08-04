#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>

namespace exchange {

class ExchangeJournal {
public:
    explicit ExchangeJournal(
        const std::filesystem::path& path,
        bool truncate = false
    );

    ~ExchangeJournal();

    ExchangeJournal(const ExchangeJournal&) = delete;
    ExchangeJournal& operator=(
        const ExchangeJournal&
    ) = delete;

    ExchangeJournal(ExchangeJournal&&) = delete;
    ExchangeJournal& operator=(
        ExchangeJournal&&
    ) = delete;

    [[nodiscard]] bool is_open() const noexcept;

    void append(
        std::span<const std::byte> message
    );

    void flush();

private:
    std::ofstream output_;
    std::mutex mutex_;
};

}  // namespace exchange