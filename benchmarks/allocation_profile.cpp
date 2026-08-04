#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "exchange/matching_engine.hpp"
#include "exchange/order.hpp"
#include "exchange/order_book.hpp"
#include "exchange/types.hpp"

namespace allocation_tracking {

std::atomic<bool> enabled {false};
std::atomic<std::uint64_t> allocation_count {0};
std::atomic<std::uint64_t> allocated_bytes {0};
std::atomic<std::uint64_t> deallocation_count {0};

void record_allocation(std::size_t size) noexcept {
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

Snapshot snapshot() noexcept {
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

void* operator new(
    std::size_t size,
    std::align_val_t alignment
) {
    const std::size_t actual_size =
        size == 0 ? 1 : size;

    void* memory = nullptr;

    const std::size_t alignment_value =
        static_cast<std::size_t>(alignment);

    if (
        posix_memalign(
            &memory,
            alignment_value,
            actual_size
        ) != 0
    ) {
        throw std::bad_alloc {};
    }

    allocation_tracking::record_allocation(
        actual_size
    );

    return memory;
}

void* operator new[](
    std::size_t size,
    std::align_val_t alignment
) {
    return ::operator new(size, alignment);
}

void operator delete(
    void* memory,
    std::align_val_t
) noexcept {
    allocation_tracking::record_deallocation();
    std::free(memory);
}

void operator delete[](
    void* memory,
    std::align_val_t alignment
) noexcept {
    ::operator delete(memory, alignment);
}

void operator delete(
    void* memory,
    std::size_t,
    std::align_val_t
) noexcept {
    allocation_tracking::record_deallocation();
    std::free(memory);
}

void operator delete[](
    void* memory,
    std::size_t,
    std::align_val_t alignment
) noexcept {
    ::operator delete(memory, alignment);
}

namespace {

std::atomic<std::uint64_t> result_sink {0};

constexpr std::size_t default_trial_count = 500;
constexpr std::int64_t crowded_order_count = 1'024;
constexpr std::int64_t sparse_level_count = 1'024;
constexpr std::int64_t sweep_level_count = 16;

struct ProfileResult {
    std::string operation;
    std::size_t trials;

    double average_allocations;
    double average_bytes;
    double average_deallocations;

    std::uint64_t minimum_allocations;
    std::uint64_t maximum_allocations;
    std::uint64_t maximum_bytes;
};

struct ProfileAccumulator {
    std::uint64_t total_allocations {0};
    std::uint64_t total_bytes {0};
    std::uint64_t total_deallocations {0};

    std::uint64_t minimum_allocations {
        std::numeric_limits<std::uint64_t>::max()
    };

    std::uint64_t maximum_allocations {0};
    std::uint64_t maximum_bytes {0};

    void add(
        const allocation_tracking::Snapshot& sample
    ) {
        total_allocations += sample.allocations;
        total_bytes += sample.bytes;
        total_deallocations += sample.deallocations;

        minimum_allocations = std::min(
            minimum_allocations,
            sample.allocations
        );

        maximum_allocations = std::max(
            maximum_allocations,
            sample.allocations
        );

        maximum_bytes = std::max(
            maximum_bytes,
            sample.bytes
        );
    }

    [[nodiscard]] ProfileResult result(
        std::string operation,
        std::size_t trials
    ) const {
        const double divisor =
            static_cast<double>(trials);

        return {
            .operation = std::move(operation),
            .trials = trials,
            .average_allocations =
                static_cast<double>(
                    total_allocations
                ) / divisor,
            .average_bytes =
                static_cast<double>(
                    total_bytes
                ) / divisor,
            .average_deallocations =
                static_cast<double>(
                    total_deallocations
                ) / divisor,
            .minimum_allocations =
                minimum_allocations,
            .maximum_allocations =
                maximum_allocations,
            .maximum_bytes =
                maximum_bytes
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

void populate_same_price_bids(
    exchange::OrderBook& book,
    exchange::MatchingEngine& engine,
    std::int64_t order_count
) {
    for (
        std::int64_t index = 0;
        index < order_count;
        ++index
    ) {
        const auto id =
            static_cast<exchange::OrderId>(
                index + 1
            );

        const auto trades = engine.process_order(
            book,
            make_limit_order(
                id,
                exchange::Side::Buy,
                100,
                10,
                id
            )
        );

        result_sink.fetch_add(
            trades.size(),
            std::memory_order_relaxed
        );
    }
}

void populate_sparse_bids(
    exchange::OrderBook& book,
    exchange::MatchingEngine& engine,
    std::int64_t level_count
) {
    for (
        std::int64_t index = 0;
        index < level_count;
        ++index
    ) {
        const auto id =
            static_cast<exchange::OrderId>(
                index + 1
            );

        const auto trades = engine.process_order(
            book,
            make_limit_order(
                id,
                exchange::Side::Buy,
                10'000 - index,
                10,
                id
            )
        );

        result_sink.fetch_add(
            trades.size(),
            std::memory_order_relaxed
        );
    }
}

void populate_ask_levels(
    exchange::OrderBook& book,
    exchange::MatchingEngine& engine,
    std::int64_t level_count
) {
    for (
        std::int64_t index = 0;
        index < level_count;
        ++index
    ) {
        const auto id =
            static_cast<exchange::OrderId>(
                index + 1
            );

        const auto trades = engine.process_order(
            book,
            make_limit_order(
                id,
                exchange::Side::Sell,
                101 + index,
                10,
                id
            )
        );

        result_sink.fetch_add(
            trades.size(),
            std::memory_order_relaxed
        );
    }
}

template <typename Setup, typename Operation>
ProfileResult profile_operation(
    std::string name,
    std::size_t trials,
    Setup&& setup,
    Operation&& operation
) {
    ProfileAccumulator accumulator;

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

    return accumulator.result(
        std::move(name),
        trials
    );
}

void print_results(
    const std::vector<ProfileResult>& results
) {
    std::cout
        << "\nExchangeLab Allocation Profile\n"
        << "==============================\n\n";

    std::cout
        << std::left
        << std::setw(34)
        << "Operation"
        << std::right
        << std::setw(12)
        << "Allocs"
        << std::setw(14)
        << "Bytes"
        << std::setw(12)
        << "Frees"
        << std::setw(12)
        << "Min"
        << std::setw(12)
        << "Max"
        << '\n';

    std::cout
        << std::string(96, '-')
        << '\n';

    for (const ProfileResult& result : results) {
        std::cout
            << std::left
            << std::setw(34)
            << result.operation
            << std::right
            << std::fixed
            << std::setprecision(2)
            << std::setw(12)
            << result.average_allocations
            << std::setw(14)
            << result.average_bytes
            << std::setw(12)
            << result.average_deallocations
            << std::setw(12)
            << result.minimum_allocations
            << std::setw(12)
            << result.maximum_allocations
            << '\n';
    }
}

bool write_csv(
    std::string_view path,
    const std::vector<ProfileResult>& results
) {
    std::ofstream output {
        std::string(path)
    };

    if (!output.is_open()) {
        return false;
    }

    output
        << "operation,trials,"
        << "average_allocations,"
        << "average_bytes,"
        << "average_deallocations,"
        << "minimum_allocations,"
        << "maximum_allocations,"
        << "maximum_bytes\n";

    for (const ProfileResult& result : results) {
        output
            << result.operation
            << ','
            << result.trials
            << ','
            << std::fixed
            << std::setprecision(3)
            << result.average_allocations
            << ','
            << result.average_bytes
            << ','
            << result.average_deallocations
            << ','
            << result.minimum_allocations
            << ','
            << result.maximum_allocations
            << ','
            << result.maximum_bytes
            << '\n';
    }

    return true;
}

std::size_t parse_trial_count(
    int argc,
    char** argv
) {
    if (argc < 2) {
        return default_trial_count;
    }

    try {
        const auto value =
            std::stoull(argv[1]);

        if (value == 0) {
            return default_trial_count;
        }

        return static_cast<std::size_t>(value);
    } catch (...) {
        return default_trial_count;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t trials =
        parse_trial_count(argc, argv);

    std::vector<ProfileResult> results;
    results.reserve(9);

    results.push_back(
        profile_operation(
            "Insert into empty book",
            trials,
            [](
                exchange::OrderBook&,
                exchange::MatchingEngine&,
                std::size_t
            ) {},
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t trial
            ) {
                const auto trades =
                    engine.process_order(
                        book,
                        make_limit_order(
                            static_cast<
                                exchange::OrderId
                            >(trial + 1),
                            exchange::Side::Buy,
                            100,
                            10,
                            trial + 1
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
        profile_operation(
            "Insert into crowded level",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t
            ) {
                populate_same_price_bids(
                    book,
                    engine,
                    crowded_order_count
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
                        make_limit_order(
                            10'000'000 + trial,
                            exchange::Side::Buy,
                            100,
                            10,
                            10'000'000 + trial
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
        profile_operation(
            "Insert new sparse price level",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t
            ) {
                populate_sparse_bids(
                    book,
                    engine,
                    sparse_level_count
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
                        make_limit_order(
                            20'000'000 + trial,
                            exchange::Side::Buy,
                            20'000,
                            10,
                            20'000'000 + trial
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
        profile_operation(
            "Cancel middle crowded order",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t
            ) {
                populate_same_price_bids(
                    book,
                    engine,
                    crowded_order_count
                );
            },
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t
            ) {
                const bool cancelled =
                    engine.cancel_order(
                        book,
                        512
                    );

                result_sink.fetch_add(
                    cancelled ? 1 : 0,
                    std::memory_order_relaxed
                );
            }
        )
    );

    results.push_back(
        profile_operation(
            "Cancel only order at level",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t
            ) {
                const auto trades =
                    engine.process_order(
                        book,
                        make_limit_order(
                            1,
                            exchange::Side::Buy,
                            100,
                            10,
                            1
                        )
                    );

                result_sink.fetch_add(
                    trades.size(),
                    std::memory_order_relaxed
                );
            },
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t
            ) {
                const bool cancelled =
                    engine.cancel_order(
                        book,
                        1
                    );

                result_sink.fetch_add(
                    cancelled ? 1 : 0,
                    std::memory_order_relaxed
                );
            }
        )
    );

    results.push_back(
        profile_operation(
            "Match one resting order",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t
            ) {
                const auto trades =
                    engine.process_order(
                        book,
                        make_limit_order(
                            1,
                            exchange::Side::Sell,
                            101,
                            10,
                            1
                        )
                    );

                result_sink.fetch_add(
                    trades.size(),
                    std::memory_order_relaxed
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
                        make_limit_order(
                            10'000 + trial,
                            exchange::Side::Buy,
                            101,
                            10,
                            2
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
        profile_operation(
            "Market sweep 16 levels",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t
            ) {
                populate_ask_levels(
                    book,
                    engine,
                    sweep_level_count
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
                            30'000'000 + trial,
                            exchange::Side::Buy,
                            160,
                            30'000'000 + trial
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
        profile_operation(
            "Replace at same price",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t
            ) {
                populate_same_price_bids(
                    book,
                    engine,
                    crowded_order_count
                );
            },
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t trial
            ) {
                const auto result =
                    engine.replace_order(
                        book,
                        512,
                        100,
                        20,
                        50'000'000 + trial
                    );

                result_sink.fetch_add(
                    result.trades.size() +
                        (result.replaced ? 1 : 0),
                    std::memory_order_relaxed
                );
            }
        )
    );

    results.push_back(
        profile_operation(
            "Aggressive replace and match",
            trials,
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t
            ) {
                auto trades = engine.process_order(
                    book,
                    make_limit_order(
                        1,
                        exchange::Side::Buy,
                        100,
                        20,
                        1
                    )
                );

                result_sink.fetch_add(
                    trades.size(),
                    std::memory_order_relaxed
                );

                trades = engine.process_order(
                    book,
                    make_limit_order(
                        2,
                        exchange::Side::Sell,
                        105,
                        15,
                        2
                    )
                );

                result_sink.fetch_add(
                    trades.size(),
                    std::memory_order_relaxed
                );
            },
            [](
                exchange::OrderBook& book,
                exchange::MatchingEngine& engine,
                std::size_t trial
            ) {
                const auto result =
                    engine.replace_order(
                        book,
                        1,
                        105,
                        20,
                        60'000'000 + trial
                    );

                result_sink.fetch_add(
                    result.trades.size() +
                        (result.replaced ? 1 : 0),
                    std::memory_order_relaxed
                );
            }
        )
    );

    print_results(results);

    const std::string output_path =
        "benchmark-results/allocation_profile.csv";

    if (!write_csv(output_path, results)) {
        std::cerr
            << "\nCould not write "
            << output_path
            << '\n';

        return 1;
    }

    std::cout
        << "\nTrials per operation: "
        << trials
        << '\n'
        << "Saved results to: "
        << output_path
        << '\n';

    return 0;
}