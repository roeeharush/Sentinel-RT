#include <gtest/gtest.h>
#include "common/network/Socket.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

TEST(Socket, FdAccessorReturnsConstructedValue) {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    Socket socket(fds[0]);

    EXPECT_EQ(socket.fd(), fds[0]);
    close(fds[1]);
}

TEST(Socket, DestructorClosesFd) {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    int readFd = fds[0];

    {
        Socket socket(readFd);
    }

    char buffer[1];
    ssize_t result = read(readFd, buffer, sizeof(buffer));

    EXPECT_EQ(result, -1);
    EXPECT_EQ(errno, EBADF);
    close(fds[1]);
}

TEST(Socket, MoveConstructorTransfersOwnership) {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    Socket a(fds[0]);
    Socket b(std::move(a));

    EXPECT_EQ(b.fd(), fds[0]);
    EXPECT_EQ(a.fd(), -1);
    close(fds[1]);
}

TEST(Socket, MoveAssignmentClosesPreviousFd) {
    int fdsA[2];
    int fdsB[2];
    ASSERT_EQ(pipe(fdsA), 0);
    ASSERT_EQ(pipe(fdsB), 0);
    int oldFdOfB = fdsB[0];

    Socket a(fdsA[0]);
    Socket b(fdsB[0]);

    b = std::move(a);

    EXPECT_EQ(b.fd(), fdsA[0]);
    EXPECT_EQ(a.fd(), -1);

    char buffer[1];
    ssize_t result = read(oldFdOfB, buffer, sizeof(buffer));

    EXPECT_EQ(result, -1);
    EXPECT_EQ(errno, EBADF);

    close(fdsA[1]);
    close(fdsB[1]);
}

TEST(Socket, SendAllSendsAllBytes) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket sender(fds[0]);
    std::vector<std::uint8_t> data = {'H', 'E', 'L', 'L', 'O'};

    EXPECT_TRUE(sender.sendAll(data));

    char buffer[5] = {};
    ssize_t received = recv(fds[1], buffer, sizeof(buffer), 0);

    ASSERT_EQ(received, 5);
    EXPECT_EQ(std::memcmp(buffer, data.data(), 5), 0);

    close(fds[1]);
}

TEST(Socket, SendAllFailsGracefullyWhenPeerClosed) {
    ignoreSigPipe();

    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    close(fds[1]);

    Socket sender(fds[0]);
    std::vector<std::uint8_t> data = {'H', 'I'};

    EXPECT_FALSE(sender.sendAll(data));
}

TEST(Socket, EnableAddressReuseSetsOption) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    Socket socket(fd);
    socket.enableAddressReuse();

    int value = 0;
    socklen_t len = sizeof(value);
    ASSERT_EQ(getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, &len), 0);
    EXPECT_NE(value, 0);
}

TEST(Socket, SetReceiveTimeoutSetsOption) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    Socket socket(fd);
    socket.setReceiveTimeout(std::chrono::milliseconds(250));

    timeval tv{};
    socklen_t len = sizeof(tv);
    ASSERT_EQ(getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, &len), 0);
    EXPECT_EQ(tv.tv_sec, 0);
    EXPECT_GE(tv.tv_usec, 249000);
    EXPECT_LE(tv.tv_usec, 260000);
}

TEST(Socket, ReceiveTimesOutInsteadOfBlockingForever) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket receiver(fds[0]);
    receiver.setReceiveTimeout(std::chrono::milliseconds(50));

    char buffer[10];
    ssize_t result = recv(fds[0], buffer, sizeof(buffer), 0);

    EXPECT_EQ(result, -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

    close(fds[1]);
}

TEST(Socket, ReceiveReturnsDataWhenBytesArrive) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket receiver(fds[0]);
    ASSERT_EQ(send(fds[1], "HI", 2, 0), 2);

    std::vector<std::uint8_t> data;
    EXPECT_EQ(receiver.receive(data), ReceiveResult::Data);
    EXPECT_EQ(data, (std::vector<std::uint8_t>{'H', 'I'}));

    close(fds[1]);
}

TEST(Socket, ReceiveReportsTimeoutWhenNoDataArrives) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket receiver(fds[0]);
    receiver.setReceiveTimeout(std::chrono::milliseconds(50));

    std::vector<std::uint8_t> data;
    EXPECT_EQ(receiver.receive(data), ReceiveResult::Timeout);

    close(fds[1]);
}

TEST(Socket, ReceiveReportsClosedWhenPeerCloses) {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Socket receiver(fds[0]);
    close(fds[1]);

    std::vector<std::uint8_t> data;
    EXPECT_EQ(receiver.receive(data), ReceiveResult::Closed);
}
