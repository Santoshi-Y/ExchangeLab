#include "exchange/performance_metrics.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#error "Windows support not implemented yet."
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace exchange {

void PerformanceMetrics::record_new_order(
    bool accepted,
    bool risk_rejected
) noexcept {
    new_order_requests_.fetch_add(1, std::memory_order_relaxed);

    if (accepted) {
        accepted_orders_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    rejected_orders_.fetch_add(1, std::memory_order_relaxed);

    if (risk_rejected) {
        risk_rejections_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PerformanceMetrics::record_cancel(
    bool successful
) noexcept {
    cancel_requests_.fetch_add(1, std::memory_order_relaxed);

    if (successful) {
        successful_cancels_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PerformanceMetrics::record_replace(
    bool successful,
    bool risk_rejected
) noexcept {
    replace_requests_.fetch_add(1, std::memory_order_relaxed);

    if (successful) {
        successful_replaces_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (risk_rejected) {
        risk_rejections_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PerformanceMetrics::record_trades(
    std::uint64_t trade_count,
    std::uint64_t quantity
) noexcept {
    trades_.fetch_add(trade_count, std::memory_order_relaxed);
    traded_quantity_.fetch_add(quantity, std::memory_order_relaxed);
}

void PerformanceMetrics::record_matching_latency(
    std::uint64_t nanoseconds
) noexcept {
    const std::size_t bucket = latency_bucket(nanoseconds);

    latency_histogram_[bucket].fetch_add(
        1,
        std::memory_order_relaxed
    );

    latency_samples_.fetch_add(1, std::memory_order_relaxed);
    latency_sum_ns_.fetch_add(nanoseconds, std::memory_order_relaxed);

    std::uint64_t observed = latency_max_ns_.load(
        std::memory_order_relaxed
    );

    while (
        observed < nanoseconds &&
        !latency_max_ns_.compare_exchange_weak(
            observed,
            nanoseconds,
            std::memory_order_relaxed,
            std::memory_order_relaxed
        )
    ) {
    }
}

PerformanceCounterSnapshot
PerformanceMetrics::snapshot() const noexcept {
    std::array<std::uint64_t, latency_bucket_count> histogram {};

    for (std::size_t index = 0; index < histogram.size(); ++index) {
        histogram[index] = latency_histogram_[index].load(
            std::memory_order_relaxed
        );
    }

    const std::uint64_t samples = latency_samples_.load(
        std::memory_order_relaxed
    );
    const std::uint64_t sum = latency_sum_ns_.load(
        std::memory_order_relaxed
    );
    const std::uint64_t maximum = latency_max_ns_.load(
        std::memory_order_relaxed
    );

    MatchingLatencySnapshot latency {
        .samples = samples,
        .mean_ns = samples == 0
            ? 0.0
            : static_cast<double>(sum) /
                static_cast<double>(samples),
        .p50_ns = percentile(histogram, samples, 50, 100, maximum),
        .p95_ns = percentile(histogram, samples, 95, 100, maximum),
        .p99_ns = percentile(histogram, samples, 99, 100, maximum),
        .max_ns = maximum
    };

    return {
        .new_order_requests = new_order_requests_.load(
            std::memory_order_relaxed
        ),
        .accepted_orders = accepted_orders_.load(
            std::memory_order_relaxed
        ),
        .rejected_orders = rejected_orders_.load(
            std::memory_order_relaxed
        ),
        .risk_rejections = risk_rejections_.load(
            std::memory_order_relaxed
        ),
        .cancel_requests = cancel_requests_.load(
            std::memory_order_relaxed
        ),
        .successful_cancels = successful_cancels_.load(
            std::memory_order_relaxed
        ),
        .replace_requests = replace_requests_.load(
            std::memory_order_relaxed
        ),
        .successful_replaces = successful_replaces_.load(
            std::memory_order_relaxed
        ),
        .trades = trades_.load(std::memory_order_relaxed),
        .traded_quantity = traded_quantity_.load(
            std::memory_order_relaxed
        ),
        .matching_latency = latency
    };
}

std::size_t PerformanceMetrics::latency_bucket(
    std::uint64_t nanoseconds
) noexcept {
    if (nanoseconds == 0) {
        return 0;
    }

    const unsigned int width = std::bit_width(nanoseconds);
    const std::size_t bucket = static_cast<std::size_t>(width - 1U);

    return std::min(bucket, latency_bucket_count - 1U);
}

std::uint64_t PerformanceMetrics::bucket_upper_bound(
    std::size_t bucket
) noexcept {
    if (bucket >= 63U) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    return (std::uint64_t {1} << (bucket + 1U)) - 1U;
}

std::uint64_t PerformanceMetrics::percentile(
    const std::array<std::uint64_t, latency_bucket_count>& histogram,
    std::uint64_t total,
    std::uint64_t numerator,
    std::uint64_t denominator,
    std::uint64_t observed_max
) noexcept {
    if (total == 0 || denominator == 0) {
        return 0;
    }

    const std::uint64_t target = std::max<std::uint64_t>(
        1,
        (total * numerator + denominator - 1U) / denominator
    );

    std::uint64_t cumulative = 0;

    for (std::size_t bucket = 0; bucket < histogram.size(); ++bucket) {
        cumulative += histogram[bucket];

        if (cumulative >= target) {
            return std::min(
                bucket_upper_bound(bucket),
                observed_max
            );
        }
    }

    return observed_max;
}

std::string performance_snapshot_to_json(
    const PerformanceTelemetrySnapshot& snapshot
) {
    std::ostringstream json;
    json << std::fixed << std::setprecision(2);

    json
        << "{\"type\":\"performance\""
        << ",\"timestampMs\":" << snapshot.timestamp_ms
        << ",\"uptimeMs\":" << snapshot.uptime_ms
        << ",\"ordersTotal\":" << snapshot.counters.new_order_requests
        << ",\"acceptedTotal\":" << snapshot.counters.accepted_orders
        << ",\"rejectedTotal\":" << snapshot.counters.rejected_orders
        << ",\"riskRejectedTotal\":" << snapshot.counters.risk_rejections
        << ",\"cancelRequestsTotal\":" << snapshot.counters.cancel_requests
        << ",\"successfulCancelsTotal\":" << snapshot.counters.successful_cancels
        << ",\"replaceRequestsTotal\":" << snapshot.counters.replace_requests
        << ",\"successfulReplacesTotal\":" << snapshot.counters.successful_replaces
        << ",\"tradesTotal\":" << snapshot.counters.trades
        << ",\"tradedQuantity\":" << snapshot.counters.traded_quantity
        << ",\"ordersPerSecond\":" << snapshot.orders_per_second
        << ",\"executionsPerSecond\":" << snapshot.executions_per_second
        << ",\"activeOrders\":" << snapshot.active_orders
        << ",\"instruments\":" << snapshot.instruments
        << ",\"connectedClients\":" << snapshot.connected_clients
        << ",\"matchingLatencyNs\":{"
        << "\"samples\":" << snapshot.counters.matching_latency.samples
        << ",\"mean\":" << snapshot.counters.matching_latency.mean_ns
        << ",\"p50\":" << snapshot.counters.matching_latency.p50_ns
        << ",\"p95\":" << snapshot.counters.matching_latency.p95_ns
        << ",\"p99\":" << snapshot.counters.matching_latency.p99_ns
        << ",\"max\":" << snapshot.counters.matching_latency.max_ns
        << "}"
        << ",\"marketData\":{"
        << "\"enqueued\":" << snapshot.market_data.enqueued
        << ",\"sent\":" << snapshot.market_data.sent
        << ",\"dropped\":" << snapshot.market_data.dropped
        << ",\"sendErrors\":" << snapshot.market_data.send_errors
        << ",\"queueDepth\":" << snapshot.market_data.queue_depth
        << ",\"maxQueueDepth\":" << snapshot.market_data.max_queue_depth
        << ",\"queueCapacity\":" << snapshot.market_data_queue_capacity
        << ",\"producerShardsUsed\":" << snapshot.market_data.producer_shards_used
        << ",\"producerRegistrationFailures\":"
        << snapshot.market_data.producer_registration_failures
        << "}}";

    return json.str();
}

struct PerformanceTelemetryPublisher::Destination {
    sockaddr_in address {};
};

PerformanceTelemetryPublisher::PerformanceTelemetryPublisher(
    PerformanceTelemetryConfig config
)
    : config_(std::move(config)) {}

PerformanceTelemetryPublisher::~PerformanceTelemetryPublisher() {
    stop();
}

bool PerformanceTelemetryPublisher::start() {
    if (is_open()) {
        return true;
    }

    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);

    if (socket_ < 0) {
        return false;
    }

    auto destination = std::make_unique<Destination>();
    destination->address.sin_family = AF_INET;
    destination->address.sin_port = htons(config_.port);

    if (
        ::inet_pton(
            AF_INET,
            config_.host.c_str(),
            &destination->address.sin_addr
        ) != 1
    ) {
        ::close(socket_);
        socket_ = -1;
        return false;
    }

    destination_ = destination.release();
    return true;
}

void PerformanceTelemetryPublisher::stop() noexcept {
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }

    delete destination_;
    destination_ = nullptr;
}

bool PerformanceTelemetryPublisher::send(
    const PerformanceTelemetrySnapshot& snapshot
) noexcept {
    if (!is_open()) {
        return false;
    }

    try {
        const std::string json = performance_snapshot_to_json(snapshot);

        const auto sent = ::sendto(
            socket_,
            json.data(),
            json.size(),
            0,
            reinterpret_cast<const sockaddr*>(
                &destination_->address
            ),
            sizeof(destination_->address)
        );

        return sent == static_cast<ssize_t>(json.size());
    } catch (...) {
        return false;
    }
}

bool PerformanceTelemetryPublisher::is_open() const noexcept {
    return socket_ >= 0 && destination_ != nullptr;
}

const PerformanceTelemetryConfig&
PerformanceTelemetryPublisher::config() const noexcept {
    return config_;
}

}  // namespace exchange