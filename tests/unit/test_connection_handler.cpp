#include <gtest/gtest.h>
#include "common/network/ConnectionHandler.h"
#include "common/network/Codec.h"
#include "common/network/FrameReader.h"

#include <atomic>
#include <chrono>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

TEST(ConnectionHandler, DecodesFramedMeasurementAndPushesToQueue) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket serverSideSocket(fds[0]);
    BoundedQueue<Measurement> queue(4);
    std::ostringstream logOutput;
    Logger logger(logOutput);
    std::atomic<bool> shutdown{false};

    ConnectionHandler handler(std::move(serverSideSocket), queue, logger, shutdown);
    std::thread handlerThread([&handler]() { handler.run(); });

    Measurement sent;
    sent.deviceId = 7;
    sent.sensorType = SensorType::Voltage;
    sent.timestamp = std::chrono::steady_clock::now();
    sent.value = 24.0;

    std::vector<std::uint8_t> frame = encodeFrame(encodeMeasurement(sent));
    ASSERT_EQ(send(fds[1], frame.data(), frame.size(), 0), static_cast<ssize_t>(frame.size()));

    std::optional<Measurement> received = queue.pop();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->deviceId, 7u);
    EXPECT_EQ(received->sensorType, SensorType::Voltage);
    EXPECT_DOUBLE_EQ(received->value, 24.0);

    close(fds[1]);
    handlerThread.join();
}

TEST(ConnectionHandler, MalformedPayloadIsSkippedWithoutClosingConnection) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket serverSideSocket(fds[0]);
    BoundedQueue<Measurement> queue(4);
    std::ostringstream logOutput;
    Logger logger(logOutput);
    std::atomic<bool> shutdown{false};

    ConnectionHandler handler(std::move(serverSideSocket), queue, logger, shutdown);
    std::thread handlerThread([&handler]() { handler.run(); });

    std::vector<std::uint8_t> badFrame = encodeFrame(std::vector<std::uint8_t>{'x', 'x'});
    ASSERT_EQ(send(fds[1], badFrame.data(), badFrame.size(), 0), static_cast<ssize_t>(badFrame.size()));

    Measurement sent;
    sent.deviceId = 3;
    sent.sensorType = SensorType::Pressure;
    sent.timestamp = std::chrono::steady_clock::now();
    sent.value = 101.3;
    std::vector<std::uint8_t> goodFrame = encodeFrame(encodeMeasurement(sent));
    ASSERT_EQ(send(fds[1], goodFrame.data(), goodFrame.size(), 0), static_cast<ssize_t>(goodFrame.size()));

    std::optional<Measurement> received = queue.pop();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->deviceId, 3u);

    close(fds[1]);
    handlerThread.join();
}

TEST(ConnectionHandler, DrainsMultipleFramesReceivedInOneChunk) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket serverSideSocket(fds[0]);
    BoundedQueue<Measurement> queue(4);
    std::ostringstream logOutput;
    Logger logger(logOutput);
    std::atomic<bool> shutdown{false};

    ConnectionHandler handler(std::move(serverSideSocket), queue, logger, shutdown);
    std::thread handlerThread([&handler]() { handler.run(); });

    Measurement first;
    first.deviceId = 1;
    first.sensorType = SensorType::Temperature;
    first.timestamp = std::chrono::steady_clock::now();
    first.value = 55.0;

    Measurement second;
    second.deviceId = 2;
    second.sensorType = SensorType::Temperature;
    second.timestamp = std::chrono::steady_clock::now();
    second.value = 60.0;

    std::vector<std::uint8_t> combined = encodeFrame(encodeMeasurement(first));
    std::vector<std::uint8_t> secondFrame = encodeFrame(encodeMeasurement(second));
    combined.insert(combined.end(), secondFrame.begin(), secondFrame.end());
    ASSERT_EQ(send(fds[1], combined.data(), combined.size(), 0), static_cast<ssize_t>(combined.size()));

    std::optional<Measurement> receivedFirst = queue.pop();
    std::optional<Measurement> receivedSecond = queue.pop();

    ASSERT_TRUE(receivedFirst.has_value());
    ASSERT_TRUE(receivedSecond.has_value());
    EXPECT_EQ(receivedFirst->deviceId, 1u);
    EXPECT_EQ(receivedSecond->deviceId, 2u);

    close(fds[1]);
    handlerThread.join();
}

TEST(ConnectionHandler, StopsWhenShutdownRequestedEvenIfClientStaysSilent) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket serverSideSocket(fds[0]);
    BoundedQueue<Measurement> queue(4);
    std::ostringstream logOutput;
    Logger logger(logOutput);
    std::atomic<bool> shutdown{false};
    std::atomic<bool> finished{false};

    ConnectionHandler handler(std::move(serverSideSocket), queue, logger, shutdown);
    std::thread handlerThread([&]() {
        handler.run();
        finished = true;
    });

    shutdown = true;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool stoppedWhileClientWasStillConnected = finished;

    close(fds[1]);
    handlerThread.join();

    EXPECT_TRUE(stoppedWhileClientWasStillConnected);
}

TEST(ConnectionHandler, SilenceLongerThanTheReceiveTimeoutDoesNotCloseTheConnection) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket serverSideSocket(fds[0]);
    BoundedQueue<Measurement> queue(4);
    std::ostringstream logOutput;
    Logger logger(logOutput);
    std::atomic<bool> shutdown{false};
    std::atomic<bool> finished{false};

    ConnectionHandler handler(std::move(serverSideSocket), queue, logger, shutdown);
    std::thread handlerThread([&]() {
        handler.run();
        finished = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    bool stillRunningAfterSilence = !finished;

    std::optional<Measurement> received;
    if (stillRunningAfterSilence) {
        Measurement sent;
        sent.deviceId = 9;
        sent.sensorType = SensorType::Voltage;
        sent.timestamp = std::chrono::steady_clock::now();
        sent.value = 24.0;
        std::vector<std::uint8_t> frame = encodeFrame(encodeMeasurement(sent));
        send(fds[1], frame.data(), frame.size(), 0);
        received = queue.pop();
    }

    close(fds[1]);
    handlerThread.join();

    EXPECT_TRUE(stillRunningAfterSilence);
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->deviceId, 9u);
}
