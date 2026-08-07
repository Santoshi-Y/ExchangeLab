#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>

#include "exchange/order_book.hpp"

namespace {

exchange::Order make_order(exchange::OrderId id) {
    return {
        .id = id,
        .side = exchange::Side::Buy,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = 100,
        .initial_quantity = 1,
        .remaining_quantity = 1,
        .timestamp = id
    };
}

void churn(
    exchange::OrderBook& book,
    exchange::OrderId& next_id,
    std::uint64_t iterations
) {
    for (std::uint64_t i = 0; i < iterations; ++i) {
        const exchange::OrderId id = next_id++;
        book.add_order(make_order(id));

        if (!book.cancel_order(id)) {
            throw std::runtime_error(
                "memory-pool benchmark cancel failed"
            );
        }
    }
}

}  // namespace

int main() {
    constexpr std::uint64_t warmup_iterations = 50'000;
    constexpr std::uint64_t measured_iterations = 1'000'000;

    exchange::OrderBook book;
    exchange::OrderId next_id = 1;

    churn(book, next_id, warmup_iterations);
    const auto before = book.memory_pool_stats();

    const auto start = std::chrono::steady_clock::now();
    churn(book, next_id, measured_iterations);
    const auto finish = std::chrono::steady_clock::now();

    const auto after = book.memory_pool_stats();

    const std::chrono::duration<double> elapsed = finish - start;
    const double lifecycles_per_second =
        static_cast<double>(measured_iterations) /
        elapsed.count();

    const std::uint64_t new_upstream_allocations =
        after.upstream_allocations -
        before.upstream_allocations;

    const std::uint64_t new_upstream_bytes =
        after.upstream_bytes_allocated -
        before.upstream_bytes_allocated;

    std::cout
        << "ExchangeLab Memory Pool Churn Benchmark\n"
        << "=======================================\n"
        << "Warmup lifecycles: "
        << warmup_iterations << '\n'
        << "Measured lifecycles: "
        << measured_iterations << '\n'
        << std::fixed << std::setprecision(2)
        << "Elapsed: " << elapsed.count() << " s\n"
        << "Order lifecycles/sec: "
        << lifecycles_per_second << '\n'
        << "Upstream allocations during measured phase: "
        << new_upstream_allocations << '\n'
        << "Upstream bytes during measured phase: "
        << new_upstream_bytes << '\n'
        << "Total upstream allocations since startup: "
        << after.upstream_allocations << '\n'
        << "Total upstream bytes since startup: "
        << after.upstream_bytes_allocated << '\n';

    if (new_upstream_allocations == 0) {
        std::cout
            << "Result: hot-path container storage was fully reused "
            << "after warmup.\n";
    } else {
        std::cout
            << "Result: pool expanded during the measured phase; "
            << "repeat with a larger warmup to observe steady state.\n";
    }

    return 0;
}