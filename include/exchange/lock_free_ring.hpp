#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace exchange {

/*
 * Fixed-capacity single-producer/single-consumer ring buffer.
 *
 * The producer owns write_position_ and the consumer owns read_position_.
 * Each side only observes the other side's index, so enqueue/dequeue do not
 * contend on one shared compare-and-swap location. Capacity must be a power
 * of two and no allocation occurs after construction.
 */
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2, "Ring capacity must be at least 2");
    static_assert(
        (Capacity & (Capacity - 1U)) == 0,
        "Ring capacity must be a power of two"
    );

    static constexpr std::size_t kMask = Capacity - 1U;

public:
    SpscRing() noexcept = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;

    /*
     * reset() is intentionally not concurrent. Call only while the producer
     * and consumer are stopped.
     */
    void reset() noexcept {
        write_position_.store(0, std::memory_order_relaxed);
        read_position_.store(0, std::memory_order_relaxed);
        cached_read_position_ = 0;
        cached_write_position_ = 0;
    }

    template <typename Writer>
    [[nodiscard]] bool try_emplace(Writer&& writer) noexcept {
        static_assert(
            std::is_nothrow_invocable_v<Writer&, T&>,
            "Ring writer callback must be noexcept"
        );

        const std::size_t write = write_position_.load(
            std::memory_order_relaxed
        );

        if (write - cached_read_position_ >= Capacity) {
            cached_read_position_ = read_position_.load(
                std::memory_order_acquire
            );

            if (write - cached_read_position_ >= Capacity) {
                return false;
            }
        }

        T& slot = slots_[write & kMask];
        std::invoke(std::forward<Writer>(writer), slot);

        write_position_.store(
            write + 1U,
            std::memory_order_release
        );

        return true;
    }

    [[nodiscard]] bool try_push(const T& value) noexcept(
        std::is_nothrow_copy_assignable_v<T>
    ) {
        return try_emplace(
            [&value](T& destination) noexcept(
                std::is_nothrow_copy_assignable_v<T>
            ) {
                destination = value;
            }
        );
    }

    [[nodiscard]] bool try_push(T&& value) noexcept(
        std::is_nothrow_move_assignable_v<T>
    ) {
        return try_emplace(
            [&value](T& destination) noexcept(
                std::is_nothrow_move_assignable_v<T>
            ) {
                destination = std::move(value);
            }
        );
    }

    template <typename Reader>
    [[nodiscard]] bool try_consume(Reader&& reader) noexcept {
        static_assert(
            std::is_nothrow_invocable_v<Reader&, const T&>,
            "Ring reader callback must be noexcept"
        );

        const std::size_t read = read_position_.load(
            std::memory_order_relaxed
        );

        if (read == cached_write_position_) {
            cached_write_position_ = write_position_.load(
                std::memory_order_acquire
            );

            if (read == cached_write_position_) {
                return false;
            }
        }

        const T& slot = slots_[read & kMask];
        std::invoke(std::forward<Reader>(reader), slot);

        read_position_.store(
            read + 1U,
            std::memory_order_release
        );

        return true;
    }

    [[nodiscard]] bool try_pop(T& value) noexcept(
        std::is_nothrow_copy_assignable_v<T>
    ) {
        return try_consume(
            [&value](const T& source) noexcept(
                std::is_nothrow_copy_assignable_v<T>
            ) {
                value = source;
            }
        );
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t write = write_position_.load(
            std::memory_order_acquire
        );
        const std::size_t read = read_position_.load(
            std::memory_order_acquire
        );

        const std::size_t difference = write - read;
        return difference > Capacity ? Capacity : difference;
    }

    [[nodiscard]] bool empty_approx() const noexcept {
        return size_approx() == 0;
    }

    static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

    static constexpr bool index_atomics_always_lock_free() noexcept {
        return std::atomic<std::size_t>::is_always_lock_free;
    }

private:
    std::array<T, Capacity> slots_ {};

    // Separate cache lines prevent producer/consumer false sharing.
    alignas(64) std::atomic<std::size_t> write_position_ {0};
    std::size_t cached_read_position_ {0};

    alignas(64) std::atomic<std::size_t> read_position_ {0};
    std::size_t cached_write_position_ {0};
};

}  // namespace exchange