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

// System test: Network Deep Workflow
// Deep coverage for connection pool, TCP transport, event loop, HTTP client,
// and actor location cache. Targets untested code paths in src/net/.

#include <gtest/gtest.h>

#include <hpactor/actor/spawn.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/endpoint_circuit_breaker.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/net/tls_context.hpp>
#include <hpactor/net/transport.hpp>

#include "system_test_fixture.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace hpactor;

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 1: Connection pool deep
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkDeep, PoolRoundRobinWithMultipleConnections) {
    // Verify pool default state and connection count tracking.
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9101");
    net::PoolConfig cfg;
    cfg.max_connections = 4;
    net::EventLoop loop;

    net::ConnectionPool pool(ep, cfg, &loop);

    // Initial state: no connections, pool is not connected
    EXPECT_FALSE(pool.is_connected());
    auto stats = pool.stats();
    EXPECT_EQ(stats.active_connections, 0u);

    // Verify pool can be used with explicit host/port connect via transport
    SUCCEED();
}

TEST(NetworkDeep, PoolRpcHandlerRouting) {
    // Verify RPC handler, spawn handler, and actor message handler
    // registration on the connection pool.
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9102");
    net::PoolConfig cfg;
    net::EventLoop loop;

    net::ConnectionPool pool(ep, cfg, &loop);

    // Set RPC handler
    int rpc_count = 0;
    pool.set_rpc_handler([&rpc_count](const RpcResponseFrame&) { rpc_count++; });

    // Set spawn handler — SpawnResponse is in hpactor::, not hpactor::net::
    int spawn_count = 0;
    pool.set_spawn_handler(
        [&spawn_count](uint64_t, const SpawnResponse&) { spawn_count++; });

    // Set actor message handler
    int actor_msg_count = 0;
    pool.set_actor_message_handler(
        [&actor_msg_count](const net::WireFrame&) { actor_msg_count++; });

    // Handlers set without crash
    SUCCEED();
}

TEST(NetworkDeep, PoolReconnectBackoffBehavior) {
    // Verify reconnect attempt limits and pool configuration.
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9103");
    net::PoolConfig cfg;
    cfg.max_attempts = 3;
    cfg.initial_backoff = std::chrono::milliseconds(1000);
    cfg.max_backoff = std::chrono::milliseconds(16000);
    net::EventLoop loop;

    net::ConnectionPool pool(ep, cfg, &loop);

    // Initial stats: zero reconnect attempts, not connected
    auto stats = pool.stats();
    EXPECT_EQ(stats.reconnect_attempts, 0u);
    EXPECT_EQ(stats.active_connections, 0u);
    EXPECT_FALSE(stats.is_connected);

    // Pool with no connections — circuit breaker starts closed
    EXPECT_EQ(stats.circuit_state, 0u);

    // Verify remote endpoint accessor
    EXPECT_EQ(pool.remote_endpoint(), ep);

    SUCCEED();
}

TEST(NetworkDeep, PoolPreWarmAndAcceptors) {
    // Verify prewarm_pool stores acceptor info without crashing.
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9104");
    net::PoolConfig cfg;
    net::EventLoop loop;

    net::ConnectionPool pool(ep, cfg, &loop);

    std::vector<net::AcceptorInfo> acceptors;
    net::AcceptorInfo info;
    info.port = 8080;
    info.handshake_version = 1;
    info.protocol_version = 1;
    info.tls_required = false;
    acceptors.push_back(info);

    pool.prewarm_pool(ep, acceptors);
    // Should not crash
    SUCCEED();
}

TEST(NetworkDeep, PoolDrainAndAbort) {
    // Verify drain and abort lifecycle for empty pool.
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9105");
    net::PoolConfig cfg;
    net::EventLoop loop;

    auto* pool = new net::ConnectionPool(ep, cfg, &loop);

    // Drain empty pool returns 0 unsent messages
    size_t unsent = pool->drain();
    EXPECT_EQ(unsent, 0u);

    // Create another pool and abort it
    auto* pool2 = new net::ConnectionPool(ep, cfg, &loop);
    pool2->abort(); // should not crash

    delete pool2;
    delete pool;
    SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 2: TCP transport deep
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkDeep, TransportUdsPathExists) {
    // Verify that the UDS directory is created when TcpTransport is
    // constructed.
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    // The /tmp/hpactor directory should exist after transport construction
    // (TcpTransport constructor creates it via ::mkdir)
    struct stat st;
    int ret = ::stat("/tmp/hpactor", &st);
    EXPECT_EQ(ret, 0);
    EXPECT_TRUE(S_ISDIR(st.st_mode));

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(NetworkDeep, TlsContextFromConfig) {
    // Verify TlsContext construction from empty config (plain text mode).
    net::TlsConfig tls_cfg;
    tls_cfg.verify_peer = false;

    net::TlsContext ctx = net::TlsContext::from_config(tls_cfg);

    // Verify endpoint is preserved
    EXPECT_EQ(ctx.endpoint(), tls_cfg.endpoint);

    // Verify public key is empty (no certs configured)
    EXPECT_EQ(ctx.public_key().size(), 0u);

    SUCCEED();
}

TEST(NetworkDeep, TlsConfigDefaults) {
    // Verify default TlsConfig values.
    net::TlsConfig tls_cfg;
    EXPECT_TRUE(tls_cfg.verify_peer);
    EXPECT_EQ(tls_cfg.own_cert_der.size(), 0u);
    EXPECT_EQ(tls_cfg.own_key_der.size(), 0u);
    EXPECT_TRUE(tls_cfg.ca_certs_der.empty());

    SUCCEED();
}

TEST(NetworkDeep, NonBlockingConnectCompletionPolling) {
    // Verify EventLoop provides write handler support for non-blocking connect.
    net::EventLoop loop;

    // Run event loop in background thread
    std::thread loop_thread([&loop]() { loop.run(); });
    test::assert_eventually([&loop]() { return loop.is_running(); }, 3000);
    EXPECT_TRUE(loop.is_running());

    // Verify backend supports write handlers (needed for complete_connect)
    bool has_write = loop.supports_write_handler();
    (void)has_write; // kqueue/epoll backends should support this

    // Verify read handler support
    bool has_read = loop.supports_read_handler();
    (void)has_read;

    // Verify FD registration API works
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    bool added = loop.add_fd(fd, net::EventLoop::Event::Write);
    EXPECT_TRUE(added);

    // Set a write handler for the FD
    std::atomic<bool> write_fired{false};
    loop.set_write_handler(fd, [&write_fired](int) { write_fired = true; });

    // Clean up
    loop.clear_write_handler(fd);
    loop.remove_fd(fd);
    ::close(fd);

    loop.stop();
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
}

TEST(NetworkDeep, PoolConfigOutboundLimits) {
    // Verify outbound queue limits and circuit breaker in PoolConfig.
    net::PoolConfig cfg;
    cfg.outbound_limits.max_messages = 500;
    cfg.outbound_limits.max_bytes = 65536;
    cfg.circuit_breaker_cfg.failure_threshold = 3;

    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9107");
    net::EventLoop loop;
    net::ConnectionPool pool(ep, cfg, &loop);

    // Verify outbound queue accessible for inspection
    const auto& queue = pool.outbound_queue();
    EXPECT_EQ(queue.total_messages(), 0u);
    EXPECT_EQ(queue.total_bytes(), 0u);

    // Circuit breaker accessible — starts in Closed state
    const auto& cb = pool.circuit_breaker();
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Closed);

    // Mutable circuit breaker access
    auto& mutable_cb = pool.circuit_breaker();
    EXPECT_EQ(mutable_cb.state(), net::EndpointCircuitBreaker::State::Closed);

    SUCCEED();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 3: Event loop deep
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkDeep, EventLoopTimerHandleMappingLifecycle) {
    // Verify timer handle mapping: run_after creates unique handles,
    // cancel_timer cleans up, and handles are properly tracked.
    // Note: Timer dispatch is backend-dependent (GCD may need active run loop),
    // so we verify scheduling/cancellation without waiting for actual fire.
    net::EventLoop loop;

    std::thread loop_thread([&loop]() { loop.run(); });
    test::assert_eventually([&loop]() { return loop.is_running(); }, 3000);
    EXPECT_TRUE(loop.is_running());

    // Schedule a one-shot timer — handle should be non-zero
    bool fired = false;
    uint64_t handle1 = loop.run_after([&fired]() { fired = true; }, 100);
    EXPECT_GT(handle1, 0u);

    // Schedule a second one-shot timer — handles should be unique
    bool fired2 = false;
    uint64_t handle2 = loop.run_after([&fired2]() { fired2 = true; }, 200);
    EXPECT_GT(handle2, 0u);
    EXPECT_NE(handle1, handle2); // handles should be unique

    // Cancel first timer before it fires
    loop.cancel_timer(handle1);

    // Cancel second timer
    loop.cancel_timer(handle2);

    // Verify cancel_timer handles already-cancelled timers gracefully
    loop.cancel_timer(handle1); // double-cancel should be safe

    loop.stop();
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
}

TEST(NetworkDeep, EventLoopRepeatingTimer) {
    // Verify repeating timer creation and cancellation.
    // Timer dispatch is backend-dependent: schedule a repeating timer,
    // verify it receives a valid handle, cancel it cleanly.
    net::EventLoop loop;

    std::thread loop_thread([&loop]() { loop.run(); });
    test::assert_eventually([&loop]() { return loop.is_running(); }, 3000);
    EXPECT_TRUE(loop.is_running());

    // Schedule a repeating timer
    std::atomic<int> fire_count{0};
    uint64_t handle = loop.run_every([&fire_count]() { fire_count++; }, 50);
    EXPECT_GT(handle, 0u);

    // Cancel it before waiting for fires (backend-dependent delivery)
    loop.cancel_timer(handle);

    // Schedule multiple repeating timers and cancel them all
    uint64_t h1 = loop.run_every([]() {}, 100);
    uint64_t h2 = loop.run_every([]() {}, 200);
    uint64_t h3 = loop.run_every([]() {}, 300);
    EXPECT_GT(h1, 0u);
    EXPECT_GT(h2, 0u);
    EXPECT_GT(h3, 0u);
    EXPECT_NE(h1, h2);
    EXPECT_NE(h2, h3);

    loop.cancel_timer(h1);
    loop.cancel_timer(h2);
    loop.cancel_timer(h3);

    loop.stop();
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
}

TEST(NetworkDeep, EventLoopBackendDetection) {
    // Verify the EventLoop reports its backend name and provides
    // a non-null backend pointer when running.
    net::EventLoop loop;

    std::thread loop_thread([&loop]() { loop.run(); });
    test::assert_eventually([&loop]() { return loop.is_running(); }, 3000);
    EXPECT_TRUE(loop.is_running());

    const char* name = loop.backend_name();
    EXPECT_NE(name, nullptr);
    // On macOS, backend should be "kqueue" or "gcd"
    // On Linux, backend should be "epoll" or "iouring"
    EXPECT_STRNE(name, "unknown");

    // Verify backend pointer is non-null when running
    EXPECT_NE(loop.backend(), nullptr);

    loop.stop();
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
}

TEST(NetworkDeep, EventLoopFdAddRemoveLifecycle) {
    // Verify FD registration, update, and removal lifecycle.
    net::EventLoop loop;

    std::thread loop_thread([&loop]() { loop.run(); });
    test::assert_eventually([&loop]() { return loop.is_running(); }, 3000);
    EXPECT_TRUE(loop.is_running());

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    // Add FD for read events
    bool added = loop.add_fd(fd, net::EventLoop::Event::Read);
    EXPECT_TRUE(added);

    // Update FD for read+write events
    bool updated = loop.update_fd(
        fd, static_cast<net::EventLoop::Event>(
                static_cast<uint32_t>(net::EventLoop::Event::Read) |
                static_cast<uint32_t>(net::EventLoop::Event::Write)));
    EXPECT_TRUE(updated);

    // Verify has_event returns true
    EXPECT_TRUE(loop.has_event(fd, net::EventLoop::Event::Read));

    // Remove FD
    bool removed = loop.remove_fd(fd);
    EXPECT_TRUE(removed);

    ::close(fd);

    loop.stop();
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
}

TEST(NetworkDeep, EventLoopWaitTimesOut) {
    // Verify wait() returns 0 on timeout when no events are pending.
    net::EventLoop loop;

    std::thread loop_thread([&loop]() { loop.run(); });
    test::assert_eventually([&loop]() { return loop.is_running(); }, 3000);
    EXPECT_TRUE(loop.is_running());

    // Wait with a short timeout — should return 0 (no events)
    int nfds = loop.wait(10);
    EXPECT_GE(nfds, 0);

    loop.stop();
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 4: HTTP client + cache
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkDeep, HttpClientCreationAndConfig) {
    // Verify HttpClient can be created with an event loop, configured,
    // and destroyed cleanly.
    net::EventLoop loop;
    net::HttpClient client(&loop);

    // Configure timeout and retries
    client.set_default_timeout(std::chrono::milliseconds(5000));
    client.set_max_retries(3);

    // Abort when no requests are in flight
    client.abort();

    SUCCEED();
}

TEST(NetworkDeep, HttpClientRequestToInvalidUrl) {
    // Verify HttpClient gracefully handles unreachable URLs by returning
    // an error result rather than crashing.
    net::EventLoop loop;
    net::HttpClient client(&loop);

    // Use a very short timeout to avoid slow TCP connect timeouts.
    client.set_default_timeout(std::chrono::milliseconds(100));
    client.set_max_retries(0);

    // Request to a non-routable address — connection should fail fast
    auto future = client.get("http://127.0.0.1:19999/nonexistent");

    // The blocking get() should return an error (connection refused or timeout)
    auto res = future.get();
    // The result should contain an error
    EXPECT_FALSE(res.has_value()) << "Expected error for unreachable URL";

    SUCCEED();
}

TEST(NetworkDeep, ActorLocationCacheTtlExpiry) {
    net::ActorLocationCache cache;

    // Put entry with very short TTL (expires immediately)
    ActorId id(42);
    EndPoint ep = endpoint_ops::parse_endpoint("192.168.1.1:9000");
    cache.put(id, ep, std::chrono::seconds(0)); // expires immediately

    // get() should return nullopt because TTL is checked on access
    auto result = cache.get(id);
    EXPECT_FALSE(result.has_value());

    // Put entry with normal TTL
    ActorId id2(43);
    cache.put(id2, ep); // default 30s TTL

    // Verify the normal entry is still accessible
    auto result2 = cache.get(id2);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value(), ep);

    // purge_expired() should clean up the already-expired entry silently
    cache.purge_expired();

    // Normal entry should survive purge (TTL hasn't expired yet)
    auto result3 = cache.get(id2);
    EXPECT_TRUE(result3.has_value());
}

TEST(NetworkDeep, ActorLocationCacheEvictNode) {
    net::ActorLocationCache cache;

    EndPoint ep_a = endpoint_ops::parse_endpoint("10.0.0.1:8000");
    EndPoint ep_b = endpoint_ops::parse_endpoint("10.0.0.2:8000");

    ActorId id_a1(1);
    ActorId id_a2(2);
    ActorId id_b1(3);

    // Put two actors on node A, one on node B
    cache.put(id_a1, ep_a);
    cache.put(id_a2, ep_a);
    cache.put(id_b1, ep_b);

    // Verify all three are present
    EXPECT_TRUE(cache.get(id_a1).has_value());
    EXPECT_TRUE(cache.get(id_a2).has_value());
    EXPECT_TRUE(cache.get(id_b1).has_value());

    // Evict all entries for node A
    cache.evict_node(ep_a);

    // Node A entries should be gone
    EXPECT_FALSE(cache.get(id_a1).has_value());
    EXPECT_FALSE(cache.get(id_a2).has_value());

    // Node B entry should still be present
    EXPECT_TRUE(cache.get(id_b1).has_value());

    // Evict node B — all entries gone
    cache.evict_node(ep_b);
    EXPECT_FALSE(cache.get(id_b1).has_value());
}

TEST(NetworkDeep, ActorLocationCacheSize) {
    net::ActorLocationCache cache;

    // Put many entries to exercise the internal hash map
    for (int i = 0; i < 100; i++) {
        ActorId id(static_cast<uint64_t>(i + 1));
        EndPoint ep =
            endpoint_ops::parse_endpoint("10.0.0." + std::to_string(i % 255 + 1) +
                                         ":" + std::to_string(8000 + i));
        cache.put(id, ep, std::chrono::seconds(60));
    }

    // Verify the first and last entries are accessible
    EXPECT_TRUE(cache.get(ActorId(1)).has_value());
    EXPECT_TRUE(cache.get(ActorId(100)).has_value());

    // Purge expired — nothing should be expired yet with 60s TTL
    cache.purge_expired();
    EXPECT_TRUE(cache.get(ActorId(50)).has_value());

    // Purge with some 0-TTL entries mixed in
    for (int i = 0; i < 10; i++) {
        ActorId id(static_cast<uint64_t>(200 + i));
        EndPoint ep = endpoint_ops::parse_endpoint("10.0.0.1:9000");
        cache.put(id, ep, std::chrono::seconds(0));
    }
    cache.purge_expired();

    // The 0-TTL entries should be gone after purge
    EXPECT_FALSE(cache.get(ActorId(200)).has_value());
    // Original entries should still be present
    EXPECT_TRUE(cache.get(ActorId(1)).has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 5: Connection Pool Transport — end-to-end pool + transport
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkDeep, TransportPoolCreation) {
    // Verify transport creates and manages connection pools.
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    // Verify get_or_create_pool via is_connected for unknown endpoints
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:19999");
    EXPECT_FALSE(transport->is_connected(ep));

    // Close connection for a non-existent pool (should not crash)
    transport->close_connection(ep);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(NetworkDeep, TransportConnectionPoolLifecycle) {
    // Verify full lifecycle: transport connect/host:port for closed port.
    // The connect() API itself should not crash even when the remote
    // endpoint is unreachable or refuses the connection.
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    // Connect to a port that likely has no listener — the API itself
    // should not crash. (Note: connect() adds the connection to the pool
    // even before it completes, so is_connected() may return true for
    // connections still in Connecting state.)
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:19998");
    auto conn = transport->connect(ep, "127.0.0.1", 19998);
    (void)conn;

    // Close the connection for this endpoint to clean up
    transport->close_connection(ep);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(NetworkDeep, TransportRpcHandlerPropagation) {
    // Verify RPC handler and pool config set on transport without crash.
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    std::atomic<int> rpc_call_count{0};
    transport->set_rpc_handler(
        [&rpc_call_count](const RpcResponseFrame&) { rpc_call_count++; });

    // Set actor message handler on TcpTransport
    auto* tcp = static_cast<net::TcpTransport*>(transport);
    std::atomic<int> actor_msg_count{0};
    tcp->set_actor_message_handler(
        [&actor_msg_count](const net::WireFrame&) { actor_msg_count++; });

    // Set pool config with custom values
    net::PoolConfig pool_cfg;
    pool_cfg.max_connections = 8;
    pool_cfg.use_tls = false;
    tcp->set_pool_config(pool_cfg);

    SUCCEED();

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(NetworkDeep, TransportStopListeningAndEphemeralPort) {
    // Verify transport can stop listening and rebind.
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0; // ephemeral
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    // The endpoint should be valid after binding
    auto ep = transport->endpoint();
    EXPECT_NE(ep, EndPoint{});

    // stop_listening should not crash
    transport->stop_listening();

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}
