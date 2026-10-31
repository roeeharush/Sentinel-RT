#include "common/pipeline/MeasurementProcessor.h"

namespace {

    LogLevel levelForStatus(Status status) {
        switch (status) {
            case Status::Warning:
                return LogLevel::Warning;
            case Status::Critical:
            case Status::Invalid:
                return LogLevel::Error;
            case Status::Ok:
                return LogLevel::Info;
        }
        return LogLevel::Info;
    }

}

MeasurementProcessor::MeasurementProcessor(MeasurementJudge& judge, StatsCollector& stats,TimeoutMonitor& timeoutMonitor, Logger& logger)
    : judge_(judge), stats_(stats), timeoutMonitor_(timeoutMonitor), logger_(logger) {}

Status MeasurementProcessor::process(const Measurement& measurement, std::chrono::steady_clock::time_point now) {
    const std::chrono::steady_clock::time_point processingStart = std::chrono::steady_clock::now();
    const std::chrono::nanoseconds waitTime = std::chrono::duration_cast<std::chrono::nanoseconds>(now - measurement.timestamp);

    timeoutMonitor_.recordArrival(measurement.deviceId, now);

    Status status = judge_.judge(measurement);

    stats_.record(status);
    if (status != Status::Ok) {
        logger_.log(levelForStatus(status), "Measurement outside normal range");
    } else {
        logger_.log(LogLevel::Info, "Measurement received");
    }

    const std::chrono::nanoseconds processingTime = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - processingStart);
    stats_.recordLatency(waitTime + processingTime);

    return status;
}