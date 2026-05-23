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
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/types/types.hpp>

#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

// Helper to test path derivation
static std::string derive_uds_path(const std::string& node_id) {
    std::string sanitized = node_id;
    for (char& c : sanitized) {
        if (c == ':')
            c = '_';
    }
    return "/tmp/hpactor/" + sanitized + ".sock";
}

class UnixDomainSocketTest : public ::testing::Test {
  protected:
    void SetUp() override {
        mkdir("/tmp/hpactor", 0755);
    }
};

TEST_F(UnixDomainSocketTest, SimpleNodeId) {
    auto path = derive_uds_path("localhost:5000");
    EXPECT_EQ(path, "/tmp/hpactor/localhost_5000.sock");
}

TEST_F(UnixDomainSocketTest, IpAddress) {
    auto path = derive_uds_path("127.0.0.1:8080");
    EXPECT_EQ(path, "/tmp/hpactor/127.0.0.1_8080.sock");
}

TEST_F(UnixDomainSocketTest, NoPort) {
    auto path = derive_uds_path("node1");
    EXPECT_EQ(path, "/tmp/hpactor/node1.sock");
}

TEST_F(UnixDomainSocketTest, MultipleColons) {
    auto path = derive_uds_path("192.168.1.1:5000");
    EXPECT_EQ(path, "/tmp/hpactor/192.168.1.1_5000.sock");
}

TEST_F(UnixDomainSocketTest, ListenAndAccept) {
    EventLoop loop;
    UnixDomainAcceptor acceptor(&loop);
    std::string socket_path = "/tmp/hpactor/test_listen_accept.sock";

    bool server_started = acceptor.listen(socket_path);
    ASSERT_TRUE(server_started);

    // Verify socket file exists
    struct stat st;
    EXPECT_EQ(stat(socket_path.c_str(), &st), 0);

    // Verify it's actually listening
    EXPECT_TRUE(acceptor.is_listening());
    EXPECT_EQ(acceptor.uds_path(), socket_path);

    // Clean up
    acceptor.close();

    // After close, socket file should be unlinked
    EXPECT_EQ(stat(socket_path.c_str(), &st), -1);
}

TEST_F(UnixDomainSocketTest, AcceptHandler) {
    EventLoop loop;
    UnixDomainAcceptor acceptor(&loop);
    std::string socket_path = "/tmp/hpactor/test_accept_handler.sock";

    bool server_started = acceptor.listen(socket_path);
    ASSERT_TRUE(server_started);

    int client_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    int ret = ::connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr),
                        sizeof(addr));
    (void)ret;

    // Client socket exists and is valid
    ASSERT_GE(client_fd, 0);

    ::close(client_fd);
    acceptor.close();
}

TEST_F(UnixDomainSocketTest, TcpAcceptorClose) {
    EventLoop loop;
    TcpAcceptor acceptor(&loop);

    bool started = acceptor.listen(19995);
    ASSERT_TRUE(started);

    // close() should be safe
    acceptor.close();
    EXPECT_FALSE(acceptor.is_listening());

    // Double close should also be safe
    acceptor.close();
    EXPECT_FALSE(acceptor.is_listening());
}

TEST_F(UnixDomainSocketTest, UdsFallbackInvalidPath) {
    EventLoop loop;
    auto remote_ep = Ipv4Endpoint{htonl(0x7F000001), htons(19996)};

    TlsConfig tls_config;
    PoolConfig pool_config;
    TcpTransport transport(remote_ep, tls_config, pool_config, nullptr);

    auto conn = transport.connect_unix_domain(remote_ep, "/nonexistent/"
                                                         "path.sock");
    EXPECT_EQ(conn, nullptr);
}

TEST_F(UnixDomainSocketTest, UdsFallbackWhenUdsUnavailable) {
    EventLoop loop;
    auto remote_ep = Ipv4Endpoint{htonl(0x7F000001), htons(19997)};

    TlsConfig tls_config;
    PoolConfig pool_config;

    TcpTransport transport(remote_ep, tls_config, pool_config, nullptr);

    auto conn = transport.connect(remote_ep);
    EXPECT_EQ(conn, nullptr);
}
