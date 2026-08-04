#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "exchange/matching_engine.hpp"
#include "exchange/order.hpp"
#include "exchange/order_book.hpp"
#include "exchange/trade_buffer.hpp"
#include "exchange/types.hpp"

namespace allocation_tracking {

std::atomic<bool> enabled {false};
std::atomic<std::uint64_t> allocation_count {0};
std::atomic<std::uint64_t> allocated_bytes {0};
std::atomic<std::uint64_t> deallocation_count {0};

void record_allocation(
    std::size_t size
) noexcept {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }

    allocation_count.fetch_add(
        1,
        std::memory_order_relaxed
    );

    allocated_bytes.fetch_add(
        static_cast<std::uint64_t>(size),
        std::memory_order_relaxed
    );
}

void record_deallocation() noexcept {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }

    deallocation_count.fetch_add(
        1,
        std::memory_order_relaxed
    );
}

void reset() noexcept {
    allocation_count.store(
        0,
        std::memory_order_relaxed
    );

    allocated_bytes.store(
        0,
        std::memory_order_relaxed
    );

    deallocation_count.store(
        0,
        std::memory_order_relaxed
    );
}

struct Snapshot {
    std::uint64_t allocations;
    std::uint64_t bytes;
    std::uint64_t deallocations;
};

[[nodiscard]] Snapshot snapshot() noexcept {
    return {
        .allocations =
            allocation_count.load(
                std::memory_order_relaxed
            ),
        .bytes =
            allocated_bytes.load(
                std::memory_order_relaxed
            ),
        .deallocations =
            deallocation_count.load(
                std::memory_order_relaxed
            )
    };
}

template <typename Operation>
Snapshot measure(Operation&& operation) {
    reset();
    enabled.store(true, std::memory_order_relaxed);

    try {
        std::forward<Operation>(operation)();
    } catch (...) {
        enabled.store(false, std::memory_order_relaxed);
        throw;
    }

    enabled.store(false, std::memory_order_relaxed);

    return snapshot();
}

}  // namespace allocation_tracking

void* operator new(std::size_t size) {
    const std::size_t actual_size =
        size == 0 ? 1 : size;

    void* memory = std::malloc(actual_size);

    if (memory == nullptr) {
        throw std::bad_alloc {};
    }

    allocation_tracking::record_allocation(
        actual_size
    );

    return memory;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    allocation_tracking::record_deallocation();
    std::free(memory);
}

void operator delete[](
    void* memory
) noexcept {
    ::operator delete(memory);
}

void operator delete(
    void* memory,
    std::size_t
) noexcept {
    allocation_tracking::record_deallocation();
    std::free(memory);
}

void operator delete[](
    void* memory,
    std::size_t
) noexcept {
    allocation_tracking::record_deallocation();
    std::free(memory);
}

void* operator new(
    std::size_t size,
    const std::nothrow_t&
) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](
    std::size_t size,
    const std::nothrow_t&
) noexcept {
    try {
        return ::operator new[](size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(
    void* memory,
    const std::nothrow_t&
) noexcept {
    ::operator delete(memory);
}

void operator delete[](
    void* memory,
    const std::nothrow_t&
) noexcept {
    ::operator delete[](memory);
}

namespace {

std::atomic<std::uint64_t> result_sink {0};

constexpr std::size_t default_trials = 500;
constexpr std::int64_t sweep_levels = 16;

struct Result {
    std::string name;
    double allocations;
    double bytes;
    double deallocations;
    std::uint64_t minimum_allocations;
    std::uint64_t maximum_allocations;
};

struct Accumulator {
    std::uint64_t allocation_total {0};
    std::uint64_t byte_total {0};
    std::uint64_t deallocation_total {0};

    std::uint64_t minimum_allocations {
        std::numeric_limits<std::uint64_t>::max()
    };

    std::uint64_t maximum_allocations {0};

    void add(
        const allocation_tracking::Snapshot& sample
    ) {
        allocation_total += sample.allocations;
        byte_total += sample.bytes;
        deallocation_total += sample.deallocations;

        minimum_allocations = std::min(
            minimum_allocations,
            sample.allocations
        );

        maximum_allocations = std::max(
            maximum_allocations,
            sample.allocations
        );
    }

    [[nodiscard]] Result finish(
        std::string name,
        std::size_t trials
    ) const {
        const double divisor =
            static_cast<double>(trials);

        return {
            .name = std::move(name),
            .allocations =
                static_cast<double>(
                    allocation_total
                ) / divisor,
            .bytes =
                static_cast<double>(
                    byte_total
                ) / divisor,
            .deallocations =
                static_cast<double>(
                    deallocation_total
                ) / divisor,
            .minimum_allocations =
                minimum_allocations,
            .maximum_allocations =
                maximum_allocations
        };
    }
};

exchange::Order make_limit_order(
    exchange::OrderId id,
    exchange::Side side,
    exchange::Price price,
    exchange::Quantity quantity,
    exchange::Timestamp timestamp
) {
    return {
        .id = id,
        .side = side,
        .type = exchange::OrderType::Limit,
        .time_in_force =
            exchange::TimeInForce::GoodTillCancel,
        .price = price,
        .initial_quantity = quantity,
        .remaining_quantity = quantity,
        .timestamp = timestamp
    };
}

exchange::Order make_market_order(
    exchange::OrderId id,
    exchange::Side side,
    exchange::Quantity quantity,
    exchange::Timestamp timestamp
) {
    return {
        .id = id,
        .side = side,
        .type = exchange::OrderType::Market,
        .time_in_force =
            exchange::TimeInForce::ImmediateOrCancel,
        .price = 0,
        .initial_quantity = quantity,
        .remaining_quantity = quantity,
        .timestamp = timestamp
    };
}

void populate_single_ask(
    exchange::OrderBook& book
) {
    book.add_order(
        make_limit_order(
            1,
            exchange::Side::Sell,
            101,
            10,
            1
        )
    );
}

void populate_ask_levels(
    exchange::OrderBook& book,
    std::int64_t level_count
) {
    for (
        std::int64_t index = 0;
        index < level_count;
        ++index
    ) {
        book.add_order(
            make_limit_order(
                static_cast<exchange::OrderId>(
                    index + 1
                ),
                exchange::Side::Sell,
                101 + index,
                10,
                static_cast<exchange::Timestamp>(
                    index + 1
                )
            )
        );
    }
}

template <typename Setup, typename Operation>
Result profile(
    std::string name,
    std::size_t trials,
    Setup&& setup,
    Operation&& operation
) {
    Accumulator accumulator;

    for (
        std::size_t trial = 0;
        trial < trials;
        ++trial
    ) {
        exchange::OrderBook book;
        exchange::MatchingEngine engine;

        setup(book, engine, trial);

        const auto sample =
            allocation_tracking::measure(
                [&]() {
                    operation(
                        book,
                        engine,
                        trial
                    );
                }
            );

        accumulator.add(sample);
    }

    return accumulator.finish(
        std::move(name),
        trials
    );
}

void print_results(
    const std::vector<Result>& results
) {
    std::cout
        << "\nExchangeLab Trade Buffer Allocation Profile\n"
        << "===========================================\n\n";

    std::cout
        << std::left
        << std::setw(38)
        << "Operation"
        << std::right
        << std::setw(12)
        << "Allocs"
        << std::setw(14)
        << "Bytes"
        << std::setw(12)
        << "Frees"
        << std::setw(10)
        << "Min"
        << std::setw(10)
        << "Max"
        << '\n';

    std::cout
        << std::string(96, '-')
        << '\n';

    for (const Result& result : results) {
        std::cout
            << std::left
            << std::setw(38)
            << result.name
            << std::right
            << std::fixed
            << std::setprecision(2)
            << std::setw(12)
            << result.allocations
            << std::setw(14)
            << result.bytes
            << std::setw(12)
            << result.deallocations
            << std::setw(10)
            << result.minimum_allocations
            << std::setw(10)
            << result.maximum_allocations
            << '\n';
    }
}

std::size_t parse_trials(
    int argc,
    char** argv
) {
    if (argc < 2) {
        return default_trials;
    }

    try {
        const std::size_t parsed =
            static_cast<std::size_t>(
                std::stoull(argv[1])
            );

        return parsed == 0
            ? default_trials
            : parsed;
    } catch (...) {
        return default_trials;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t trials =
        parse_trials(argc, argv);

    std::vector<Result> results;
    results.reserve(5);

    results.push_back(
        profile(
            "Legacy single match",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine&,
                std::size_t
            ) {
                populate_single_ask(book);
            },
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t trial
            ) {
                const auto trades =
                    engine.process_order(
                        book,
                        make_limit_order(
                            10'000 + trial,
                            exchange::Side::Buy,
                            101,
                            10,
                            10'000 + trial
                        )
                    );

                result_sink.fetch_add(
                    trades.size(),
                    std::memory_order_relaxed
                );
            }
        )
    );

    results.push_back(
        profile(
            "Buffered single match",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine&,
                std::size_t
            ) {
                populate_single_ask(book);
            },
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t trial
            ) {
                exchange::DefaultTradeBuffer trades;

                engine.process_order_into(
                    book,
                    make_limit_order(
                        20'000 + trial,
                        exchange::Side::Buy,
                        101,
                        10,
                        20'000 + trial
                    ),
                    trades
                );

                result_sink.fetch_add(
                    trades.size(),
                    std::memory_order_relaxed
                );
            }
        )
    );

    results.push_back(
        profile(
            "Legacy 16-level sweep",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine&,
                std::size_t
            ) {
                populate_ask_levels(
                    book,
                    sweep_levels
                );
            },
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t trial
            ) {
                const auto trades =
                    engine.process_order(
                        book,
                        make_market_order(
                            30'000 + trial,
                            exchange::Side::Buy,
                            160,
                            30'000 + trial
                        )
                    );

                result_sink.fetch_add(
                    trades.size(),
                    std::memory_order_relaxed
                );
            }
        )
    );

    results.push_back(
        profile(
            "Buffered 16-level sweep",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine&,
                std::size_t
            ) {
                populate_ask_levels(
                    book,
                    sweep_levels
                );
            },
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t trial
            ) {
                exchange::DefaultTradeBuffer trades;

                engine.process_order_into(
                    book,
                    make_market_order(
                        40'000 + trial,
                        exchange::Side::Buy,
                        160,
                        40'000 + trial
                    ),
                    trades
                );

                result_sink.fetch_add(
                    trades.size(),
                    std::memory_order_relaxed
                );
            }
        )
    );

    Accumulator reused_accumulator;
    exchange::DefaultTradeBuffer reusable_buffer;

    {
        exchange::OrderBook warmup_book;
        exchange::MatchingEngine warmup_engine;

        populate_ask_levels(
            warmup_book,
            sweep_levels
        );

        warmup_engine.process_order_into(
            warmup_book,
            make_market_order(
                999'999,
                exchange::Side::Buy,
                160,
                999'999
            ),
            reusable_buffer
        );
    }

    for (
        std::size_t trial = 0;
        trial < trials;
        ++trial
    ) {
        exchange::OrderBook book;
        exchange::MatchingEngine engine;

        populate_ask_levels(
            book,
            sweep_levels
        );

        const auto sample =
            allocation_tracking::measure(
                [&]() {
                    engine.process_order_into(
                        book,
                        make_market_order(
                            50'000 + trial,
                            exchange::Side::Buy,
                            160,
                            50'000 + trial
                        ),
                        reusable_buffer
                    );

                    result_sink.fetch_add(
                        reusable_buffer.size(),
                        std::memory_order_relaxed
                    );
                }
            );

        reused_accumulator.add(sample);
    }

    results.push_back(
        reused_accumulator.finish(
            "Reused buffer 16-level sweep",
            trials
        )
    );

    print_results(results);

    std::cout
        << "\nTrials per operation: "
        << trials
        << '\n';

    return 0;
}