#include "common/network/ConnectionHandler.h"
#include "common/network/Codec.h"
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

    constexpr std::chrono::milliseconds kReceiveTimeout(200);

}

ConnectionHandler::ConnectionHandler(Socket socket, BoundedQueue<Measurement>& queue, Logger& logger, const std::atomic<bool>& shutdown)
    : socket_(std::move(socket)), queue_(queue), logger_(logger), shutdown_(shutdown) {
    socket_.setReceiveTimeout(kReceiveTimeout);
}

void ConnectionHandler::run() {
    while (!shutdown_) {
        std::vector<std::uint8_t> chunk;
        ReceiveResult result = socket_.receive(chunk);
        if (result == ReceiveResult::Closed) {
            logger_.log(LogLevel::Info, "Connection closed");
            return;
        }
        if (result == ReceiveResult::Timeout) {
            continue;
        }

        frameReader_.feed(chunk);

        while (true) {
            std::optional<std::vector<std::uint8_t>> frame;
            try {
                frame = frameReader_.tryExtractFrame();
            } catch (const std::exception& e) {
                logger_.log(LogLevel::Error, std::string("Closing connection after frame error: ") + e.what());
                return;
            }

            if (!frame.has_value()) {
                break;
            }

            std::optional<Measurement> measurement = decodeMeasurement(*frame);
            if (!measurement.has_value()) {
                logger_.log(LogLevel::Warning, "Discarding malformed measurement payload");
                continue;
            }

            queue_.push(*measurement);
        }
    }
    logger_.log(LogLevel::Info, "Connection closing: shutdown requested");
}
