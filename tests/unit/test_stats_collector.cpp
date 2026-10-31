#include <gtest/gtest.h>
#include "common/infra/StatsCollector.h"

#include <chrono>
#include <thread>
#include <vector>

TEST(StatsCollector, StartsAtZero) {
    StatsCollector stats;

    EXPECT_EQ(stats.totalCount(), 0u);
}

TEST(StatsCollector, RecordsEachStatusIndependently) {
    StatsCollector stats;

    stats.record(Status::Ok);
    stats.record(Status::Ok);
    stats.record(Status::Warning);
    stats.record(Status::Critical);
    stats.record(Status::Invalid);

    EXPECT_EQ(stats.okCount(), 2u);
    EXPECT_EQ(stats.warningCount(), 1u);
    EXPECT_EQ(stats.criticalCount(), 1u);
    EXPECT_EQ(stats.invalidCount(), 1u);
    EXPECT_EQ(stats.totalCount(), 5u);
}

TEST(StatsCollector, MaxLatencyStartsAtZero) {
    StatsCollector stats;

    EXPECT_EQ(stats.maxLatency(), std::chrono::nanoseconds(0));
}

TEST(StatsCollector, RecordLatencyUpdatesMax) {
    StatsCollector stats;

    stats.recordLatency(std::chrono::nanoseconds(500));

    EXPECT_EQ(stats.maxLatency(), std::chrono::nanoseconds(500));
}

TEST(StatsCollector, SmallerLatencyAfterLargerDoesNotDecreaseMax) {
    StatsCollector stats;

    stats.recordLatency(std::chrono::nanoseconds(1000));
    stats.recordLatency(std::chrono::nanoseconds(200));

    EXPECT_EQ(stats.maxLatency(), std::chrono::nanoseconds(1000));
}

TEST(StatsCollector, RecordsIncreasingLatenciesCorrectly) {
    StatsCollector stats;

    stats.recordLatency(std::chrono::nanoseconds(100));
    stats.recordLatency(std::chrono::nanoseconds(300));
    stats.recordLatency(std::chrono::nanoseconds(700));

    EXPECT_EQ(stats.maxLatency(), std::chrono::nanoseconds(700));
}

TEST(StatsCollector, MaxLatencyIsCorrectUnderConcurrentUpdates) {
    StatsCollector stats;
    constexpr int threadCount = 16;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([&stats, i]() {
            stats.recordLatency(std::chrono::nanoseconds(i + 1));
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(stats.maxLatency(), std::chrono::nanoseconds(threadCount));
}

TEST(StatsCollector, DeadlineMissCountStartsAtZero) {
    StatsCollector stats;

    EXPECT_EQ(stats.deadlineMissCount(), 0u);
}

TEST(StatsCollector, NoDeadlineMeansNoMisses) {
    StatsCollector stats;

    stats.recordLatency(std::chrono::seconds(10));

    EXPECT_EQ(stats.deadlineMissCount(), 0u);
}

TEST(StatsCollector, LatencyBelowDeadlineIsNotAMiss) {
    StatsCollector stats(std::chrono::milliseconds(20));

    stats.recordLatency(std::chrono::milliseconds(10));

    EXPECT_EQ(stats.deadlineMissCount(), 0u);
}

TEST(StatsCollector, LatencyEqualToDeadlineIsNotAMiss) {
    StatsCollector stats(std::chrono::milliseconds(20));

    stats.recordLatency(std::chrono::milliseconds(20));

    EXPECT_EQ(stats.deadlineMissCount(), 0u);
}

TEST(StatsCollector, LatencyAboveDeadlineIsAMiss) {
    StatsCollector stats(std::chrono::milliseconds(20));

    stats.recordLatency(std::chrono::milliseconds(25));

    EXPECT_EQ(stats.deadlineMissCount(), 1u);
}

TEST(StatsCollector, CountsOnlyLatenciesThatExceedDeadline) {
    StatsCollector stats(std::chrono::milliseconds(20));

    stats.recordLatency(std::chrono::milliseconds(10));
    stats.recordLatency(std::chrono::milliseconds(25));
    stats.recordLatency(std::chrono::milliseconds(15));
    stats.recordLatency(std::chrono::milliseconds(30));

    EXPECT_EQ(stats.deadlineMissCount(), 2u);
}

TEST(StatsCollector, SummaryListsCountsDeadlineMissesAndMaxLatency) {
    StatsCollector stats(std::chrono::milliseconds(20));

    stats.record(Status::Ok);
    stats.record(Status::Ok);
    stats.record(Status::Warning);
    stats.record(Status::Critical);
    stats.record(Status::Invalid);
    stats.recordLatency(std::chrono::milliseconds(10));
    stats.recordLatency(std::chrono::milliseconds(32));

    EXPECT_EQ(stats.summary(),
              "total=5 ok=2 warning=1 critical=1 invalid=1 deadline_misses=1 max_latency_us=32000");
}

TEST(StatsCollector, SummaryOfAnEmptyCollectorIsAllZeros) {
    StatsCollector stats;

    EXPECT_EQ(stats.summary(),
              "total=0 ok=0 warning=0 critical=0 invalid=0 deadline_misses=0 max_latency_us=0");
}
