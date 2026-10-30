#include "common/pipeline/BoundedQueue.h"
#include "common/network/ConnectionHandler.h"
#include "common/infra/Logger.h"
#include "common/core/Measurement.h"
#include "common/network/Socket.h"
#include "common/detection/MeasurementJudge.h"
#include "common/infra/StatsCollector.h"
#include "common/detection/ThresholdDetector.h"
#include "common/detection/TimeoutMonitor.h"
#include "common/pipeline/MeasurementProcessor.h"
#include "common/pipeline/WorkerPool.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#include <vector>
#include <atomic>
#include <csignal>
#include <cerrno>

namespace {

    std::atomic<bool> g_shutdownRequested{false};
    void handleShutdownSignal(int) {
        g_shutdownRequested = true;
    }
    constexpr std::size_t kQueueCapacity = 1024;
    constexpr std::size_t kWorkerCount = 4;
    constexpr int kListenBacklog = 10;
    constexpr std::chrono::milliseconds kTimeoutThreshold(2000);
    constexpr std::chrono::milliseconds kTimeoutCheckInterval(500);
    constexpr std::chrono::milliseconds kAcceptTimeout(200);

}

int main() {
    ignoreSigPipe();
    std::signal(SIGINT, handleShutdownSignal);
    std::signal(SIGTERM, handleShutdownSignal);

    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    Socket listenSocket(listenFd);
    listenSocket.setReceiveTimeout(kAcceptTimeout);

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(5000);

    if (::bind(listenSocket.fd(), reinterpret_cast<sockaddr*>(&serverAddress), sizeof(serverAddress)) < 0) {
        std::cerr << "Failed to bind\n";
        return 1;
    }

    std::cout << "Bound to port 5000\n";

    if (::listen(listenSocket.fd(), kListenBacklog) < 0) {
        std::cerr << "Failed to listen\n";
        return 1;
    }

    std::cout << "Listening on port 5000...\n";

    Logger logger;
    StatsCollector stats(std::chrono::milliseconds(20));
    BoundedQueue<Measurement> queue(kQueueCapacity);
    MeasurementJudge judge(defaultThresholds(), kDefaultWindowCapacity);
    TimeoutMonitor timeoutMonitor(kTimeoutThreshold);
    MeasurementProcessor processor(judge, stats, timeoutMonitor, logger);

    WorkerPool<Measurement> pool(queue, logger, [&](Measurement measurement) {
        processor.process(measurement, std::chrono::steady_clock::now());
    }, kWorkerCount);

    std::thread timeoutThread([&timeoutMonitor, &logger]() {
    while (!g_shutdownRequested) {
        std::this_thread::sleep_for(kTimeoutCheckInterval);
        std::vector<std::uint32_t> timedOutDevices = timeoutMonitor.checkTimeouts(std::chrono::steady_clock::now());
        for (std::uint32_t deviceId : timedOutDevices) {
            logger.log(LogLevel::Error, "Device " + std::to_string(deviceId) + " timed out");
        }
    }
});
    std::vector<std::thread> connectionThreads;
    std::cout << "Waiting for clients to connect...\n";

    while (!g_shutdownRequested) {
        int clientFd = ::accept(listenSocket.fd(), nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            logger.log(LogLevel::Error, "Failed to accept connection");
            continue;
        }

        Socket clientSocket(clientFd);
        logger.log(LogLevel::Info, "Client connected");

        connectionThreads.emplace_back([socket = std::move(clientSocket), &queue, &logger]() mutable {
    ConnectionHandler handler(std::move(socket), queue, logger, g_shutdownRequested);
    handler.run();
    });
    }
    for (std::thread& thread : connectionThreads) {
        thread.join();
    }
    timeoutThread.join();
    pool.stop();
    std::cout << "Server stopped cleanly\n";
}