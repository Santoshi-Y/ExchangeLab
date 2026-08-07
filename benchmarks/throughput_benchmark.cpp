#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "exchange/matching_engine.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct TrialResult {
    double elapsed_seconds {0.0};
    double orders_per_second {0.0};
    double trades_per_second {0.0};
    std::uint64_t orders {0};
    std::uint64_t trades {0};
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

std::uint64_t parse_positive_integer(
    const char* text,
    std::string_view name
) {
    if (text == nullptr || *text == '\0') {
        throw std::invalid_argument(
            std::string(name) + " must not be empty"
        );
    }

    char* end = nullptr;
    const unsigned long long parsed =
        std::strtoull(text, &end, 10);

    if (
        end == text ||
        *end != '\0' ||
        parsed == 0
    ) {
        throw std::invalid_argument(
            std::string(name) +
            " must be a positive integer"
        );
    }

    return static_cast<std::uint64_t>(parsed);
}

TrialResult run_rest_and_cancel_trial(
    std::uint64_t iterations
) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;
    exchange::MatchingEngine::BufferedTrades trades;

    const auto start = Clock::now();

    for (
        std::uint64_t index = 0;
        index < iterations;
        ++index
    ) {
        const exchange::OrderId order_id =
            index + 1;

        engine.process_order_into(
            book,
            make_limit_order(
                order_id,
                exchange::Side::Buy,
                100,
                10,
                index + 1
            ),
            trades
        );

        if (!engine.cancel_order(book, order_id)) {
            throw std::runtime_error(
                "Rest-and-cancel workload lost an order"
            );
        }
    }

    const auto finish = Clock::now();

    const double elapsed_seconds =
        std::chrono::duration<double>(
            finish - start
        ).count();

    return {
        .elapsed_seconds = elapsed_seconds,
        .orders_per_second =
            static_cast<double>(iterations) /
            elapsed_seconds,
        .trades_per_second = 0.0,
        .orders = iterations,
        .trades = 0
    };
}

TrialResult run_crossing_match_trial(
    std::uint64_t iterations
) {
    exchange::OrderBook book;
    exchange::MatchingEngine engine;
    exchange::MatchingEngine::BufferedTrades trades;

    std::uint64_t total_orders = 0;
    std::uint64_t total_trades = 0;

    const auto start = Clock::now();

    for (
        std::uint64_t index = 0;
        index < iterations;
        ++index
    ) {
        const exchange::OrderId sell_id =
            (index * 2U) + 1U;

        const exchange::OrderId buy_id =
            sell_id + 1U;

        engine.process_order_into(
            book,
            make_limit_order(
                sell_id,
                exchange::Side::Sell,
                101,
                10,
                sell_id
            ),
            trades
        );

        ++total_orders;

        engine.process_order_into(
            book,
            make_limit_order(
                buy_id,
                exchange::Side::Buy,
                101,
                10,
                buy_id
            ),
            trades
        );

        ++total_orders;
        total_trades +=
            static_cast<std::uint64_t>(
                trades.size()
            );
    }

    const auto finish = Clock::now();

    if (!book.empty()) {
        throw std::runtime_error(
            "Crossing-match workload left orders resting"
        );
    }

    const double elapsed_seconds =
        std::chrono::duration<double>(
            finish - start
        ).count();

    return {
        .elapsed_seconds = elapsed_seconds,
        .orders_per_second =
            static_cast<double>(total_orders) /
            elapsed_seconds,
        .trades_per_second =
            static_cast<double>(total_trades) /
            elapsed_seconds,
        .orders = total_orders,
        .trades = total_trades
    };
}

double percentile(
    std::vector<double> values,
    double probability
) {
    if (values.empty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());

    const double position =
        probability *
        static_cast<double>(values.size() - 1U);

    const auto lower =
        static_cast<std::size_t>(position);

    const auto upper =
        std::min(
            lower + 1U,
            values.size() - 1U
        );

    const double fraction =
        position -
        static_cast<double>(lower);

    return values[lower] +
        ((values[upper] - values[lower]) * fraction);
}

void print_summary(
    std::string_view workload,
    const std::vector<TrialResult>& results
) {
    std::vector<double> order_rates;
    std::vector<double> trade_rates;
    std::vector<double> elapsed_times;

    order_rates.reserve(results.size());
    trade_rates.reserve(results.size());
    elapsed_times.reserve(results.size());

    for (const TrialResult& result : results) {
        order_rates.push_back(
            result.orders_per_second
        );

        trade_rates.push_back(
            result.trades_per_second
        );

        elapsed_times.push_back(
            result.elapsed_seconds
        );
    }

    std::cout
        << '\n'
        << workload
        << '\n'
        << std::string(workload.size(), '-')
        << '\n'
        << std::fixed
        << std::setprecision(2)
        << "Median elapsed:     "
        << percentile(elapsed_times, 0.50)
        << " s\n"
        << "Median orders/sec:  "
        << percentile(order_rates, 0.50)
        << '\n'
        << "p10 orders/sec:     "
        << percentile(order_rates, 0.10)
        << '\n'
        << "p90 orders/sec:     "
        << percentile(order_rates, 0.90)
        << '\n';

    if (
        percentile(trade_rates, 0.50) > 0.0
    ) {
        std::cout
            << "Median trades/sec:  "
            << percentile(trade_rates, 0.50)
            << '\n'
            << "p10 trades/sec:     "
            << percentile(trade_rates, 0.10)
            << '\n'
            << "p90 trades/sec:     "
            << percentile(trade_rates, 0.90)
            << '\n';
    }
}

void write_csv(
    const std::filesystem::path& path,
    const std::vector<TrialResult>& rest_results,
    const std::vector<TrialResult>& match_results
) {
    const std::filesystem::path parent =
        path.parent_path();

    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream output(path);

    if (!output.is_open()) {
        throw std::runtime_error(
            "Could not open throughput CSV"
        );
    }

    output
        << "workload,trial,orders,trades,"
        << "elapsed_seconds,orders_per_second,"
        << "trades_per_second\n";

    for (
        std::size_t index = 0;
        index < rest_results.size();
        ++index
    ) {
        const TrialResult& result =
            rest_results[index];

        output
            << "rest_and_cancel,"
            << (index + 1U)
            << ','
            << result.orders
            << ','
            << result.trades
            << ','
            << std::setprecision(12)
            << result.elapsed_seconds
            << ','
            << result.orders_per_second
            << ','
            << result.trades_per_second
            << '\n';
    }

    for (
        std::size_t index = 0;
        index < match_results.size();
        ++index
    ) {
        const TrialResult& result =
            match_results[index];

        output
            << "crossing_match,"
            << (index + 1U)
            << ','
            << result.orders
            << ','
            << result.trades
            << ','
            << std::setprecision(12)
            << result.elapsed_seconds
            << ','
            << result.orders_per_second
            << ','
            << result.trades_per_second
            << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::uint64_t iterations = 1'000'000;
        std::uint64_t trials = 7;

        if (argc >= 2) {
            iterations =
                parse_positive_integer(
                    argv[1],
                    "iterations"
                );
        }

        if (argc >= 3) {
            trials =
                parse_positive_integer(
                    argv[2],
                    "trials"
                );
        }

        if (argc > 3) {
            std::cerr
                << "Usage: "
                << argv[0]
                << " [iterations] [trials]\n";

            return 1;
        }

        if (
            iterations >
            (std::numeric_limits<
                exchange::OrderId
            >::max() / 2U)
        ) {
            throw std::invalid_argument(
                "iterations is too large"
            );
        }

        const std::uint64_t warmup_iterations =
            std::min<std::uint64_t>(
                iterations,
                100'000
            );

        std::cout
            << "ExchangeLab Matching Engine Throughput\n"
            << "======================================\n"
            << "Iterations per trial: "
            << iterations
            << '\n'
            << "Trials: "
            << trials
            << '\n'
            << "Warmup iterations: "
            << warmup_iterations
            << '\n';

        /*
         * Warm both workloads before measuring so one-time
         * initialization does not dominate the first trial.
         */
        static_cast<void>(
            run_rest_and_cancel_trial(
                warmup_iterations
            )
        );

        static_cast<void>(
            run_crossing_match_trial(
                warmup_iterations
            )
        );

        std::vector<TrialResult> rest_results;
        std::vector<TrialResult> match_results;

        rest_results.reserve(
            static_cast<std::size_t>(trials)
        );

        match_results.reserve(
            static_cast<std::size_t>(trials)
        );

        for (
            std::uint64_t trial = 0;
            trial < trials;
            ++trial
        ) {
            rest_results.push_back(
                run_rest_and_cancel_trial(
                    iterations
                )
            );

            match_results.push_back(
                run_crossing_match_trial(
                    iterations
                )
            );
        }

        print_summary(
            "Rest + cancel lifecycle",
            rest_results
        );

        print_summary(
            "Crossing limit-order match",
            match_results
        );

        const std::filesystem::path output_path =
            "benchmark-results/throughput.csv";

        write_csv(
            output_path,
            rest_results,
            match_results
        );

        std::cout
            << "\nSaved results to: "
            << std::quoted(
                output_path.string()
            )
            << '\n';

        return 0;
    } catch (const std::exception& exception) {
        std::cerr
            << "Throughput benchmark failed: "
            << exception.what()
            << '\n';

        return 1;
    }
}