#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

#include "exchange/matching_engine.hpp"

namespace {

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
            exchange::TimeInForce::GoodTillCancel,
        .price = 0,
        .initial_quantity = quantity,
        .remaining_quantity = quantity,
        .timestamp = timestamp
    };
}

void populate_same_price_bids(
    exchange::OrderBook& book,
    exchange::MatchingEngine& engine,
    std::int64_t order_count,
    exchange::Price price = 100
) {
    for (
        std::int64_t index = 0;
        index < order_count;
        ++index
    ) {
        engine.process_order(
            book,
            make_limit_order(
                static_cast<exchange::OrderId>(
                    index + 1
                ),
                exchange::Side::Buy,
                price,
                10,
                static_cast<exchange::Timestamp>(
                    index + 1
                )
            )
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
        engine.process_order(
            book,
            make_limit_order(
                static_cast<exchange::OrderId>(
                    index + 1
                ),
                exchange::Side::Buy,
                10'000 - index,
                10,
                static_cast<exchange::Timestamp>(
                    index + 1
                )
            )
        );
    }
}

void populate_ask_levels(
    exchange::OrderBook& book,
    exchange::MatchingEngine& engine,
    std::int64_t level_count,
    exchange::Quantity quantity_per_level
) {
    for (
        std::int64_t index = 0;
        index < level_count;
        ++index
    ) {
        engine.process_order(
            book,
            make_limit_order(
                static_cast<exchange::OrderId>(
                    index + 1
                ),
                exchange::Side::Sell,
                101 + index,
                quantity_per_level,
                static_cast<exchange::Timestamp>(
                    index + 1
                )
            )
        );
    }
}

static void BM_InsertIntoEmptyBook(
    benchmark::State& state
) {
    for (auto _ : state) {
        state.PauseTiming();

        exchange::OrderBook book;
        exchange::MatchingEngine engine;

        exchange::Order incoming =
            make_limit_order(
                1,
                exchange::Side::Buy,
                100,
                10,
                1
            );

        state.ResumeTiming();

        const std::vector<exchange::Trade> trades =
            engine.process_order(
                book,
                incoming
            );

        benchmark::DoNotOptimize(trades);
        benchmark::DoNotOptimize(book.order_count());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        state.iterations()
    );
}

BENCHMARK(BM_InsertIntoEmptyBook)
    ->Unit(benchmark::kNanosecond);

static void BM_InsertAtCrowdedPriceLevel(
    benchmark::State& state
) {
    const std::int64_t existing_order_count =
        state.range(0);

    for (auto _ : state) {
        state.PauseTiming();

        exchange::OrderBook book;
        exchange::MatchingEngine engine;

        populate_same_price_bids(
            book,
            engine,
            existing_order_count
        );

        const exchange::OrderId incoming_id =
            static_cast<exchange::OrderId>(
                existing_order_count + 1
            );

        exchange::Order incoming =
            make_limit_order(
                incoming_id,
                exchange::Side::Buy,
                100,
                10,
                static_cast<exchange::Timestamp>(
                    incoming_id
                )
            );

        state.ResumeTiming();

        const std::vector<exchange::Trade> trades =
            engine.process_order(
                book,
                incoming
            );

        benchmark::DoNotOptimize(trades);
        benchmark::DoNotOptimize(book.order_count());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        state.iterations()
    );

    state.counters["existing_orders"] =
        static_cast<double>(
            existing_order_count
        );
}

BENCHMARK(BM_InsertAtCrowdedPriceLevel)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Unit(benchmark::kNanosecond);

static void BM_InsertAtNewPriceLevel(
    benchmark::State& state
) {
    const std::int64_t existing_level_count =
        state.range(0);

    for (auto _ : state) {
        state.PauseTiming();

        exchange::OrderBook book;
        exchange::MatchingEngine engine;

        populate_sparse_bids(
            book,
            engine,
            existing_level_count
        );

        const exchange::OrderId incoming_id =
            static_cast<exchange::OrderId>(
                existing_level_count + 1
            );

        exchange::Order incoming =
            make_limit_order(
                incoming_id,
                exchange::Side::Buy,
                20'000,
                10,
                static_cast<exchange::Timestamp>(
                    incoming_id
                )
            );

        state.ResumeTiming();

        const std::vector<exchange::Trade> trades =
            engine.process_order(
                book,
                incoming
            );

        benchmark::DoNotOptimize(trades);
        benchmark::DoNotOptimize(book.order_count());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        state.iterations()
    );

    state.counters["existing_levels"] =
        static_cast<double>(
            existing_level_count
        );
}

BENCHMARK(BM_InsertAtNewPriceLevel)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Unit(benchmark::kNanosecond);

static void BM_MatchSingleRestingOrder(
    benchmark::State& state
) {
    for (auto _ : state) {
        state.PauseTiming();

        exchange::OrderBook book;
        exchange::MatchingEngine engine;

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

        exchange::Order incoming =
            make_limit_order(
                2,
                exchange::Side::Buy,
                101,
                10,
                2
            );

        state.ResumeTiming();

        const std::vector<exchange::Trade> trades =
            engine.process_order(
                book,
                incoming
            );

        benchmark::DoNotOptimize(trades);
        benchmark::DoNotOptimize(book.empty());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        state.iterations()
    );
}

BENCHMARK(BM_MatchSingleRestingOrder)
    ->Unit(benchmark::kNanosecond);

static void BM_MarketSweepMultipleLevels(
    benchmark::State& state
) {
    const std::int64_t level_count =
        state.range(0);

    constexpr exchange::Quantity
        quantity_per_level = 10;

    const exchange::Quantity sweep_quantity =
        static_cast<exchange::Quantity>(
            level_count
        ) * quantity_per_level;

    for (auto _ : state) {
        state.PauseTiming();

        exchange::OrderBook book;
        exchange::MatchingEngine engine;

        populate_ask_levels(
            book,
            engine,
            level_count,
            quantity_per_level
        );

        exchange::Order incoming =
            make_market_order(
                static_cast<exchange::OrderId>(
                    level_count + 1
                ),
                exchange::Side::Buy,
                sweep_quantity,
                static_cast<exchange::Timestamp>(
                    level_count + 1
                )
            );

        state.ResumeTiming();

        const std::vector<exchange::Trade> trades =
            engine.process_order(
                book,
                incoming
            );

        benchmark::DoNotOptimize(trades);
        benchmark::DoNotOptimize(book.empty());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        state.iterations() *
        level_count
    );

    state.counters["levels_swept"] =
        static_cast<double>(level_count);
}

BENCHMARK(BM_MarketSweepMultipleLevels)
    ->Arg(1)
    ->Arg(4)
    ->Arg(16)
    ->Arg(64)
    ->Unit(benchmark::kNanosecond);

static void BM_CancelOrder(
    benchmark::State& state
) {
    const std::int64_t order_count =
        state.range(0);

    const exchange::OrderId target_id =
        static_cast<exchange::OrderId>(
            (order_count / 2) + 1
        );

    for (auto _ : state) {
        state.PauseTiming();

        exchange::OrderBook book;
        exchange::MatchingEngine engine;

        populate_same_price_bids(
            book,
            engine,
            order_count
        );

        state.ResumeTiming();

        const bool cancelled =
            engine.cancel_order(
                book,
                target_id
            );

        benchmark::DoNotOptimize(cancelled);
        benchmark::DoNotOptimize(book.order_count());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        state.iterations()
    );

    state.counters["book_orders"] =
        static_cast<double>(order_count);
}

BENCHMARK(BM_CancelOrder)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Unit(benchmark::kNanosecond);

static void BM_CancelBestPriceOrder(
    benchmark::State& state
) {
    const std::int64_t level_count =
        state.range(0);

    const exchange::OrderId target_id =
        static_cast<exchange::OrderId>(
            level_count
        );

    for (auto _ : state) {
        state.PauseTiming();

        exchange::OrderBook book;
        exchange::MatchingEngine engine;

        populate_sparse_bids(
            book,
            engine,
            level_count
        );

        state.ResumeTiming();

        const bool cancelled =
            engine.cancel_order(
                book,
                target_id
            );

        benchmark::DoNotOptimize(cancelled);
        benchmark::DoNotOptimize(book.order_count());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        state.iterations()
    );

    state.counters["price_levels"] =
        static_cast<double>(level_count);
}

BENCHMARK(BM_CancelBestPriceOrder)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Unit(benchmark::kNanosecond);

static void BM_ReplaceAtSamePrice(
    benchmark::State& state
) {
    const std::int64_t order_count =
        state.range(0);

    const exchange::OrderId target_id =
        static_cast<exchange::OrderId>(
            (order_count / 2) + 1
        );

    for (auto _ : state) {
        state.PauseTiming();

        exchange::OrderBook book;
        exchange::MatchingEngine engine;

        populate_same_price_bids(
            book,
            engine,
            order_count
        );

        state.ResumeTiming();

        const exchange::ReplaceResult result =
            engine.replace_order(
                book,
                target_id,
                100,
                20,
                static_cast<exchange::Timestamp>(
                    order_count + 1
                )
            );

        benchmark::DoNotOptimize(result.replaced);
        benchmark::DoNotOptimize(result.trades);
        benchmark::DoNotOptimize(book.order_count());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        state.iterations()
    );

    state.counters["book_orders"] =
        static_cast<double>(order_count);
}

BENCHMARK(BM_ReplaceAtSamePrice)
    ->Arg(16)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Unit(benchmark::kNanosecond);

static void BM_AggressiveReplaceAndMatch(
    benchmark::State& state
) {
    for (auto _ : state) {
        state.PauseTiming();

        exchange::OrderBook book;
        exchange::MatchingEngine engine;

        engine.process_order(
            book,
            make_limit_order(
                1,
                exchange::Side::Buy,
                100,
                20,
                1
            )
        );

        engine.process_order(
            book,
            make_limit_order(
                2,
                exchange::Side::Sell,
                105,
                15,
                2
            )
        );

        state.ResumeTiming();

        const exchange::ReplaceResult result =
            engine.replace_order(
                book,
                1,
                105,
                20,
                3
            );

        benchmark::DoNotOptimize(result.replaced);
        benchmark::DoNotOptimize(result.trades);
        benchmark::DoNotOptimize(book.order_count());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(
        state.iterations()
    );
}

BENCHMARK(BM_AggressiveReplaceAndMatch)
    ->Unit(benchmark::kNanosecond);

}  // namespace