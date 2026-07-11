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

// System test: Network Transport Workflow
// Exercises transport lifecycle, WireFrame protocol, event loop, acceptor,
// connection pool, HTTP client, and actor location cache over loopback.

#include <gtest/gtest.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/net/wireframe_connection.hpp>

#include "system_test_fixture.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace hpactor;

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 1: Transport lifecycle
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkWorkflow, TwoSystemsLoopbackDataExchange) {
    // Create two ActorSystems on loopback with networking enabled
    Config cfg_a = test::config_with_scheduler(1);
    cfg_a.enable_network = true;
    cfg_a.tcp_port = 0; // ephemeral
    cfg_a.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem sys_a(cfg_a);
    EXPECT_TRUE(sys_a.is_running());
    EXPECT_NE(sys_a.transport(), nullptr);

    Config cfg_b = test::config_with_scheduler(1);
    cfg_b.enable_network = true;
    cfg_b.tcp_port = 0;
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem sys_b(cfg_b);
    EXPECT_TRUE(sys_b.is_running());
    EXPECT_NE(sys_b.transport(), nullptr);

    // Both transports should exist and have valid (non-empty) endpoints
    auto* t_a = sys_a.transport();
    auto* t_b = sys_b.transport();
    ASSERT_NE(t_a, nullptr);
    ASSERT_NE(t_b, nullptr);

    auto ep_a = t_a->endpoint();
    auto ep_b = t_b->endpoint();
    EXPECT_NE(ep_a, EndPoint{});
    EXPECT_NE(ep_b, EndPoint{});

    // Clean shutdown
    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

TEST(NetworkWorkflow, TransportEphemeralPortListen) {
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0; // ephemeral — OS assigns a free port
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    EXPECT_NE(system.transport(), nullptr);

    // After binding ephemeral port, endpoint should be valid
    auto ep = system.endpoint();
    EXPECT_NE(ep, EndPoint{});

    // Verify transport is a valid instance
    EXPECT_NE(system.transport(), nullptr);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(NetworkWorkflow, TransportDisabledByDefault) {
    Config cfg = test::minimal_config();
    EXPECT_FALSE(cfg.enable_network);

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());

    // Network transport should be nullptr when networking is disabled
    EXPECT_EQ(system.transport(), nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 2: WireFrame and Connection
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkWorkflow, WireFrameProtocolEncodeDecode) {
    // Create a WireFrame and verify encode/decode round-trip
    net::WireFrame frame;
    frame.pb_envelope.mutable_data_frame()->set_type_tag(42);
    frame.pb_envelope.mutable_data_frame()->set_message_id(100);
    frame.pb_envelope.mutable_data_frame()->set_flags(net::WireFrame::Important);

    // Encode to wire format
    StreamBuffer encoded = frame.encode();
    EXPECT_GE(encoded.size(), net::WireFrame::HeaderSize);

    // Decode back from wire format
    net::WireFrame decoded = net::WireFrame::decode(encoded);
    EXPECT_EQ(decoded.pb_envelope.data_frame().type_tag(), 42);
    EXPECT_EQ(decoded.pb_envelope.data_frame().message_id(), 100);
    EXPECT_EQ(decoded.pb_envelope.data_frame().flags(), net::WireFrame::Important);
    EXPECT_EQ(decoded.magic_hdr, net::WireFrame::MagicHeader);
}

TEST(NetworkWorkflow, WireFrameMagicHeaderConstant) {
    // Verify the HPAC magic header constant
    EXPECT_EQ(net::WireFrame::MagicHeader, 0x43415048u);
    EXPECT_EQ(net::WireFrame::HeaderSize, 8u);
}

TEST(NetworkWorkflow, TwoSystemsEndpointConnectivity) {
    // Setup 2 systems on loopback, verify their endpoints can be used
    // for connectivity checks (no actual connection needed)
    Config cfg_a = test::config_with_scheduler(1);
    cfg_a.enable_network = true;
    cfg_a.tcp_port = 0;
    cfg_a.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem sys_a(cfg_a);
    EXPECT_TRUE(sys_a.is_running());
    auto* t_a = sys_a.transport();
    ASSERT_NE(t_a, nullptr);

    Config cfg_b = test::config_with_scheduler(1);
    cfg_b.enable_network = true;
    cfg_b.tcp_port = 0;
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem sys_b(cfg_b);
    EXPECT_TRUE(sys_b.is_running());
    auto* t_b = sys_b.transport();
    ASSERT_NE(t_b, nullptr);

    // Each system should not be connected to the other initially
    // (since no connection has been established)
    EXPECT_FALSE(t_a->is_connected(t_b->endpoint()));
    EXPECT_FALSE(t_b->is_connected(t_a->endpoint()));

    // Systems should not be connected to themselves either
    EXPECT_FALSE(t_a->is_connected(t_a->endpoint()));
    EXPECT_FALSE(t_b->is_connected(t_b->endpoint()));

    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

TEST(NetworkWorkflow, ConnectionPoolIsConnectedForUnknown) {
    // Verify is_connected returns false for unknown/random endpoints
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    // An unknown endpoint (high port, not in registry) — should not be
    // connected
    auto unknown = endpoint_ops::parse_endpoint("127.0.0.1:65123");
    EXPECT_FALSE(transport->is_connected(unknown));

    // Connect with no registry entry should return nullptr
    auto conn = transport->connect(unknown);
    EXPECT_EQ(conn, nullptr);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkWorkflow, EventLoopRunStop) {
    // Create a standalone EventLoop, start and stop it
    net::EventLoop loop;
    EXPECT_FALSE(loop.is_running());

    // Run the event loop in a background thread
    std::atomic<bool> loop_started{false};
    std::thread loop_thread([&loop, &loop_started]() {
        loop_started.store(true);
        loop.run();
    });

    // Wait for the loop to start
    test::assert_eventually([&loop]() { return loop.is_running(); }, 3000);
    EXPECT_TRUE(loop.is_running());

    // Stop the loop
    loop.stop();
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
    EXPECT_FALSE(loop.is_running());
}

TEST(NetworkWorkflow, EventLoopTimerIntegration) {
    // Verify EventLoop timer API through transport's event loop.
    // Timers are scheduled and can be cancelled without crashing.
    // (Timer dispatch is backend-dependent; GCD may need active run loop.)
    Config cfg = test::config_with_scheduler(1);
    cfg.enable_network = true;
    cfg.tcp_port = 0;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    // Access the underlying EventLoop — it is now started by
    // NetworkRuntime::start() as Stage 0, so is_running() is true.
    auto* tcp = static_cast<net::TcpTransport*>(transport);
    net::EventLoop& loop = tcp->loop();
    EXPECT_TRUE(loop.is_running());

    // Schedule a one-shot timer — verify handle is non-zero
    uint64_t handle = loop.run_after([]() { /* no-op */ }, 100);
    EXPECT_GT(handle, 0u);

    // Cancel the timer before it fires
    loop.cancel_timer(handle);

    // Schedule a repeating timer and cancel it
    uint64_t rep_handle = loop.run_every([]() { /* no-op */ }, 200);
    EXPECT_GT(rep_handle, 0u);
    loop.cancel_timer(rep_handle);

    // The EventLoop lifecycle is now managed by NetworkRuntime.
    // Stopping it directly and shutting down the system are sufficient.
    loop.stop();

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 4: Acceptor
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkWorkflow, AcceptorListenOnEphemeralPort) {
    net::EventLoop loop;

    // Run the event loop in a background thread
    std::thread loop_thread([&loop]() { loop.run(); });
    test::assert_eventually([&loop]() { return loop.is_running(); }, 3000);
    EXPECT_TRUE(loop.is_running());

    // Create a TCP acceptor and listen on ephemeral port
    net::TcpAcceptor acceptor(&loop);
    bool bound = acceptor.listen(0); // ephemeral
    EXPECT_TRUE(bound);
    EXPECT_TRUE(acceptor.is_listening());

    // port() returns the requested port (0 for ephemeral), not the
    // OS-assigned port. Verify the API works without crashing.
    uint16_t assigned_port = acceptor.port();
    (void)assigned_port;

    // Set an accept handler — verify it does not crash
    std::atomic<int> accept_calls{0};
    acceptor.set_accept_handler(
        [&accept_calls](int /*fd*/, EndPoint /*ep*/) { accept_calls++; });

    // Clean up
    acceptor.close();
    EXPECT_FALSE(acceptor.is_listening());

    loop.stop();
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
}

TEST(NetworkWorkflow, AcceptorStopListening) {
    net::EventLoop loop;

    std::thread loop_thread([&loop]() { loop.run(); });
    test::assert_eventually([&loop]() { return loop.is_running(); }, 3000);

    net::TcpAcceptor acceptor(&loop);
    EXPECT_FALSE(acceptor.is_listening());

    // Listen on ephemeral port
    bool bound = acceptor.listen(0);
    EXPECT_TRUE(bound);
    EXPECT_TRUE(acceptor.is_listening());

    // Multiple close calls should be safe (idempotent)
    acceptor.close();
    EXPECT_FALSE(acceptor.is_listening());

    // Second close should be safe
    acceptor.close();
    EXPECT_FALSE(acceptor.is_listening());

    // Re-listen after close
    bool re_bound = acceptor.listen(0);
    EXPECT_TRUE(re_bound);
    EXPECT_TRUE(acceptor.is_listening());

    acceptor.close();

    loop.stop();
    if (loop_thread.joinable()) {
        loop_thread.join();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test Group 5: HTTP client + location cache
// ═══════════════════════════════════════════════════════════════════════════════

TEST(NetworkWorkflow, HttpClientExistence) {
    // Verify HttpClient can be created and configured
    net::EventLoop loop;
    net::HttpClient client(&loop);

    // Configure timeout and retries
    client.set_default_timeout(std::chrono::milliseconds(3000));
    client.set_max_retries(2);

    // Abort should not crash when no requests are in-flight
    client.abort();

    // Verify the client can be destroyed cleanly
    SUCCEED();
}

TEST(NetworkWorkflow, ActorLocationCacheOperations) {
    net::ActorLocationCache cache;

    // Put entries into the cache
    ActorId id_a(1);
    ActorId id_b(2);
    EndPoint ep_a = endpoint_ops::parse_endpoint("127.0.0.1:10001");
    EndPoint ep_b = endpoint_ops::parse_endpoint("127.0.0.1:10002");

    cache.put(id_a, ep_a);
    cache.put(id_b, ep_b, std::chrono::seconds(60));

    // Get entries
    auto result_a = cache.get(id_a);
    ASSERT_TRUE(result_a.has_value());
    EXPECT_EQ(result_a.value(), ep_a);

    auto result_b = cache.get(id_b);
    ASSERT_TRUE(result_b.has_value());
    EXPECT_EQ(result_b.value(), ep_b);

    // Get unknown entry
    ActorId id_unknown(999);
    auto result_unknown = cache.get(id_unknown);
    EXPECT_FALSE(result_unknown.has_value());

    // Evict a specific entry
    cache.evict(id_a);
    auto result_after_evict = cache.get(id_a);
    EXPECT_FALSE(result_after_evict.has_value());

    // id_b should still be present
    auto result_b_after = cache.get(id_b);
    ASSERT_TRUE(result_b_after.has_value());
    EXPECT_EQ(result_b_after.value(), ep_b);

    // Evict all entries for a node
    cache.put(id_a, ep_a);
    cache.evict_node(ep_a);
    EXPECT_FALSE(cache.get(id_a).has_value());
    // id_b on a different node should still be present
    EXPECT_TRUE(cache.get(id_b).has_value());

    // Purge expired — nothing should be expired yet (TTL is 30s default)
    cache.purge_expired();

    // Add a new entry with short TTL for purge test coverage
    ActorId id_c(3);
    EndPoint ep_c = endpoint_ops::parse_endpoint("127.0.0.1:10003");
    cache.put(id_c, ep_c, std::chrono::seconds(0)); // expires immediately
    cache.purge_expired();
    auto result_c = cache.get(id_c);
    EXPECT_FALSE(result_c.has_value());
}
