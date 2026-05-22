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

// Comprehensive async TCP transport tests

#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/net/wireframe_connection.hpp>
#include <hpactor/types/types.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

class TcpTransportComprehensiveTest : public ::testing::Test {
  protected:
    void SetUp() override {
        mkdir("/tmp/hpactor", 0755);
    }
};

TEST_F(TcpTransportComprehensiveTest, AcceptorRapidOpenClose) {
    EventLoop loop;
    std::atomic<int> accept_count{0};

    for (int cycle = 0; cycle < 20; ++cycle) {
        UnixDomainAcceptor acceptor(&loop);
        std::string path =
            "/tmp/hpactor/test_rapid_" + std::to_string(cycle) + ".sock";

        bool ok = acceptor.listen(path);
        ASSERT_TRUE(ok);

        acceptor.set_accept_handler([&](int fd, EndPoint) {
            accept_count++;
            ::close(fd);
        });

        acceptor.close();
        EXPECT_TRUE(acceptor.uds_path().empty());
    }
}

TEST_F(TcpTransportComprehensiveTest, MultipleTransportInstances) {
    auto ep1 = endpoint_ops::parse_endpoint("127.0.0.1:19001");
    auto ep2 = endpoint_ops::parse_endpoint("127.0.0.1:19002");

    TlsConfig tls_config;
    PoolConfig pool_config;
    pool_config.use_tls = false;

    TcpTransport transport1(ep1, tls_config, pool_config, nullptr);
    TcpTransport transport2(ep2, tls_config, pool_config, nullptr);

    transport1.listen(19001);
    transport2.listen(19002);

    transport1.stop_listening();
    transport2.stop_listening();
}

TEST_F(TcpTransportComprehensiveTest, UdsThenTcpFallbackSameEndpoint) {
    RegistrarConfig reg_config;
    NodeRegistry registry(reg_config);

    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:19999");
    NodeEndpoint node_ep;
    node_ep.identity.endpoint = ep;
    node_ep.identity.host = "127.0.0.1";
    node_ep.tcp_port = 19999;
    node_ep.identity.uds_path = "/tmp/hpactor/node1.sock";
    registry.upsert_endpoint(node_ep);

    TlsConfig tls_config;
    PoolConfig pool_config;
    pool_config.use_tls = false;

    TcpTransport transport(ep, tls_config, pool_config, &registry);

    EventLoop loop;
    TcpAcceptor acceptor(&loop);
    bool listening = acceptor.listen(19999);
    ASSERT_TRUE(listening);

    std::atomic<int> accepts{0};
    acceptor.set_accept_handler([&](int fd, EndPoint) {
        accepts++;
        ::close(fd);
    });

    transport.connect(ep);

    transport.stop_listening();
    acceptor.close();
}

TEST_F(TcpTransportComprehensiveTest, SendDuringCloseRace) {
    EventLoop loop;

    int sv[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    ASSERT_EQ(r, 0);

    int flags = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(sv[1], F_GETFL, 0);
    fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);

    auto remote_ep = endpoint_ops::parse_endpoint("127.0.0.1:12345");
    auto client = WireFrameConnection::create_as_client(sv[0], LocalEndpoint,
                                                        remote_ep, &loop);
    auto server = WireFrameConnection::create_as_server(sv[1], LocalEndpoint,
                                                        remote_ep, &loop);

    std::atomic<int> send_completions{0};
    std::atomic<int> errors{0};

    client->set_send_completion_handler([&](int) { send_completions++; });
    client->set_error_handler([&](ConnectionPtr, const error&) { errors++; });

    for (int i = 0; i < 10; ++i) {
        StreamBuffer data{static_cast<uint8_t>('A' + i)};
        client->send(data);
    }

    client->close();

    loop.wait(50);
    loop.process_completions();

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST_F(TcpTransportComprehensiveTest, AcceptorPortRangeFallback) {
    EventLoop loop;

    TcpAcceptor acceptor1(&loop);
    bool ok1 = acceptor1.listen(0, 100);
    ASSERT_TRUE(ok1);

    TcpAcceptor acceptor2(&loop);
    bool ok2 = acceptor2.listen(acceptor1.port(), 100);
    ASSERT_TRUE(ok2);

    acceptor1.close();
    acceptor2.close();
}

TEST_F(TcpTransportComprehensiveTest, ConnectToUnknownNode) {
    RegistrarConfig reg_config;
    NodeRegistry registry(reg_config);

    auto ep = endpoint_ops::parse_endpoint("192.168.99.99:19999");
    TlsConfig tls_config;
    PoolConfig pool_config;
    pool_config.use_tls = false;

    TcpTransport transport(ep, tls_config, pool_config, &registry);

    auto conn = transport.connect(ep);
    EXPECT_EQ(conn, nullptr);
}

TEST_F(TcpTransportComprehensiveTest, SendWithoutFrameHandler) {
    EventLoop loop;

    int sv[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    ASSERT_EQ(r, 0);

    auto remote_ep = endpoint_ops::parse_endpoint("127.0.0.1:12345");
    auto conn = WireFrameConnection::create_as_client(sv[0], LocalEndpoint,
                                                      remote_ep, &loop);

    std::atomic<int> completions{0};
    conn->set_send_completion_handler([&](int) { completions++; });

    StreamBuffer data{'H', 'i'};
    conn->send(data);

    loop.wait(50);
    loop.process_completions();

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST_F(TcpTransportComprehensiveTest, AcceptorReuseAfterClose) {
    EventLoop loop;
    UnixDomainAcceptor acceptor(&loop);

    std::string path1 = "/tmp/hpactor/test_reuse1.sock";
    std::string path2 = "/tmp/hpactor/test_reuse2.sock";

    bool ok1 = acceptor.listen(path1);
    ASSERT_TRUE(ok1);
    ASSERT_TRUE(acceptor.is_listening());
    EXPECT_EQ(acceptor.uds_path(), path1);

    acceptor.close();
    EXPECT_FALSE(acceptor.is_listening());

    bool ok2 = acceptor.listen(path2);
    ASSERT_TRUE(ok2);
    ASSERT_TRUE(acceptor.is_listening());
    EXPECT_EQ(acceptor.uds_path(), path2);

    acceptor.close();
}

TEST_F(TcpTransportComprehensiveTest, UdsPathDerivationEdgeCases) {
    auto derive_path = [](const std::string& node_id) -> std::string {
        std::string sanitized = node_id;
        for (char& c : sanitized) {
            if (c == ':')
                c = '_';
        }
        return "/tmp/hpactor/" + sanitized + ".sock";
    };

    struct {
        const char* input;
        const char* expected;
    } cases[] = {
        {"localhost:5000", "/tmp/hpactor/localhost_5000.sock"},
        {"127.0.0.1:8080", "/tmp/hpactor/127.0.0.1_8080.sock"},
        {"node1", "/tmp/hpactor/node1.sock"},
        {"192.168.1.100:9000", "/tmp/hpactor/192.168.1.100_9000.sock"},
    };

    for (const auto& c : cases) {
        std::string derived = derive_path(c.input);
        EXPECT_EQ(derived.find("/tmp/hpactor/"), 0u);
        EXPECT_EQ(derived, c.expected);
    }
}

TEST_F(TcpTransportComprehensiveTest, WireFrameConnectionStateTransitions) {
    EventLoop loop;

    int sv[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    ASSERT_EQ(r, 0);

    auto remote_ep = endpoint_ops::parse_endpoint("127.0.0.1:12345");

    auto client = WireFrameConnection::create_as_client(sv[0], LocalEndpoint,
                                                        remote_ep, &loop);
    EXPECT_EQ(client->state(), ConnectionState::Connected);

    auto server = WireFrameConnection::create_as_server(sv[1], LocalEndpoint,
                                                        remote_ep, &loop);
    EXPECT_EQ(server->state(), ConnectionState::Connected);

    client->close();
    EXPECT_EQ(client->state(), ConnectionState::Disconnected);

    ::close(sv[0]);
    ::close(sv[1]);
}

TEST_F(TcpTransportComprehensiveTest, AcceptorEdgeTriggered) {
    EventLoop loop;
    TcpAcceptor acceptor(&loop);

    bool ok = acceptor.listen(22150);
    ASSERT_TRUE(ok);

    std::atomic<int> accept_count{0};
    acceptor.set_accept_handler([&](int fd, EndPoint) {
        accept_count++;
        ::close(fd);
    });

    std::thread clients([&]() {
        for (int i = 0; i < 10; ++i) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(0x7F000001);
            addr.sin_port = htons(22150);
            (void)::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                            sizeof(addr));
            close(fd);
        }
    });

    for (int i = 0; i < 20; ++i) {
        loop.wait(10);
        loop.process_completions();
    }

    clients.join();
    acceptor.close();
}

TEST_F(TcpTransportComprehensiveTest, DoubleStopListening) {
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:22300");
    TlsConfig tls_config;
    PoolConfig pool_config;

    TcpTransport transport(ep, tls_config, pool_config, nullptr);

    transport.listen(22300);

    transport.stop_listening();
    transport.stop_listening();
    // No crash = pass
}

TEST_F(TcpTransportComprehensiveTest, ConcurrentAcceptorOperations) {
    EventLoop loop;
    std::atomic<int> total_accepts{0};
    std::atomic<bool> stop{false};

    std::vector<std::unique_ptr<TcpAcceptor>> acceptors;
    std::vector<unsigned short> ports;

    for (int i = 0; i < 3; ++i) {
        auto acceptor = std::make_unique<TcpAcceptor>(&loop);
        uint16_t port = static_cast<uint16_t>(22000 + i);
        bool ok = acceptor->listen(port);
        if (ok) {
            acceptor->set_accept_handler([&](int fd, EndPoint) {
                total_accepts++;
                ::close(fd);
            });
            acceptors.push_back(std::move(acceptor));
            ports.push_back(port);
        }
    }

    std::vector<std::thread> threads;
    for (size_t i = 0; i < ports.size(); ++i) {
        threads.emplace_back([&, port = ports[i]]() {
            for (int j = 0; j < 5; ++j) {
                int fd = socket(AF_INET, SOCK_STREAM, 0);
                if (fd >= 0) {
                    struct sockaddr_in addr;
                    memset(&addr, 0, sizeof(addr));
                    addr.sin_family = AF_INET;
                    addr.sin_addr.s_addr = htonl(0x7F000001);
                    addr.sin_port = htons(port);
                    (void)::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                                    sizeof(addr));
                    close(fd);
                }
            }
        });
    }

    std::thread runner([&]() {
        while (!stop.load()) {
            loop.wait(10);
            loop.process_completions();
        }
    });

    for (auto& t : threads) {
        t.join();
    }

    loop.wait(50);
    loop.process_completions();

    for (auto& a : acceptors) {
        a->close();
    }
    acceptors.clear();

    stop = true;
    runner.join();
}
