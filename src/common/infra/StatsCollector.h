#pragma once

#include "common/core/Status.h"
#include <cstdint>
#include <atomic>
#include <chrono>
#include <string>


class StatsCollector {
  public:
    explicit StatsCollector(std::chrono::nanoseconds deadline = std::chrono::nanoseconds::max());
    void record(Status status);
    void recordLatency(std::chrono::nanoseconds latency);

    [[nodiscard]] std::uint64_t okCount() const;
    [[nodiscard]] std::uint64_t warningCount() const;
    [[nodiscard]] std::uint64_t criticalCount() const;
    [[nodiscard]] std::uint64_t invalidCount() const;
    [[nodiscard]] std::uint64_t totalCount() const;
    [[nodiscard]] std::chrono::nanoseconds maxLatency() const;
    [[nodiscard]] std::uint64_t deadlineMissCount() const;
    [[nodiscard]] std::string summary() const;

private:
    std::atomic<std::uint64_t> okCount_ = 0;
    std::atomic<std::uint64_t> warningCount_ = 0;
    std::atomic<std::uint64_t> criticalCount_ = 0;
    std::atomic<std::uint64_t> invalidCount_ = 0;
    std::atomic<std::int64_t> maxLatencyNs_ = 0;
    std::chrono::nanoseconds deadline_;
    std::atomic<std::uint64_t> deadlineMissCount_ = 0;
};