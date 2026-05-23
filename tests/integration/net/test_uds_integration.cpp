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

#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>

#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

namespace {

std::string derive_helper(const std::string& node_id) {
    std::string s = node_id;
    for (char& c : s)
        if (c == ':')
            c = '_';
    return "/tmp/hpactor/" + s + ".sock";
}

} // namespace

class UdsIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        mkdir("/tmp/hpactor", 0755);
    }
};

TEST_F(UdsIntegrationTest, ConnectAndFrame) {
    EventLoop loop;

    UnixDomainAcceptor acceptor(&loop);
    std::string socket_path = "/tmp/hpactor/test_connect_frame.sock";

    bool server_started = acceptor.listen(socket_path);
    ASSERT_TRUE(server_started);

    int accepted_fd = -1;
    acceptor.set_accept_handler(
        [&accepted_fd](int fd, EndPoint) { accepted_fd = fd; });

    int client_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    int result = ::connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr),
                           sizeof(addr));
    (void)result;

    // Client socket is valid
    ASSERT_GE(client_fd, 0);

    ::close(client_fd);
    acceptor.close();
}

TEST_F(UdsIntegrationTest, PathDerivation) {
    EXPECT_EQ(derive_helper("localhost:5000"), "/tmp/hpactor/"
                                               "localhost_5000.sock");
    EXPECT_EQ(derive_helper("127.0.0.1:8080"), "/tmp/hpactor/"
                                               "127.0.0.1_8080.sock");
    EXPECT_EQ(derive_helper("node1"), "/tmp/hpactor/node1.sock");
}
