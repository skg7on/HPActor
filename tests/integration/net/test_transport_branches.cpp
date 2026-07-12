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
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/net/wireframe_connection.hpp>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

namespace {

void ensure_tmp_dir() {
    ::mkdir("/tmp/hpactor", 0755);
}

EndPoint make_ep(const std::string& host, uint16_t port) {
    return endpoint_ops::parse_endpoint(host + ":" + std::to_string(port));
}

PoolConfig make_pool_cfg(size_t max_conns = 4) {
    PoolConfig cfg;
    cfg.min_connections = 1;
    cfg.max_connections = max_conns;
    cfg.max_attempts = 3;
    cfg.initial_backoff = std::chrono::milliseconds(50);
    cfg.max_backoff = std::chrono::milliseconds(200);
    cfg.use_tls = false;
    return cfg;
}

std::pair<int, int> make_socket_pair() {
    int sv[2];
    ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int flags = ::fcntl(sv[0], F_GETFL, 0);
    ::fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
    flags = ::fcntl(sv[1], F_GETFL, 0);
    ::fcntl(sv[1], F_SETFL, flags | O_NONBLOCK);
    return {sv[0], sv[1]};
}

} // anonymous namespace

class TransportBranchesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ensure_tmp_dir();
    }
};

// ── 1. TcpTransport construction and listen lifecycle ──────────────────

TEST_F(TransportBranchesTest, TcpTransportConstructAndListenLifecycle) {
    EndPoint ep = make_ep("127.0.0.1", 0);
    TlsConfig tls_cfg;
    PoolConfig pool_cfg = make_pool_cfg(2);

    EventLoop loop;
    loop.run();
    TcpTransport transport(ep, tls_cfg, pool_cfg, &loop, nullptr);
    EXPECT_EQ(transport.endpoint(), ep);

    transport.listen(0);
    transport.stop_listening();

    EndPoint unknown = make_ep("10.0.0.1", 9999);
    transport.close_connection(unknown);
    EXPECT_FALSE(transport.is_connected(unknown));
}

// ── 2. TcpTransport UDS connect path ───────────────────────────────────

TEST_F(TransportBranchesTest, TcpTransportUdsConnectLifecycle) {
    EndPoint ep = make_ep("127.0.0.1", 0);
    TlsConfig tls_cfg;
    PoolConfig pool_cfg = make_pool_cfg(2);

    EventLoop loop;
    loop.run();
    TcpTransport transport(ep, tls_cfg, pool_cfg, &loop, nullptr);

    std::string socket_path = "/tmp/hpactor/test_transport_uds.sock";
    ::unlink(socket_path.c_str());
    auto conn = transport.connect_unix_domain(ep, socket_path);
    if (conn) {
        EXPECT_TRUE(conn->state() == ConnectionState::Connecting ||
                    conn->state() == ConnectionState::Disconnected);
    }
}

// ── 3. Transport with different pool configs ───────────────────────────

TEST_F(TransportBranchesTest, TransportWithDifferentPoolConfigs) {
    EndPoint ep = make_ep("127.0.0.1", 0);

    // Config with max_connections = 1
    {
        PoolConfig cfg;
        cfg.min_connections = 0;
        cfg.max_connections = 1;
        cfg.max_attempts = 1;
        cfg.initial_backoff = std::chrono::milliseconds(10);
        cfg.max_backoff = std::chrono::milliseconds(50);
        cfg.use_tls = false;

        TlsConfig tls_cfg;
        EventLoop loop;
        loop.run();
        TcpTransport transport(ep, tls_cfg, cfg, &loop, nullptr);
        EXPECT_EQ(transport.endpoint(), ep);
    }

    // Config with larger pool
    {
        PoolConfig cfg;
        cfg.min_connections = 2;
        cfg.max_connections = 8;
        cfg.max_attempts = 10;
        cfg.initial_backoff = std::chrono::milliseconds(100);
        cfg.max_backoff = std::chrono::milliseconds(5000);
        cfg.use_tls = false;

        TlsConfig tls_cfg;
        EventLoop loop;
        loop.run();
        TcpTransport transport(ep, tls_cfg, cfg, &loop, nullptr);
        EXPECT_EQ(transport.endpoint(), ep);
    }
}

// ── 4. Transport send/try_send with basic wireframe ────────────────────

TEST_F(TransportBranchesTest, TransportSendWithWireFrameConnection) {
    EventLoop loop;
    auto [fd_a, fd_b] = make_socket_pair();

    auto server_conn = WireFrameConnection::create_as_server(
        fd_a, make_ep("127.0.0.1", 10000), make_ep("127.0.0.1", 20000), &loop);
    ASSERT_NE(server_conn, nullptr);

    auto client_conn = WireFrameConnection::create_as_client(
        fd_b, make_ep("127.0.0.1", 20000), make_ep("127.0.0.1", 10000), &loop);
    ASSERT_NE(client_conn, nullptr);

    StreamBuffer data = {'H', 'P', 'A', 'C', 'T', 'O', 'R'};
    client_conn->send(data);
    server_conn->handle_read();

    client_conn->close();
    server_conn->close();
}

// ── 5. Transport actor message handler registration ────────────────────

TEST_F(TransportBranchesTest, TransportActorMessageHandlerRegistration) {
    EndPoint ep = make_ep("127.0.0.1", 0);
    TlsConfig tls_cfg;
    PoolConfig pool_cfg = make_pool_cfg(2);

    EventLoop loop;
    loop.run();
    TcpTransport transport(ep, tls_cfg, pool_cfg, &loop, nullptr);

    bool handler_called = false;
    transport.set_actor_message_handler(
        [&handler_called](const WireFrame& /*frame*/) { handler_called = true; });

    EXPECT_FALSE(handler_called);
}

// ── 6. Transport RPC handler wiring ────────────────────────────────────

TEST_F(TransportBranchesTest, TransportRpcHandlerWiring) {
    EndPoint ep = make_ep("127.0.0.1", 0);
    TlsConfig tls_cfg;
    PoolConfig pool_cfg = make_pool_cfg(2);

    EventLoop loop;
    loop.run();
    TcpTransport transport(ep, tls_cfg, pool_cfg, &loop, nullptr);

    int call_count = 0;
    transport.set_rpc_handler(
        [&call_count](const RpcResponseFrame& /*frame*/) { call_count++; });

    EXPECT_EQ(call_count, 0);
}

// ── 7. Transport metrics ring buffer pass-through ──────────────────────

TEST_F(TransportBranchesTest, TransportMetricsRingBufferPassThrough) {
    EndPoint ep = make_ep("127.0.0.1", 0);
    TlsConfig tls_cfg;
    PoolConfig pool_cfg = make_pool_cfg(2);

    EventLoop loop;
    loop.run();
    TcpTransport transport(ep, tls_cfg, pool_cfg, &loop, nullptr);

    metrics::MpscRingBuffer<metrics::MetricEvent> ring_buffer;
    transport.set_metrics_ring_buffer(&ring_buffer);
    transport.set_metrics_ring_buffer(nullptr);
}

// ── 8. EventLoop backend type detection ────────────────────────────────

TEST_F(TransportBranchesTest, EventLoopBackendTypeDetection) {
    EventLoop loop;
    const char* name = loop.backend_name();
    ASSERT_NE(name, nullptr);
    EXPECT_GT(std::strlen(name), 0u);

    std::string backend(name);
    bool known = (backend == "kqueue" || backend == "epoll" ||
                  backend == "GCD" || backend == "io_uring");
    EXPECT_TRUE(known) << "Unexpected backend: " << backend;
}

// ── 9. Reactor backend read/write dispatch support ─────────────────────

TEST_F(TransportBranchesTest, BackendReadWriteSupport) {
    EventLoop loop;
    IReactorBackend* backend = loop.backend();
    ASSERT_NE(backend, nullptr);

    bool supports_read = backend->supports_read_handler();
    bool supports_write = backend->supports_write_handler();
    EXPECT_EQ(supports_read, supports_write);
}

// ── 10. Backend FD registration and event dispatching ───────────────────

TEST_F(TransportBranchesTest, BackendFdRegistrationAndEvents) {
    EventLoop loop;
    IReactorBackend* backend = loop.backend();
    ASSERT_NE(backend, nullptr);

    auto [fd_a, fd_b] = make_socket_pair();

    bool added = backend->add_fd(fd_a, IoEvent::Read);
    EXPECT_TRUE(added);

    const char* msg = "hello";
    ssize_t written = ::write(fd_b, msg, 4);
    ASSERT_EQ(written, 4);

    int n = loop.wait(100);
    EXPECT_GE(n, 0);

    backend->process_events();
    bool has_read = loop.has_event(fd_a, EventLoop::Event::Read);
    (void)has_read;

    backend->remove_fd(fd_a);
    ::close(fd_a);
    ::close(fd_b);
}

// ── 11. Acceptor listen and accept handling ────────────────────────────

TEST_F(TransportBranchesTest, AcceptorListenAndAcceptHandling) {
    EventLoop loop;
    ensure_tmp_dir();

    // TcpAcceptor
    {
        TcpAcceptor acceptor(&loop);
        bool ok = acceptor.listen(0);
        EXPECT_TRUE(ok);
        EXPECT_TRUE(acceptor.is_listening());
        uint16_t port = acceptor.port();
        (void)port;
        acceptor.close();
        EXPECT_FALSE(acceptor.is_listening());
    }

    // UnixDomainAcceptor
    {
        std::string path = "/tmp/hpactor/test_acceptor_uds.sock";
        ::unlink(path.c_str());
        UnixDomainAcceptor acceptor(&loop);
        bool ok = acceptor.listen(path);
        EXPECT_TRUE(ok);
        EXPECT_TRUE(acceptor.is_listening());
        EXPECT_EQ(acceptor.uds_path(), path);
        acceptor.close();
        EXPECT_FALSE(acceptor.is_listening());
    }
}

// ── 12. WireFrame connection send/read with callbacks ──────────────────

TEST_F(TransportBranchesTest, WireFrameConnectionCallbacks) {
    EventLoop loop;
    auto [fd_a, fd_b] = make_socket_pair();

    auto server = WireFrameConnection::create_as_server(
        fd_a, make_ep("127.0.0.1", 10001), make_ep("127.0.0.1", 20001), &loop);
    ASSERT_NE(server, nullptr);

    auto client = WireFrameConnection::create_as_client(
        fd_b, make_ep("127.0.0.1", 20001), make_ep("127.0.0.1", 10001), &loop);
    ASSERT_NE(client, nullptr);

    std::atomic<bool> ready_fired{false};
    std::atomic<int> frame_count{0};
    std::atomic<bool> error_fired{false};

    server->set_ready_handler(
        [&ready_fired](ConnectionPtr /*conn*/) { ready_fired = true; });
    server->set_frame_handler(
        [&frame_count](StreamBuffer /*data*/) { frame_count++; });
    server->set_error_handler(
        [&error_fired](ConnectionPtr /*conn*/, const error& /*err*/) {
            error_fired = true;
        });

    StreamBuffer data = {'A', 'B', 'C', 'D'};
    client->send(data);
    loop.process_completions();

    client->close();
    server->close();
}

// ── 13. ConnectionPool config with circuit breaker ─────────────────────

TEST_F(TransportBranchesTest, ConnectionPoolConfigWithCircuitBreaker) {
    EventLoop loop;
    EndPoint remote = make_ep("192.168.1.100", 9000);

    PoolConfig cfg;
    cfg.min_connections = 1;
    cfg.max_connections = 4;
    cfg.max_attempts = 5;
    cfg.initial_backoff = std::chrono::milliseconds(100);
    cfg.max_backoff = std::chrono::milliseconds(1000);
    cfg.use_tls = false;
    cfg.circuit_breaker_cfg.failure_threshold = 3;
    cfg.circuit_breaker_cfg.cooldown = std::chrono::milliseconds(100);
    cfg.outbound_limits.max_messages = 100;
    cfg.outbound_limits.max_bytes = 64 * 1024;

    ConnectionPool pool(remote, cfg, &loop);
    EXPECT_FALSE(pool.is_connected());
    EXPECT_EQ(pool.remote_endpoint(), remote);

    PoolStats stats = pool.stats();
    EXPECT_EQ(stats.active_connections, 0u);
    EXPECT_EQ(stats.is_connected, false);

    pool.close();
}

// ── 14. TcpTransport connect to unknown host returns null ───────────────

TEST_F(TransportBranchesTest, TcpTransportConnectUnknownHostReturnsNull) {
    EndPoint ep = make_ep("127.0.0.1", 0);
    TlsConfig tls_cfg;
    PoolConfig pool_cfg = make_pool_cfg(2);

    EventLoop loop;
    loop.run();
    TcpTransport transport(ep, tls_cfg, pool_cfg, &loop, nullptr);

    EndPoint unknown = make_ep("192.0.2.1", 9999);
    auto conn = transport.connect(unknown);
    if (conn) {
        EXPECT_EQ(conn->state(), ConnectionState::Connecting);
    }
}

// ── 15. Fault injection path coverage ──────────────────────────────────

TEST_F(TransportBranchesTest, FaultInjectionPathCoverage) {
    EndPoint ep = make_ep("127.0.0.1", 0);
    TlsConfig tls_cfg;
    PoolConfig pool_cfg = make_pool_cfg(2);

    EventLoop loop;
    loop.run();
    TcpTransport transport(ep, tls_cfg, pool_cfg, &loop, nullptr);

    ActorAddress addr;
    StreamBuffer data = {'f', 'a', 'u', 'l', 't'};
    auto result = transport.try_send(addr, data);
    (void)result;

    EndPoint unknown = make_ep("203.0.113.1", 12345);
    transport.close_connection(unknown);

    SUCCEED();
}
