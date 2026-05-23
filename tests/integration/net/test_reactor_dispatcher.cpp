// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/net/reactor_dispatcher.hpp>

#include <cerrno>
#include <cstring>
#include <optional>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

class ReactorDispatcherTest : public ::testing::Test {
  protected:
    ReactorDispatcher disp;
};

TEST_F(ReactorDispatcherTest, RegisterFdUnregisterFd) {
    disp.register_fd(42, ActorId(1));
    disp.unregister_fd(42);
    // No crash = pass
}

TEST_F(ReactorDispatcherTest, RegisterRecvAndHasPending) {
    char buf[64];
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    disp.register_recv(10, ActorId(5), OpType::Recv, &iov, 1);
    EXPECT_TRUE(disp.has_pending(10));
    disp.unregister_fd(10);
    EXPECT_FALSE(disp.has_pending(10));
}

TEST_F(ReactorDispatcherTest, RegisterSendAndHasPending) {
    struct iovec iov;
    char data[] = "hello";
    iov.iov_base = data;
    iov.iov_len = 5;
    disp.register_send(10, ActorId(5), OpType::Send, &iov, 1);
    EXPECT_TRUE(disp.has_pending(10));
    disp.unregister_io(10);
    EXPECT_FALSE(disp.has_pending(10));
}

TEST_F(ReactorDispatcherTest, OnReadinessTriggersRecv) {
    std::optional<OpCompletion> captured;
    disp.set_completion_handler([&captured](OpCompletion c) { captured = c; });

    int fds[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);

    char recv_buf[64];
    struct iovec iov;
    iov.iov_base = recv_buf;
    iov.iov_len = sizeof(recv_buf);
    disp.register_recv(fds[0], ActorId(7), OpType::Recv, &iov, 1);

    const char* msg = "hello reactor";
    ssize_t written = write(fds[1], msg, 14);
    ASSERT_EQ(written, 14);

    disp.on_readiness(fds[0], IoEvent::Read);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->type, OpType::Recv);
    EXPECT_EQ(captured->actor, ActorId(7));
    EXPECT_EQ(captured->result, 14);
    EXPECT_EQ(memcmp(recv_buf, "hello reactor", 14), 0);

    close(fds[0]);
    close(fds[1]);
}

TEST_F(ReactorDispatcherTest, OnReadinessTriggersSend) {
    std::optional<OpCompletion> captured;
    disp.set_completion_handler([&captured](OpCompletion c) { captured = c; });

    int fds[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);

    struct iovec iov;
    char send_buf[] = "world";
    iov.iov_base = send_buf;
    iov.iov_len = 5;
    disp.register_send(fds[0], ActorId(3), OpType::Send, &iov, 1);

    disp.on_readiness(fds[0], IoEvent::Write);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->type, OpType::Send);
    EXPECT_EQ(captured->actor, ActorId(3));
    EXPECT_EQ(captured->result, 5);

    char verify_buf[16] = {};
    ssize_t n = read(fds[1], verify_buf, sizeof(verify_buf));
    ASSERT_EQ(n, 5);
    EXPECT_EQ(memcmp(verify_buf, "world", 5), 0);

    close(fds[0]);
    close(fds[1]);
}

TEST_F(ReactorDispatcherTest, OnReadinessErrorOnClosedFd) {
    std::optional<OpCompletion> captured;
    disp.set_completion_handler([&captured](OpCompletion c) { captured = c; });

    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    char recv_buf[64];
    struct iovec iov;
    iov.iov_base = recv_buf;
    iov.iov_len = sizeof(recv_buf);
    disp.register_recv(fds[0], ActorId(1), OpType::Recv, &iov, 1);

    close(fds[0]);

    disp.on_readiness(fds[0], IoEvent::Read);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_LT(captured->result, 0);

    close(fds[1]);
}

TEST_F(ReactorDispatcherTest, NoOpForUnregisteredFd) {
    bool called = false;
    disp.set_completion_handler([&called](OpCompletion) { called = true; });

    disp.on_readiness(99, IoEvent::Read);
    EXPECT_FALSE(called);
}

TEST_F(ReactorDispatcherTest, UnregisterIo) {
    char buf[64];
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    disp.register_recv(20, ActorId(2), OpType::Recv, &iov, 1);
    EXPECT_TRUE(disp.has_pending(20));
    disp.unregister_io(20);
    EXPECT_FALSE(disp.has_pending(20));
}

TEST_F(ReactorDispatcherTest, AcceptReadiness) {
    std::optional<OpCompletion> captured;
    disp.set_completion_handler([&captured](OpCompletion c) { captured = c; });

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "/tmp/hpactor_test_accept_%ld", static_cast<long>(getpid()));
    unlink(addr.sun_path);

    int r =
        bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (r != 0 && (errno == EPERM || errno == EACCES)) {
        close(listen_fd);
        unlink(addr.sun_path);
        GTEST_SKIP() << "AF_UNIX bind not permitted";
        return;
    }
    ASSERT_EQ(r, 0);

    r = listen(listen_fd, 1);
    ASSERT_EQ(r, 0);

    disp.register_accept(listen_fd, ActorId(10));

    int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);
    connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    usleep(50000);

    disp.on_readiness(listen_fd, IoEvent::Read);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->type, OpType::Accept);
    EXPECT_GE(captured->result, 0);

    if (captured->result >= 0) {
        close(captured->result);
    }
    close(client_fd);
    close(listen_fd);
    unlink(addr.sun_path);
}

TEST_F(ReactorDispatcherTest, RegisterSendto) {
    std::optional<OpCompletion> captured;
    disp.set_completion_handler([&captured](OpCompletion c) { captured = c; });

    int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    int receiver = socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT_GE(sender, 0);
    ASSERT_GE(receiver, 0);

    struct sockaddr_un recv_addr;
    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sun_family = AF_UNIX;
    snprintf(recv_addr.sun_path, sizeof(recv_addr.sun_path),
             "/tmp/hpactor_sendto_test_%d", getpid());
    unlink(recv_addr.sun_path);
    ASSERT_EQ(bind(receiver, reinterpret_cast<struct sockaddr*>(&recv_addr),
                   sizeof(recv_addr)),
              0);

    struct iovec iov;
    char data[] = "sendto test";
    iov.iov_base = data;
    iov.iov_len = 10;

    disp.register_sendto(sender, ActorId(4), OpType::SendTo, &iov, 1,
                         reinterpret_cast<struct sockaddr*>(&recv_addr),
                         sizeof(recv_addr));

    disp.on_readiness(sender, IoEvent::Write);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->type, OpType::SendTo);
    EXPECT_EQ(captured->result, 10);

    close(sender);
    close(receiver);
    unlink(recv_addr.sun_path);
}

TEST_F(ReactorDispatcherTest, UnregisterIoCancelsPendingAccept) {
    bool called = false;
    disp.set_completion_handler([&called](OpCompletion) { called = true; });

    disp.register_accept(50, ActorId(99));
    disp.unregister_io(50);
    disp.on_readiness(50, IoEvent::Read);
    EXPECT_FALSE(called);
}
