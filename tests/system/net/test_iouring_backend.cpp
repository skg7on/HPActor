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

#include <hpactor/net/proactor/iouring_backend.hpp>

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gtest/gtest.h>

#if !defined(__linux__)
TEST(IoUringBackendTest, NotLinuxSkip) {
    GTEST_SKIP() << "io_uring tests only run on Linux";
}
#else
using namespace hpactor;
using namespace hpactor::net;

TEST(IoUringBackendTest, ConstructorDestructor) {
    IoUringBackend backend;
    // Destructs without crash
}

TEST(IoUringBackendTest, Start) {
    IoUringBackend backend;
    bool result = backend.start();
    EXPECT_TRUE(result);
}

TEST(IoUringBackendTest, RegisterBuffer) {
    IoUringBackend backend;
    backend.start();
    char buffer[64];
    int buffer_id = backend.register_buffer(buffer, sizeof(buffer));
    EXPECT_GE(buffer_id, 0);
}

TEST(IoUringBackendTest, UnregisterBuffer) {
    IoUringBackend backend;
    backend.start();
    char buffer[64];
    int buffer_id = backend.register_buffer(buffer, sizeof(buffer));
    EXPECT_GE(buffer_id, 0);
    bool result = backend.unregister_buffer(buffer_id);
    EXPECT_FALSE(result); // liburing doesn't support individual buffer
                          // unregistration
}

TEST(IoUringBackendTest, AddFdUpdateFdRemoveFd) {
    IoUringBackend backend;
    backend.start();

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);

    bool added = backend.add_fd(fds[0], IoEvent::Read);
    EXPECT_TRUE(added);

    bool updated = backend.update_fd(fds[0], IoEvent::Write);
    EXPECT_TRUE(updated);

    bool removed = backend.remove_fd(fds[0]);
    EXPECT_TRUE(removed);

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(IoUringBackendTest, RunAfter) {
    IoUringBackend backend;
    backend.start();

    uint64_t handle = backend.run_after(ActorId(1), 100);
    EXPECT_GT(handle, 0u);
}

TEST(IoUringBackendTest, AsyncAccept) {
    IoUringBackend backend;
    backend.start();

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    int r = ::bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT_EQ(r, 0);

    socklen_t addrlen = sizeof(addr);
    r = ::getsockname(listen_fd, (struct sockaddr*)&addr, &addrlen);
    ASSERT_EQ(r, 0);

    r = ::listen(listen_fd, 5);
    ASSERT_EQ(r, 0);

    bool added = backend.add_fd(listen_fd, IoEvent::Read);
    ASSERT_TRUE(added);

    backend.async_accept(listen_fd, ActorId(1));

    ::close(listen_fd);
}

TEST(IoUringBackendTest, AsyncConnect) {
    IoUringBackend backend;
    backend.start();

    int fds[2];
    int r = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    ASSERT_EQ(r, 0);

    backend.async_connect(fds[0], nullptr, 0, ActorId(1));

    ::close(fds[0]);
    ::close(fds[1]);
}
#endif
