#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include <chrono>

void ignoreSigPipe();
enum class ReceiveResult { Data, Timeout, Closed };


class Socket {
public:
    explicit Socket(int fd);
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    int fd() const;
    bool sendAll(const std::vector<std::uint8_t>& data);
    ReceiveResult receive(std::vector<std::uint8_t>& out, std::size_t maxBytes = 4096);
    void enableAddressReuse();
    void setReceiveTimeout(std::chrono::milliseconds timeout);

private:
    int fd_;
};