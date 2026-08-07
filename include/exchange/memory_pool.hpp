#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>

namespace exchange {

struct MemoryPoolStats {
    std::uint64_t upstream_allocations {0};
    std::uint64_t upstream_deallocations {0};
    std::uint64_t upstream_bytes_allocated {0};
    std::uint64_t upstream_bytes_deallocated {0};
};

/*
 * Counts calls that escape the pool and reach the upstream allocator.
 *
 * The matching-engine hot path uses an unsynchronized_pool_resource on top
 * of this resource. A rising upstream allocation count therefore means the
 * pool had to obtain another chunk from the system allocator. Reuse of an
 * already-freed pooled block does not increment these counters.
 */
class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    explicit CountingMemoryResource(
        std::pmr::memory_resource* upstream =
            std::pmr::new_delete_resource()
    ) noexcept
        : upstream_(upstream) {}

    [[nodiscard]] MemoryPoolStats stats() const noexcept {
        return stats_;
    }

private:
    void* do_allocate(
        std::size_t bytes,
        std::size_t alignment
    ) override {
        void* memory = upstream_->allocate(bytes, alignment);

        ++stats_.upstream_allocations;
        stats_.upstream_bytes_allocated +=
            static_cast<std::uint64_t>(bytes);

        return memory;
    }

    void do_deallocate(
        void* memory,
        std::size_t bytes,
        std::size_t alignment
    ) override {
        upstream_->deallocate(memory, bytes, alignment);

        ++stats_.upstream_deallocations;
        stats_.upstream_bytes_deallocated +=
            static_cast<std::uint64_t>(bytes);
    }

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other
    ) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    MemoryPoolStats stats_;
};

/*
 * Per-order-book pool used by the matching engine.
 *
 * unsynchronized_pool_resource is intentional: an OrderBook is already
 * accessed under ExchangeServer's engine mutex, so paying for allocator-side
 * locking would only add overhead. Freed map/list/hash nodes are retained by
 * the pool and reused by later orders instead of returning to operator new.
 */
class MemoryPool {
public:
    MemoryPool()
        : upstream_(),
          pool_(
              std::pmr::pool_options {
                  .max_blocks_per_chunk = 256,
                  .largest_required_pool_block = 4096
              },
              &upstream_
          ) {}

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;

    [[nodiscard]] std::pmr::memory_resource*
    resource() noexcept {
        return &pool_;
    }

    [[nodiscard]] const std::pmr::memory_resource*
    resource() const noexcept {
        return &pool_;
    }

    [[nodiscard]] MemoryPoolStats stats() const noexcept {
        return upstream_.stats();
    }

private:
    CountingMemoryResource upstream_;
    std::pmr::unsynchronized_pool_resource pool_;
};

}  // namespace exchange