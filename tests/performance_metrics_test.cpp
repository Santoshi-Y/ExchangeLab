#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "exchange/performance_metrics.hpp"

TEST(PerformanceMetricsTest, TracksOrderAndRiskCounters) {
    exchange::PerformanceMetrics metrics;

    metrics.record_new_order(true, false);
    metrics.record_new_order(false, false);
    metrics.record_new_order(false, true);
    metrics.record_cancel(true);
    metrics.record_cancel(false);
    metrics.record_replace(true, false);
    metrics.record_replace(false, true);
    metrics.record_trades(3, 42);

    const auto snapshot = metrics.snapshot();

    EXPECT_EQ(snapshot.new_order_requests, 3U);
    EXPECT_EQ(snapshot.accepted_orders, 1U);
    EXPECT_EQ(snapshot.rejected_orders, 2U);
    EXPECT_EQ(snapshot.risk_rejections, 2U);
    EXPECT_EQ(snapshot.cancel_requests, 2U);
    EXPECT_EQ(snapshot.successful_cancels, 1U);
    EXPECT_EQ(snapshot.replace_requests, 2U);
    EXPECT_EQ(snapshot.successful_replaces, 1U);
    EXPECT_EQ(snapshot.trades, 3U);
    EXPECT_EQ(snapshot.traded_quantity, 42U);
}

TEST(PerformanceMetricsTest, ProducesMonotonicLatencyPercentiles) {
    exchange::PerformanceMetrics metrics;

    for (std::uint64_t value = 100; value <= 10'000; value += 100) {
        metrics.record_matching_latency(value);
    }

    const auto latency = metrics.snapshot().matching_latency;

    EXPECT_EQ(latency.samples, 100U);
    EXPECT_GT(latency.mean_ns, 0.0);
    EXPECT_LE(latency.p50_ns, latency.p95_ns);
    EXPECT_LE(latency.p95_ns, latency.p99_ns);
    EXPECT_LE(latency.p99_ns, latency.max_ns);
    EXPECT_EQ(latency.max_ns, 10'000U);
}

TEST(PerformanceMetricsTest, EmptyLatencySnapshotIsZeroed) {
    exchange::PerformanceMetrics metrics;
    const auto latency = metrics.snapshot().matching_latency;

    EXPECT_EQ(latency.samples, 0U);
    EXPECT_DOUBLE_EQ(latency.mean_ns, 0.0);
    EXPECT_EQ(latency.p50_ns, 0U);
    EXPECT_EQ(latency.p95_ns, 0U);
    EXPECT_EQ(latency.p99_ns, 0U);
    EXPECT_EQ(latency.max_ns, 0U);
}

TEST(PerformanceTelemetryTest, SerializesDashboardFields) {
    exchange::PerformanceTelemetrySnapshot snapshot;
    snapshot.timestamp_ms = 1234;
    snapshot.uptime_ms = 9000;
    snapshot.counters.new_order_requests = 12;
    snapshot.counters.trades = 4;
    snapshot.counters.matching_latency.p99_ns = 2047;
    snapshot.orders_per_second = 5.5;
    snapshot.executions_per_second = 2.25;
    snapshot.active_orders = 7;
    snapshot.instruments = 2;
    snapshot.connected_clients = 3;
    snapshot.market_data.queue_depth = 4;
    snapshot.market_data.dropped = 1;
    snapshot.market_data_queue_capacity = 8192;

    const std::string json =
        exchange::performance_snapshot_to_json(snapshot);

    EXPECT_NE(json.find("\"type\":\"performance\""), std::string::npos);
    EXPECT_NE(json.find("\"ordersTotal\":12"), std::string::npos);
    EXPECT_NE(json.find("\"p99\":2047"), std::string::npos);
    EXPECT_NE(json.find("\"queueCapacity\":8192"), std::string::npos);
    EXPECT_NE(json.find("\"dropped\":1"), std::string::npos);
}