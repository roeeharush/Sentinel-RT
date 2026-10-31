#include <gtest/gtest.h>
#include "common/pipeline/MeasurementProcessor.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Rig {
    std::ostringstream logOutput;
    Logger logger{logOutput};
    MeasurementJudge judge{defaultThresholds(), kDefaultWindowCapacity};
    StatsCollector stats{std::chrono::milliseconds(20)};
    TimeoutMonitor timeoutMonitor{std::chrono::milliseconds(2000)};
    MeasurementProcessor processor{judge, stats, timeoutMonitor, logger};
};

Clock::time_point baseTime() {
    return Clock::time_point{} + std::chrono::seconds(100);
}

Measurement makeMeasurement(std::uint32_t deviceId, double value, Clock::time_point timestamp) {
    Measurement measurement;
    measurement.deviceId = deviceId;
    measurement.sensorType = SensorType::Temperature;
    measurement.timestamp = timestamp;
    measurement.value = value;
    return measurement;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}

TEST(MeasurementProcessor, HealthyMeasurementReturnsOkAndIsCounted) {
    Rig rig;

    Status status = rig.processor.process(makeMeasurement(1, 55.0, baseTime()), baseTime());

    EXPECT_EQ(status, Status::Ok);
    EXPECT_EQ(rig.stats.okCount(), 1u);
    EXPECT_EQ(rig.stats.totalCount(), 1u);
}

TEST(MeasurementProcessor, WarningMeasurementReturnsWarningAndIsCounted) {
    Rig rig;

    Status status = rig.processor.process(makeMeasurement(1, 75.0, baseTime()), baseTime());

    EXPECT_EQ(status, Status::Warning);
    EXPECT_EQ(rig.stats.warningCount(), 1u);
    EXPECT_EQ(rig.stats.totalCount(), 1u);
}

TEST(MeasurementProcessor, CriticalMeasurementReturnsCriticalAndIsCounted) {
    Rig rig;

    Status status = rig.processor.process(makeMeasurement(1, 90.0, baseTime()), baseTime());

    EXPECT_EQ(status, Status::Critical);
    EXPECT_EQ(rig.stats.criticalCount(), 1u);
    EXPECT_EQ(rig.stats.totalCount(), 1u);
}

TEST(MeasurementProcessor, InvalidMeasurementReturnsInvalidAndIsCounted) {
    Rig rig;
    double nan = std::numeric_limits<double>::quiet_NaN();

    Status status = rig.processor.process(makeMeasurement(1, nan, baseTime()), baseTime());

    EXPECT_EQ(status, Status::Invalid);
    EXPECT_EQ(rig.stats.invalidCount(), 1u);
    EXPECT_EQ(rig.stats.totalCount(), 1u);
}

TEST(MeasurementProcessor, LatencyIncludesTheWaitBetweenTimestampAndNow) {
    Rig rig;
    Clock::time_point now = baseTime() + std::chrono::milliseconds(50);

    rig.processor.process(makeMeasurement(1, 55.0, baseTime()), now);

    EXPECT_GE(rig.stats.maxLatency(), std::chrono::milliseconds(50));
    EXPECT_LT(rig.stats.maxLatency(), std::chrono::milliseconds(60));
}

TEST(MeasurementProcessor, CountsADeadlineMissOnlyWhenLatencyExceedsTheDeadline) {
    Rig rig;

    rig.processor.process(makeMeasurement(1, 55.0, baseTime()), baseTime() + std::chrono::milliseconds(50));
    EXPECT_EQ(rig.stats.deadlineMissCount(), 1u);

    rig.processor.process(makeMeasurement(1, 56.0, baseTime()), baseTime() + std::chrono::milliseconds(5));
    EXPECT_EQ(rig.stats.deadlineMissCount(), 1u);
}

TEST(MeasurementProcessor, RegistersTheArrivalWithTheTimeMeasuredNowNotTheTimestamp) {
    Rig rig;
    Clock::time_point now = baseTime() + std::chrono::milliseconds(50);

    rig.processor.process(makeMeasurement(7, 55.0, baseTime()), now);

    EXPECT_TRUE(rig.timeoutMonitor.checkTimeouts(now + std::chrono::milliseconds(1999)).empty());
    std::vector<std::uint32_t> timedOut = rig.timeoutMonitor.checkTimeouts(now + std::chrono::milliseconds(2001));
    ASSERT_EQ(timedOut.size(), 1u);
    EXPECT_EQ(timedOut[0], 7u);
}

TEST(MeasurementProcessor, LogsAnInfoLineForAHealthyMeasurement) {
    Rig rig;

    rig.processor.process(makeMeasurement(1, 55.0, baseTime()), baseTime());

    EXPECT_TRUE(contains(rig.logOutput.str(), "INFO: Measurement received"));
}

TEST(MeasurementProcessor, LogsAWarningLineForAWarningMeasurement) {
    Rig rig;

    rig.processor.process(makeMeasurement(1, 75.0, baseTime()), baseTime());

    EXPECT_TRUE(contains(rig.logOutput.str(), "WARNING: Measurement outside normal range"));
}

TEST(MeasurementProcessor, LogsAnErrorLineForACriticalMeasurement) {
    Rig rig;

    rig.processor.process(makeMeasurement(1, 90.0, baseTime()), baseTime());

    EXPECT_TRUE(contains(rig.logOutput.str(), "ERROR: Measurement outside normal range"));
}

TEST(MeasurementProcessor, LogsAnErrorLineForAnInvalidMeasurement) {
    Rig rig;
    double nan = std::numeric_limits<double>::quiet_NaN();

    rig.processor.process(makeMeasurement(1, nan, baseTime()), baseTime());

    EXPECT_TRUE(contains(rig.logOutput.str(), "ERROR: Measurement outside normal range"));
}

TEST(MeasurementProcessor, JudgesEachMeasurementExactlyOnce) {
    Rig rig;

    for (std::size_t i = 1; i < kDefaultWindowCapacity; ++i) {
        EXPECT_EQ(rig.processor.process(makeMeasurement(1, 55.0, baseTime()), baseTime()), Status::Ok)
            << "reading number " << i;
    }

    EXPECT_EQ(rig.processor.process(makeMeasurement(1, 55.0, baseTime()), baseTime()), Status::Critical);
}

TEST(MeasurementProcessor, ManyThreadsProcessingTogetherCountEveryMeasurement) {
    Rig rig;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 2000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&rig, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                Clock::time_point now = Clock::now();
                double value = 55.0 + static_cast<double>(i % 7) * 0.1;
                rig.processor.process(makeMeasurement(static_cast<std::uint32_t>(t + 1), value, now), now);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(rig.stats.totalCount(), static_cast<std::uint64_t>(kThreads) * kPerThread);
}

TEST(MeasurementProcessor, ProcessingTimeCountsTowardsTheLatencyAndTheDeadline) {
    struct SlowBuffer : std::stringbuf {
        std::streamsize xsputn(const char* text, std::streamsize count) override {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return std::stringbuf::xsputn(text, count);
        }
    };

    SlowBuffer slowBuffer;
    std::ostream slowOutput(&slowBuffer);
    Logger logger{slowOutput};
    MeasurementJudge judge{defaultThresholds(), kDefaultWindowCapacity};
    StatsCollector stats{std::chrono::milliseconds(20)};
    TimeoutMonitor timeoutMonitor{std::chrono::milliseconds(2000)};
    MeasurementProcessor processor{judge, stats, timeoutMonitor, logger};

    processor.process(makeMeasurement(1, 55.0, baseTime()), baseTime());

    EXPECT_GT(stats.maxLatency(), std::chrono::milliseconds(20));
    EXPECT_EQ(stats.deadlineMissCount(), 1u);
}
