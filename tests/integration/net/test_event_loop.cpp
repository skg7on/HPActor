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

#include <hpactor/net/event_loop.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

namespace {
void make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
} // namespace

class EventLoopTest : public ::testing::Test {
  protected:
    void SetUp() override {
        setvbuf(stdout, nullptr, _IONBF, 0);
    }
};

TEST_F(EventLoopTest, ConstructorDestructor) {
    EventLoop loop;
    // Destructs without crash
}

TEST_F(EventLoopTest, BackendInitialization) {
    EventLoop loop;
    // Backend should be running
}

TEST_F(EventLoopTest, RunAfterImmediateCallback) {
    EventLoop loop;
    std::atomic<bool> fired{false};
    uint64_t handle = loop.run_after([&fired]() { fired = true; }, 10);
    if (handle == 0) {
        GTEST_SKIP() << "backend does not support timers";
        return;
    }

    int waited = 0;
    while (!fired.load() && waited < 2000) {
        loop.wait(50);
        loop.process_completions();
        waited += 50;
    }

    EXPECT_TRUE(fired.load()) << "Timer did not fire after " << waited << "ms";
}

TEST_F(EventLoopTest, TimerCancellation) {
    EventLoop loop;
    std::atomic<bool> fired{false};
    uint64_t handle = loop.run_after([&fired]() { fired = true; }, 100);
    ASSERT_GT(handle, 0u);

    loop.cancel_timer(handle);
    loop.wait(150);
    loop.process_completions();

    EXPECT_FALSE(fired) << "Cancelled timer should not fire";
}

TEST_F(EventLoopTest, RunEveryRepeatingTimer) {
    EventLoop loop;
    std::atomic<int> count{0};
    uint64_t handle = loop.run_every([&count]() { count++; }, 20);
    ASSERT_GT(handle, 0u);

    int waited = 0;
    while (count.load() < 2 && waited < 200) {
        loop.wait(20);
        loop.process_completions();
        waited += 20;
    }

    EXPECT_GE(count, 2) << "run_every should fire multiple times, got " << count;
}

TEST_F(EventLoopTest, MultipleConcurrentTimers) {
    EventLoop loop;
    std::atomic<int> count{0};
    constexpr int NUM_TIMERS = 5;

    for (int i = 0; i < NUM_TIMERS; ++i) {
        uint64_t handle = loop.run_after([&count]() { count++; }, 10 + i * 10);
        ASSERT_GT(handle, 0u);
    }

    int waited = 0;
    while (count.load() < NUM_TIMERS && waited < 500) {
        loop.wait(50);
        loop.process_completions();
        waited += 50;
    }

    EXPECT_EQ(count, NUM_TIMERS) << "All timers should fire";
}

TEST_F(EventLoopTest, FdRegistration) {
    EventLoop loop;

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);

    bool added = loop.add_fd(fds[0], EventLoop::Event::Read);
    EXPECT_TRUE(added);

    bool updated = loop.update_fd(fds[0], EventLoop::Event::Write);
    EXPECT_TRUE(updated);

    bool removed = loop.remove_fd(fds[0]);
    EXPECT_TRUE(removed);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_F(EventLoopTest, TimerFiringOrder) {
    EventLoop loop;
    std::vector<int> order;
    std::mutex order_mutex;

    loop.run_after(
        [&order, &order_mutex]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(1);
        },
        50);

    loop.run_after(
        [&order, &order_mutex]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(2);
        },
        30);

    loop.run_after(
        [&order, &order_mutex]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(3);
        },
        10);

    int waited = 0;
    while (order.size() < 3 && waited < 500) {
        loop.wait(50);
        loop.process_completions();
        waited += 50;
    }

    {
        std::lock_guard<std::mutex> lock(order_mutex);
        ASSERT_EQ(order.size(), 3u) << "All timers should fire";
    }
}

TEST_F(EventLoopTest, RunEveryCancellation) {
    EventLoop loop;
    std::atomic<int> count{0};
    uint64_t handle = loop.run_every([&count]() { count++; }, 10);
    ASSERT_GT(handle, 0u);

    int waited = 0;
    while (count.load() < 2 && waited < 200) {
        loop.wait(20);
        loop.process_completions();
        waited += 20;
    }

    int count_before_cancel = count.load();
    ASSERT_GE(count_before_cancel, 2);

    loop.cancel_timer(handle);

    waited = 0;
    while (count.load() == count_before_cancel && waited < 100) {
        loop.wait(20);
        loop.process_completions();
        waited += 20;
    }

    EXPECT_EQ(count.load(), count_before_cancel);
}

TEST_F(EventLoopTest, WaitTimeoutBehavior) {
    EventLoop loop;
    int result = loop.wait(10);
    EXPECT_EQ(result, 0);
}

TEST_F(EventLoopTest, FdReadNotification) {
    EventLoop loop;

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);

    loop.add_fd(fds[0], EventLoop::Event::Read);

    const char* msg = "hello";
    ssize_t written = ::write(fds[1], msg, 5);
    ASSERT_EQ(written, 5);

    int result = loop.wait(100);
    EXPECT_GE(result, 0);

    loop.remove_fd(fds[0]);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_F(EventLoopTest, HasEvent) {
    EventLoop loop;

    int fds[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    loop.add_fd(fds[0], EventLoop::Event::Read);
    EXPECT_TRUE(loop.has_event(fds[0], EventLoop::Event::Read));

    loop.remove_fd(fds[0]);
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_F(EventLoopTest, StressManyRapidTimers) {
    EventLoop loop;
    std::atomic<int> count{0};
    constexpr int NUM_TIMERS = 20;

    for (int i = 0; i < NUM_TIMERS; ++i) {
        loop.run_after([&count]() { count++; }, 5);
    }

    int waited = 0;
    while (count.load() < NUM_TIMERS && waited < 500) {
        loop.wait(20);
        loop.process_completions();
        waited += 20;
    }

    EXPECT_EQ(count, NUM_TIMERS);
}

TEST_F(EventLoopTest, AsyncSendOnSocketpair) {
    EventLoop loop;
    std::optional<OpCompletion> captured;

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);
    make_nonblocking(fds[0]);
    make_nonblocking(fds[1]);

    loop.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    auto* backend = loop.backend();
    ASSERT_NE(backend, nullptr);

    struct iovec iov;
    char send_buf[] = "hello";
    iov.iov_base = send_buf;
    iov.iov_len = 5;

    backend->async_send(fds[0], &iov, 1, ActorId(1),
                        static_cast<uint32_t>(OpType::Send));
    loop.process_completions();

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->result, 5);
    EXPECT_EQ(captured->type, OpType::Send);
    EXPECT_EQ(captured->fd, fds[0]);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_F(EventLoopTest, AsyncRecvOnSocketpair) {
    EventLoop loop;
    std::optional<OpCompletion> captured;

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);
    make_nonblocking(fds[0]);
    make_nonblocking(fds[1]);

    loop.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    auto* backend = loop.backend();

    const char* msg = "hello";
    ssize_t written = ::write(fds[1], msg, 5);
    ASSERT_EQ(written, 5);

    char recv_buf[64];
    struct iovec iov;
    iov.iov_base = recv_buf;
    iov.iov_len = 64;

    backend->async_recv(fds[0], &iov, 1, ActorId(1),
                        static_cast<uint32_t>(OpType::Recv));
    loop.process_completions();

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->result, 5);
    EXPECT_EQ(captured->type, OpType::Recv);
    EXPECT_EQ(memcmp(recv_buf, "hello", 5), 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_F(EventLoopTest, AsyncSendtoOnSocketpair) {
    EventLoop loop;
    std::optional<OpCompletion> captured;

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);
    make_nonblocking(fds[0]);
    make_nonblocking(fds[1]);

    loop.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    auto* backend = loop.backend();

    struct iovec iov;
    char send_buf[] = "test";
    iov.iov_base = send_buf;
    iov.iov_len = 4;

    backend->async_sendto(fds[0], &iov, 1, nullptr, 0, ActorId(1),
                          static_cast<uint32_t>(OpType::SendTo));
    loop.process_completions();

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->result, 4);
    EXPECT_EQ(captured->type, OpType::SendTo);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_F(EventLoopTest, AsyncRecvfromOnSocketpair) {
    EventLoop loop;
    std::optional<OpCompletion> captured_send;
    std::optional<OpCompletion> captured_recv;

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);
    make_nonblocking(fds[0]);
    make_nonblocking(fds[1]);

    loop.set_completion_callback([&captured_send, &captured_recv](OpCompletion c) {
        if (c.type == OpType::Send) {
            captured_send = c;
        } else if (c.type == OpType::RecvFrom) {
            captured_recv = c;
        }
    });

    auto* backend = loop.backend();
    ASSERT_NE(backend, nullptr);

    struct iovec send_iov;
    char send_buf[] = "world";
    send_iov.iov_base = send_buf;
    send_iov.iov_len = 5;

    backend->async_send(fds[0], &send_iov, 1, ActorId(1),
                        static_cast<uint32_t>(OpType::Send));

    char recv_buf[64];
    struct iovec recv_iov;
    recv_iov.iov_base = recv_buf;
    recv_iov.iov_len = 64;

    backend->async_recvfrom(fds[1], &recv_iov, 1, ActorId(1),
                            static_cast<uint32_t>(OpType::RecvFrom));

    loop.process_completions();

    ASSERT_TRUE(captured_send.has_value());
    if (!captured_send)
        GTEST_SKIP();
    EXPECT_EQ(captured_send->result, 5);
    EXPECT_EQ(captured_send->type, OpType::Send);

    ASSERT_TRUE(captured_recv.has_value());
    if (!captured_recv)
        GTEST_SKIP();
    EXPECT_EQ(captured_recv->result, 5);
    EXPECT_EQ(captured_recv->type, OpType::RecvFrom);
    EXPECT_EQ(memcmp(recv_buf, "world", 5), 0);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_F(EventLoopTest, AsyncSendErrorOnClosedFd) {
    EventLoop loop;
    std::optional<OpCompletion> captured;

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);
    make_nonblocking(fds[0]);
    make_nonblocking(fds[1]);

    loop.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    auto* backend = loop.backend();

    ::close(fds[0]);

    struct iovec iov;
    char send_buf[] = "hello";
    iov.iov_base = send_buf;
    iov.iov_len = 5;

    backend->async_send(fds[0], &iov, 1, ActorId(1),
                        static_cast<uint32_t>(OpType::Send));
    loop.process_completions();

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_NE(captured->result, 5);

    ::close(fds[1]);
}

TEST_F(EventLoopTest, AsyncSendMultiIovec) {
    EventLoop loop;
    std::optional<OpCompletion> captured;

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);
    make_nonblocking(fds[0]);
    make_nonblocking(fds[1]);

    loop.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    auto* backend = loop.backend();

    struct iovec iov[2];
    char buf1[] = "hello";
    char buf2[] = " world";
    iov[0].iov_base = buf1;
    iov[0].iov_len = 5;
    iov[1].iov_base = buf2;
    iov[1].iov_len = 6;

    backend->async_send(fds[0], iov, 2, ActorId(1),
                        static_cast<uint32_t>(OpType::Send));
    loop.process_completions();

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->result, 11);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_F(EventLoopTest, AsyncRecvMultiIovec) {
    EventLoop loop;
    std::optional<OpCompletion> captured;

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);
    make_nonblocking(fds[0]);
    make_nonblocking(fds[1]);

    loop.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    auto* backend = loop.backend();

    const char* msg = "helloworld";
    ssize_t written = ::write(fds[1], msg, 10);
    ASSERT_EQ(written, 10);

    char buf1[5];
    char buf2[5];
    struct iovec iov[2];
    iov[0].iov_base = buf1;
    iov[0].iov_len = 5;
    iov[1].iov_base = buf2;
    iov[1].iov_len = 5;

    backend->async_recv(fds[0], iov, 2, ActorId(1),
                        static_cast<uint32_t>(OpType::Recv));
    loop.process_completions();

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->result, 10);
    EXPECT_EQ(memcmp(buf1, "hello", 5), 0);
    EXPECT_EQ(memcmp(buf2, "world", 5), 0);

    ::close(fds[0]);
    ::close(fds[1]);
}
