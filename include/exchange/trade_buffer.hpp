#pragma once

#include <array>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

#include "exchange/trade.hpp"

namespace exchange {

template <std::size_t InlineCapacity = 8>
class TradeBuffer {
public:
    static_assert(
        InlineCapacity > 0,
        "TradeBuffer inline capacity must be positive"
    );

    class const_iterator {
    public:
        using iterator_category =
            std::random_access_iterator_tag;
        using value_type = Trade;
        using difference_type = std::ptrdiff_t;
        using pointer = const Trade*;
        using reference = const Trade&;

        const_iterator() = default;

        const_iterator(
            const TradeBuffer* buffer,
            std::size_t index
        )
            : buffer_(buffer),
              index_(index) {}

        [[nodiscard]] reference operator*() const {
            return (*buffer_)[index_];
        }

        [[nodiscard]] pointer operator->() const {
            return &(*buffer_)[index_];
        }

        const_iterator& operator++() {
            ++index_;
            return *this;
        }

        const_iterator operator++(int) {
            const_iterator copy = *this;
            ++(*this);
            return copy;
        }

        const_iterator& operator--() {
            --index_;
            return *this;
        }

        const_iterator operator--(int) {
            const_iterator copy = *this;
            --(*this);
            return copy;
        }

        const_iterator& operator+=(
            difference_type offset
        ) {
            index_ = static_cast<std::size_t>(
                static_cast<difference_type>(index_) +
                offset
            );

            return *this;
        }

        const_iterator& operator-=(
            difference_type offset
        ) {
            return *this += -offset;
        }

        [[nodiscard]] const_iterator operator+(
            difference_type offset
        ) const {
            const_iterator copy = *this;
            copy += offset;
            return copy;
        }

        [[nodiscard]] const_iterator operator-(
            difference_type offset
        ) const {
            const_iterator copy = *this;
            copy -= offset;
            return copy;
        }

        [[nodiscard]] difference_type operator-(
            const const_iterator& other
        ) const {
            return static_cast<difference_type>(index_) -
                   static_cast<difference_type>(
                       other.index_
                   );
        }

        [[nodiscard]] bool operator==(
            const const_iterator& other
        ) const {
            return buffer_ == other.buffer_ &&
                   index_ == other.index_;
        }

        [[nodiscard]] bool operator!=(
            const const_iterator& other
        ) const {
            return !(*this == other);
        }

        [[nodiscard]] bool operator<(
            const const_iterator& other
        ) const {
            return index_ < other.index_;
        }

        [[nodiscard]] bool operator<=(
            const const_iterator& other
        ) const {
            return index_ <= other.index_;
        }

        [[nodiscard]] bool operator>(
            const const_iterator& other
        ) const {
            return index_ > other.index_;
        }

        [[nodiscard]] bool operator>=(
            const const_iterator& other
        ) const {
            return index_ >= other.index_;
        }

    private:
        const TradeBuffer* buffer_ {nullptr};
        std::size_t index_ {0};
    };

    TradeBuffer() = default;

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        if (using_overflow_) {
            return overflow_.size();
        }

        return inline_size_;
    }

    [[nodiscard]] constexpr std::size_t
    inline_capacity() const noexcept {
        return InlineCapacity;
    }

    [[nodiscard]] bool using_overflow()
        const noexcept {
        return using_overflow_;
    }

    void clear() noexcept {
        inline_size_ = 0;

        if (using_overflow_) {
            overflow_.clear();
        }
    }

    void reserve(std::size_t capacity) {
        if (capacity <= InlineCapacity) {
            return;
        }

        move_inline_to_overflow(capacity);
    }

    void push_back(const Trade& trade) {
        emplace_back(trade);
    }

    void push_back(Trade&& trade) {
        emplace_back(std::move(trade));
    }

    template <typename... Arguments>
    Trade& emplace_back(Arguments&&... arguments) {
        if (
            !using_overflow_ &&
            inline_size_ < InlineCapacity
        ) {
            inline_[inline_size_] = Trade {
                std::forward<Arguments>(arguments)...
            };

            return inline_[inline_size_++];
        }

        if (!using_overflow_) {
            move_inline_to_overflow(
                InlineCapacity * 2
            );
        }

        overflow_.emplace_back(
            std::forward<Arguments>(arguments)...
        );

        return overflow_.back();
    }

    [[nodiscard]] Trade& operator[](
        std::size_t index
    ) {
        if (using_overflow_) {
            return overflow_[index];
        }

        return inline_[index];
    }

    [[nodiscard]] const Trade& operator[](
        std::size_t index
    ) const {
        if (using_overflow_) {
            return overflow_[index];
        }

        return inline_[index];
    }

    [[nodiscard]] Trade& at(std::size_t index) {
        if (index >= size()) {
            throw std::out_of_range(
                "TradeBuffer index is out of range"
            );
        }

        return (*this)[index];
    }

    [[nodiscard]] const Trade& at(
        std::size_t index
    ) const {
        if (index >= size()) {
            throw std::out_of_range(
                "TradeBuffer index is out of range"
            );
        }

        return (*this)[index];
    }

    [[nodiscard]] const_iterator begin() const {
        return const_iterator(this, 0);
    }

    [[nodiscard]] const_iterator end() const {
        return const_iterator(this, size());
    }

    [[nodiscard]] const_iterator begin() {
        return const_iterator(this, 0);
    }

    [[nodiscard]] const_iterator end() {
        return const_iterator(this, size());
    }

    [[nodiscard]] std::vector<Trade>
    to_vector() const {
        std::vector<Trade> result;
        result.reserve(size());

        for (const Trade& trade : *this) {
            result.push_back(trade);
        }

        return result;
    }

private:
    void move_inline_to_overflow(
        std::size_t requested_capacity
    ) {
        if (using_overflow_) {
            if (
                requested_capacity >
                overflow_.capacity()
            ) {
                overflow_.reserve(
                    requested_capacity
                );
            }

            return;
        }

        overflow_.reserve(requested_capacity);

        for (
            std::size_t index = 0;
            index < inline_size_;
            ++index
        ) {
            overflow_.push_back(
                std::move(inline_[index])
            );
        }

        inline_size_ = 0;
        using_overflow_ = true;
    }

    std::array<Trade, InlineCapacity> inline_ {};
    std::size_t inline_size_ {0};

    std::vector<Trade> overflow_;
    bool using_overflow_ {false};
};

using DefaultTradeBuffer = TradeBuffer<8>;

}  // namespace exchange