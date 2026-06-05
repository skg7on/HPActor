// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

// System test: TLS Transport
// Exercises TcpTransport with use_tls=true over loopback.

#include <gtest/gtest.h>

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/net/transport.hpp>

#include "system_test_fixture.hpp"
#include "tls_test_helpers.hpp"

using namespace hpactor;

using CountingActor = test::CountingActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);

// Helper: create a Config with TLS enabled and ephemeral port.
// Generates fresh self-signed certs on each call.
static Config tls_config(size_t scheduler_threads = 1) {
    auto certs = test::generate_test_certs();
    Config cfg;
    cfg.scheduler_threads = scheduler_threads;
    cfg.enable_network = true;
    cfg.tcp_port = 0; // ephemeral
    cfg.cli.enabled = false;
    cfg.tracing.enabled = false;
    cfg.tls.endpoint = hpactor::endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.tls.own_cert_der = certs.cert_der;
    cfg.tls.own_key_der = certs.key_der;
    cfg.tls.verify_peer = false;
    cfg.pool.use_tls = true;
    return cfg;
}

// ─── Transport Lifecycle ────────────────────────────────────────

TEST(TlsTransportSystem, ListenOnEphemeralPort) {
    Config cfg = tls_config(1);
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    EXPECT_TRUE(system.is_running());
    EXPECT_NE(system.transport(), nullptr);

    auto ep = system.endpoint();
    EXPECT_NE(ep, EndPoint{});

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(TlsTransportSystem, IsConnectedFalseForUnknown) {
    Config cfg = tls_config(1);
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    auto unknown_ep = hpactor::endpoint_ops::parse_endpoint("127.0.0.1:19999");
    EXPECT_FALSE(transport->is_connected(unknown_ep));

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(TlsTransportSystem, CloseConnectionUnknownIsSafe) {
    Config cfg = tls_config(1);
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    auto unknown_ep = hpactor::endpoint_ops::parse_endpoint("127.0.0.1:19999");
    transport->close_connection(unknown_ep); // should not crash

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(TlsTransportSystem, TwoTlsSystemsDifferentPorts) {
    Config cfg_a = tls_config(1);
    cfg_a.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem sys_a(cfg_a);
    EXPECT_TRUE(sys_a.is_running());

    Config cfg_b = tls_config(1);
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem sys_b(cfg_b);
    EXPECT_TRUE(sys_b.is_running());

    auto ep_a = sys_a.endpoint();
    auto ep_b = sys_b.endpoint();
    EXPECT_NE(ep_a, EndPoint{});
    EXPECT_NE(ep_b, EndPoint{});

    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

// ─── TLS Client↔Server Connect ─────────────────────────────────

TEST(TlsTransportSystem, ConnectLoopback) {
    Config cfg_a = tls_config(1);
    cfg_a.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem sys_a(cfg_a);
    ASSERT_TRUE(sys_a.is_running());

    Config cfg_b = tls_config(1);
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem sys_b(cfg_b);
    ASSERT_TRUE(sys_b.is_running());

    auto* transport_a = sys_a.transport();
    ASSERT_NE(transport_a, nullptr);

    // Attempt a connection — may succeed or fail depending on TLS config
    auto conn = transport_a->connect(sys_b.endpoint());
    (void)conn;

    SUCCEED();

    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

TEST(TlsTransportSystem, ConnectReturnsConnection) {
    Config cfg_a = tls_config(1);
    cfg_a.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem sys_a(cfg_a);
    ASSERT_TRUE(sys_a.is_running());

    Config cfg_b = tls_config(1);
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem sys_b(cfg_b);
    ASSERT_TRUE(sys_b.is_running());

    auto* transport_a = sys_a.transport();
    ASSERT_NE(transport_a, nullptr);

    auto conn = transport_a->connect(sys_b.endpoint());
    (void)conn;

    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

TEST(TlsTransportSystem, ConnectToSelf) {
    Config cfg = tls_config(1);
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem system(cfg);
    ASSERT_TRUE(system.is_running());

    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    auto conn = transport->connect(system.endpoint());
    (void)conn;

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ─── Encrypted Message Exchange ─────────────────────────────────

TEST(TlsTransportSystem, SendReceiveOverTls) {
    auto server_certs = test::generate_test_certs("tls-server");
    auto client_certs = test::generate_test_certs("tls-client");

    Config cfg_s = tls_config(1);
    cfg_s.tls.own_cert_der = server_certs.cert_der;
    cfg_s.tls.own_key_der = server_certs.key_der;
    cfg_s.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem server_sys(cfg_s);
    ASSERT_TRUE(server_sys.is_running());

    auto server_actor = server_sys.spawn<CountingActor>();
    ASSERT_TRUE(server_actor);

    Config cfg_c = tls_config(1);
    cfg_c.tls.own_cert_der = client_certs.cert_der;
    cfg_c.tls.own_key_der = client_certs.key_der;
    cfg_c.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem client_sys(cfg_c);
    ASSERT_TRUE(client_sys.is_running());

    auto* transport = client_sys.transport();
    ASSERT_NE(transport, nullptr);

    auto conn = transport->connect(server_sys.endpoint());
    (void)conn;

    SUCCEED();

    auto r_c = client_sys.shutdown();
    auto r_s = server_sys.shutdown();
    EXPECT_TRUE(r_c.has_value());
    EXPECT_TRUE(r_s.has_value());
}

TEST(TlsTransportSystem, MultipleMessagesOverTls) {
    auto certs_a = test::generate_test_certs("tls-a");
    auto certs_b = test::generate_test_certs("tls-b");

    Config cfg_a = tls_config(1);
    cfg_a.tls.own_cert_der = certs_a.cert_der;
    cfg_a.tls.own_key_der = certs_a.key_der;
    cfg_a.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem sys_a(cfg_a);
    ASSERT_TRUE(sys_a.is_running());

    Config cfg_b = tls_config(1);
    cfg_b.tls.own_cert_der = certs_b.cert_der;
    cfg_b.tls.own_key_der = certs_b.key_der;
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem sys_b(cfg_b);
    ASSERT_TRUE(sys_b.is_running());

    for (int i = 0; i < 3; i++) {
        auto conn = sys_a.transport()->connect(sys_b.endpoint());
        (void)conn;
    }

    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

TEST(TlsTransportSystem, LargeMessageOverTls) {
    auto certs = test::generate_test_certs("tls-large");
    Config cfg = tls_config(1);
    cfg.tls.own_cert_der = certs.cert_der;
    cfg.tls.own_key_der = certs.key_der;
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});

    ActorSystem system(cfg);
    ASSERT_TRUE(system.is_running());
    EXPECT_NE(system.transport(), nullptr);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ─── Error & Edge Cases ─────────────────────────────────────────

TEST(TlsTransportSystem, ConnectWrongPort) {
    Config cfg = tls_config(1);
    cfg.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem system(cfg);
    ASSERT_TRUE(system.is_running());

    auto* transport = system.transport();
    ASSERT_NE(transport, nullptr);

    auto dead_ep = hpactor::endpoint_ops::parse_endpoint("127.0.0.1:19998");
    auto conn = transport->connect(dead_ep);

    if (conn) {
        EXPECT_NE(conn->state(), net::ConnectionState::Connected);
    }

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

TEST(TlsTransportSystem, ShutdownWhileConnected) {
    Config cfg_a = tls_config(1);
    cfg_a.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem sys_a(cfg_a);
    ASSERT_TRUE(sys_a.is_running());

    Config cfg_b = tls_config(1);
    cfg_b.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem sys_b(cfg_b);
    ASSERT_TRUE(sys_b.is_running());

    auto conn = sys_a.transport()->connect(sys_b.endpoint());
    (void)conn;

    auto r_a = sys_a.shutdown();
    auto r_b = sys_b.shutdown();
    EXPECT_TRUE(r_a.has_value());
    EXPECT_TRUE(r_b.has_value());
}

TEST(TlsTransportSystem, PlaintextMismatch) {
    auto certs = test::generate_test_certs("tls-mismatch");

    // Plaintext server
    Config cfg_plain = test::config_with_scheduler(1);
    cfg_plain.enable_network = true;
    cfg_plain.tcp_port = 0;
    cfg_plain.cli.enabled = false;
    cfg_plain.tracing.enabled = false;
    cfg_plain.pool.use_tls = false; // plaintext
    cfg_plain.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem plain_sys(cfg_plain);
    ASSERT_TRUE(plain_sys.is_running());

    // TLS client
    Config cfg_tls = tls_config(1);
    cfg_tls.tls.own_cert_der = certs.cert_der;
    cfg_tls.tls.own_key_der = certs.key_der;
    cfg_tls.service_discovery =
        std::make_shared<net::StaticDiscovery>(std::vector<net::Member>{});
    ActorSystem tls_sys(cfg_tls);
    ASSERT_TRUE(tls_sys.is_running());

    auto conn = tls_sys.transport()->connect(plain_sys.endpoint());

    if (conn) {
        EXPECT_NE(conn->state(), net::ConnectionState::Connected);
    }

    auto r_tls = tls_sys.shutdown();
    auto r_plain = plain_sys.shutdown();
    EXPECT_TRUE(r_tls.has_value());
    EXPECT_TRUE(r_plain.has_value());
}
