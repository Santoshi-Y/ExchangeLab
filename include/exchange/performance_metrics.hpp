#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "exchange/multicast_publisher.hpp"

namespace exchange {

struct MatchingLatencySnapshot {
    std::uint64_t samples {0};
    double mean_ns {0.0};
    std::uint64_t p50_ns {0};
    std::uint64_t p95_ns {0};
    std::uint64_t p99_ns {0};
    std::uint64_t max_ns {0};
};

struct PerformanceCounterSnapshot {
    std::uint64_t new_order_requests {0};
    std::uint64_t accepted_orders {0};
    std::uint64_t rejected_orders {0};
    std::uint64_t risk_rejections {0};
    std::uint64_t cancel_requests {0};
    std::uint64_t successful_cancels {0};
    std::uint64_t replace_requests {0};
    std::uint64_t successful_replaces {0};
    std::uint64_t trades {0};
    std::uint64_t traded_quantity {0};
    MatchingLatencySnapshot matching_latency {};
};

class PerformanceMetrics {
public:
    void record_new_order(
        bool accepted,
        bool risk_rejected
    ) noexcept;

    void record_cancel(bool successful) noexcept;

    void record_replace(
        bool successful,
        bool risk_rejected
    ) noexcept;

    void record_trades(
        std::uint64_t trade_count,
        std::uint64_t quantity
    ) noexcept;

    void record_matching_latency(
        std::uint64_t nanoseconds
    ) noexcept;

    [[nodiscard]] PerformanceCounterSnapshot
    snapshot() const noexcept;

private:
    static constexpr std::size_t latency_bucket_count = 64;

    [[nodiscard]] static std::size_t latency_bucket(
        std::uint64_t nanoseconds
    ) noexcept;

    [[nodiscard]] static std::uint64_t bucket_upper_bound(
        std::size_t bucket
    ) noexcept;

    [[nodiscard]] static std::uint64_t percentile(
        const std::array<std::uint64_t, latency_bucket_count>& histogram,
        std::uint64_t total,
        std::uint64_t numerator,
        std::uint64_t denominator,
        std::uint64_t observed_max
    ) noexcept;

    std::atomic<std::uint64_t> new_order_requests_ {0};
    std::atomic<std::uint64_t> accepted_orders_ {0};
    std::atomic<std::uint64_t> rejected_orders_ {0};
    std::atomic<std::uint64_t> risk_rejections_ {0};
    std::atomic<std::uint64_t> cancel_requests_ {0};
    std::atomic<std::uint64_t> successful_cancels_ {0};
    std::atomic<std::uint64_t> replace_requests_ {0};
    std::atomic<std::uint64_t> successful_replaces_ {0};
    std::atomic<std::uint64_t> trades_ {0};
    std::atomic<std::uint64_t> traded_quantity_ {0};

    std::array<std::atomic<std::uint64_t>, latency_bucket_count>
        latency_histogram_ {};
    std::atomic<std::uint64_t> latency_samples_ {0};
    std::atomic<std::uint64_t> latency_sum_ns_ {0};
    std::atomic<std::uint64_t> latency_max_ns_ {0};
};

struct PerformanceTelemetryConfig {
    std::string host {"127.0.0.1"};
    std::uint16_t port {9200};
};

struct PerformanceTelemetrySnapshot {
    std::uint64_t timestamp_ms {0};
    std::uint64_t uptime_ms {0};

    PerformanceCounterSnapshot counters {};

    double orders_per_second {0.0};
    double executions_per_second {0.0};

    std::uint64_t active_orders {0};
    std::uint64_t instruments {0};
    std::uint64_t connected_clients {0};

    MulticastPublisherStats market_data {};
    std::uint64_t market_data_queue_capacity {0};
};

[[nodiscard]] std::string performance_snapshot_to_json(
    const PerformanceTelemetrySnapshot& snapshot
);

class PerformanceTelemetryPublisher {
public:
    explicit PerformanceTelemetryPublisher(
        PerformanceTelemetryConfig config = {}
    );

    ~PerformanceTelemetryPublisher();

    PerformanceTelemetryPublisher(
        const PerformanceTelemetryPublisher&
    ) = delete;
    PerformanceTelemetryPublisher& operator=(
        const PerformanceTelemetryPublisher&
    ) = delete;
    PerformanceTelemetryPublisher(
        PerformanceTelemetryPublisher&&
    ) = delete;
    PerformanceTelemetryPublisher& operator=(
        PerformanceTelemetryPublisher&&
    ) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;

    [[nodiscard]] bool send(
        const PerformanceTelemetrySnapshot& snapshot
    ) noexcept;

    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] const PerformanceTelemetryConfig&
    config() const noexcept;

private:
    struct Destination;

    PerformanceTelemetryConfig config_;
    int socket_ {-1};
    Destination* destination_ {nullptr};
};

}  // namespace exchange