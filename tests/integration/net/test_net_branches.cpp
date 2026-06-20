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

// tests/integration/net/test_net_branches.cpp
//
// Branch-coverage integration tests for network subsystems: circuit breaker,
// outbound queue priority lanes, WireFrame encoding, static discovery,
// connection pool backoff config, and registrar constants.

#include <gtest/gtest.h>

#include <hpactor/adt/node_identity.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/endpoint_circuit_breaker.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <thread>

using namespace hpactor;

// ============================================================================
// EndpointCircuitBreaker — state machine transitions
// ============================================================================

TEST(EndpointCircuitBreakerTest, InitialStateIsClosed) {
    net::EndpointCircuitBreakerConfig cfg;
    net::EndpointCircuitBreaker cb(cfg);
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Closed);
    EXPECT_EQ(cb.failure_count(), 0U);
    EXPECT_TRUE(cb.allow_send());
}

TEST(EndpointCircuitBreakerTest, FailuresBelowThresholdKeepClosed) {
    net::EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 5;
    net::EndpointCircuitBreaker cb(cfg);

    for (size_t i = 0; i < 4; ++i) {
        cb.record_failure();
        EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Closed);
    }
    EXPECT_EQ(cb.failure_count(), 4U);
    EXPECT_TRUE(cb.allow_send());
}

TEST(EndpointCircuitBreakerTest, ThresholdFailuresOpenCircuit) {
    net::EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 3;
    cfg.cooldown = std::chrono::milliseconds(100);
    net::EndpointCircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    cb.record_failure(); // threshold reached
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Open);
    EXPECT_EQ(cb.failure_count(), 3U);
    EXPECT_FALSE(cb.allow_send());
}

TEST(EndpointCircuitBreakerTest, OpenToHalfOpenAfterCooldown) {
    net::EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.cooldown = std::chrono::milliseconds(10);
    net::EndpointCircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Open);

    // Wait for cooldown
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // First send after cooldown transitions to HalfOpen
    EXPECT_TRUE(cb.allow_send());
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::HalfOpen);
}

TEST(EndpointCircuitBreakerTest, SuccessInHalfOpenClosesCircuit) {
    net::EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.cooldown = std::chrono::milliseconds(10);
    cfg.half_open_probe_limit = 1;
    net::EndpointCircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Open);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(cb.allow_send()); // transitions to HalfOpen
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::HalfOpen);

    cb.record_success();
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Closed);
    EXPECT_EQ(cb.failure_count(), 0U);
}

TEST(EndpointCircuitBreakerTest, FailureInHalfOpenReopensCircuit) {
    net::EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.cooldown = std::chrono::milliseconds(10);
    cfg.half_open_probe_limit = 1;
    net::EndpointCircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Open);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(cb.allow_send()); // transitions to HalfOpen

    cb.record_failure(); // failure in HalfOpen
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Open);
}

TEST(EndpointCircuitBreakerTest, ResetToClosed) {
    net::EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    net::EndpointCircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Open);

    cb.reset();
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Closed);
    EXPECT_EQ(cb.failure_count(), 0U);
}

// ============================================================================
// EndpointOutboundQueue — priority lanes and pressure
// ============================================================================

TEST(EndpointOutboundQueueTest, InitialStateEmpty) {
    net::EndpointOutboundLimits limits;
    net::EndpointOutboundQueue q(limits);
    EXPECT_EQ(q.total_messages(), 0U);
    EXPECT_EQ(q.total_bytes(), 0U);
    EXPECT_EQ(q.control_messages(), 0U);
    EXPECT_EQ(q.data_messages(), 0U);
    EXPECT_EQ(q.depth_ratio(), 0.0);
}

TEST(EndpointOutboundQueueTest, TryDequeueFromEmpty) {
    net::EndpointOutboundLimits limits;
    net::EndpointOutboundQueue q(limits);
    auto msg = q.try_dequeue();
    EXPECT_FALSE(msg.has_value());
}

TEST(EndpointOutboundQueueTest, SnapshotReflectsState) {
    net::EndpointOutboundLimits limits;
    net::EndpointOutboundQueue q(limits);
    auto snap = q.snapshot();
    EXPECT_EQ(snap.control_messages, 0U);
    EXPECT_EQ(snap.data_messages, 0U);
}

TEST(EndpointOutboundQueueTest, ControlLaneAdmitsMessages) {
    net::EndpointOutboundLimits limits;
    limits.max_messages = 10;
    limits.control_lane_reserve = 4;
    net::EndpointOutboundQueue q(limits);

    // Enqueue control messages — should be admitted within limits
    for (size_t i = 0; i < 4; ++i) {
        net::PendingMessage pm;
        pm.target = ActorAddress{};
        pm.data = StreamBuffer(1, static_cast<uint8_t>(0));
        pm.enqueued_at = std::chrono::steady_clock::now();
        TransportSendResult result =
            q.try_enqueue(std::move(pm), mailbox::DeliveryMode::BestEffort,
                          TypeTag::IoCompletionTag);
        EXPECT_NE(result, TransportSendResult::QueueFull);
    }
    EXPECT_EQ(q.control_messages(), 4U);
}

TEST(EndpointOutboundQueueTest, PressureStateInitialIsNormal) {
    net::EndpointOutboundLimits limits;
    net::EndpointOutboundQueue q(limits);
    auto ps = q.pressure_state();
    EXPECT_EQ(ps, mailbox::MailboxPressureState::Normal);
}

// ============================================================================
// ConnectionPool — PoolConfig, PoolStats, and backoff configuration
// ============================================================================

TEST(ConnectionPoolConfigTest, DefaultConfigValues) {
    net::PoolConfig cfg;
    EXPECT_EQ(cfg.min_connections, 1U);
    EXPECT_EQ(cfg.max_connections, 4U);
    EXPECT_EQ(cfg.max_attempts, 5U);
    EXPECT_EQ(cfg.initial_backoff, std::chrono::milliseconds(1000));
    EXPECT_EQ(cfg.max_backoff, std::chrono::milliseconds(16000));
    EXPECT_FALSE(cfg.use_tls);
}

TEST(ConnectionPoolConfigTest, ExponentialBackoffDoubles) {
    // Verify that the backoff sequence doubles up to max
    auto initial = std::chrono::milliseconds(1000);
    auto max = std::chrono::milliseconds(16000);

    auto backoff = initial;
    for (int i = 0; i < 10; ++i) {
        EXPECT_LE(backoff, max);
        backoff = std::min(backoff * 2, max);
    }
    EXPECT_EQ(backoff, max);
}

TEST(ConnectionPoolStatsTest, DefaultStats) {
    net::PoolStats stats{};
    EXPECT_EQ(stats.active_connections, 0U);
    EXPECT_EQ(stats.pending_messages, 0U);
    EXPECT_EQ(stats.reconnect_attempts, 0U);
    EXPECT_FALSE(stats.is_connected);
    EXPECT_EQ(stats.pending_control_messages, 0U);
    EXPECT_EQ(stats.pending_data_messages, 0U);
    EXPECT_EQ(stats.pending_bytes, 0U);
    EXPECT_EQ(stats.pressure_state, 0U);
    EXPECT_EQ(stats.circuit_state, 0U);
}

// ============================================================================
// WireFrame — magic header, encode/decode, and re-sync on bad magic
// ============================================================================

TEST(WireFrameTest, MagicHeaderConstant) {
    EXPECT_EQ(net::WireFrame::MagicHeader, 0x43415048U);
    EXPECT_EQ(net::WireFrame::HeaderSize, 8U);
}

TEST(WireFrameTest, DefaultFrameHasCorrectMagic) {
    net::WireFrame frame;
    EXPECT_EQ(frame.magic_hdr, net::WireFrame::MagicHeader);
    // length is set during encode, not initialized by default
}

TEST(WireFrameTest, EncodeProducesNonEmptyBuffer) {
    net::WireFrame frame;
    // Set basic proto fields available on ActorMsgFrame
    frame.pb_frame.set_message_id(42);
    StreamBuffer encoded = frame.encode();
    EXPECT_GE(encoded.size(), net::WireFrame::HeaderSize);
}

TEST(WireFrameTest, EncodeRoundTrip) {
    net::WireFrame frame;
    frame.pb_frame.set_message_id(1234);
    frame.pb_frame.set_type_tag(0x1000);

    StreamBuffer encoded = frame.encode();
    ASSERT_GE(encoded.size(), net::WireFrame::HeaderSize);

    // Decode the buffer
    net::WireFrame decoded = net::WireFrame::decode(encoded);
    EXPECT_EQ(decoded.magic_hdr, net::WireFrame::MagicHeader);
    EXPECT_EQ(decoded.pb_frame.message_id(), 1234U);
    EXPECT_EQ(decoded.pb_frame.type_tag(), 0x1000U);
}

TEST(WireFrameTest, DecodeShortBufferDoesNotCrash) {
    // Create a buffer too short to contain a full header
    StreamBuffer short_buf;
    short_buf.push_back(static_cast<uint8_t>('H'));
    short_buf.push_back(static_cast<uint8_t>('P'));

    // Decode should not crash on short/invalid data
    net::WireFrame frame = net::WireFrame::decode(short_buf);
    // Frame should be default-constructed on decode failure
    EXPECT_EQ(frame.magic_hdr, net::WireFrame::MagicHeader);
}

// ============================================================================
// StaticDiscovery — member lookup and iteration
// ============================================================================

// Helper to make a Member with a given endpoint and identity name
static net::Member make_member(const EndPoint& ep, const std::string& host_name,
                               uint64_t incarnation = 0) {
    net::Member m;
    m.identity.endpoint = ep;
    m.identity.host = host_name;
    m.status = net::MemberStatus::Alive;
    m.incarnation = incarnation;
    return m;
}

TEST(StaticDiscoveryTest, EmptyDiscovery) {
    net::StaticDiscovery sd({});
    EXPECT_EQ(sd.backend_name(), "static");

    auto all = sd.discover_all();
    EXPECT_TRUE(all.empty());

    auto* m = sd.discover(LocalEndpoint);
    EXPECT_EQ(m, nullptr);
}

TEST(StaticDiscoveryTest, DiscoverExistingMember) {
    Ipv4Endpoint ep1{0x7F000002, 0}; // 127.0.0.2:0
    EndPoint ep{ep1};
    auto m1 = make_member(ep, "node1", 1);

    net::StaticDiscovery sd({m1});
    auto* found = sd.discover(ep);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->identity.endpoint, ep);
    EXPECT_EQ(found->incarnation, 1U);
    EXPECT_EQ(found->status, net::MemberStatus::Alive);
}

TEST(StaticDiscoveryTest, DiscoverAllReturnsAllMembers) {
    Ipv4Endpoint ep_a{0x7F00000A, 0};
    Ipv4Endpoint ep_b{0x7F00000B, 0};
    auto m1 = make_member(EndPoint{ep_a}, "a");
    auto m2 = make_member(EndPoint{ep_b}, "b");

    net::StaticDiscovery sd({m1, m2});
    auto all = sd.discover_all();
    EXPECT_EQ(all.size(), 2U);
}

TEST(StaticDiscoveryTest, DiscoverNonExistentReturnsNull) {
    Ipv4Endpoint ep1{0x7F00000C, 0};
    auto m1 = make_member(EndPoint{ep1}, "node1");

    net::StaticDiscovery sd({m1});

    Ipv4Endpoint ep_unknown{0x0A000001, 0}; // 10.0.0.1:0
    EndPoint unknown{ep_unknown};
    auto* found = sd.discover(unknown);
    EXPECT_EQ(found, nullptr);
}

// ============================================================================
// Registrar — protocol constants and field values
// ============================================================================

TEST(RegistrarConstantsTest, MagicAndVersion) {
    EXPECT_EQ(net::RegistrarMagic, 0x48504143U); // "HPAC"
    EXPECT_EQ(net::RegistrarVersion, 0x01U);
    EXPECT_EQ(net::RegistrarHeaderSize, 12U);
}

TEST(RegistrarConstantsTest, TcpMagicAndVersion) {
    EXPECT_EQ(net::TcpRegistrarMagic, 0x48505243U); // "HPRC"
    EXPECT_EQ(net::TcpRegistrarVersion, 0x01U);
    EXPECT_EQ(net::TcpHeaderSize, 10U);
}

TEST(RegistrarConstantsTest, MessageTypeValues) {
    EXPECT_NE(static_cast<uint8_t>(net::RegistrarMessageType::Register),
              static_cast<uint8_t>(net::RegistrarMessageType::Heartbeat));
    EXPECT_NE(static_cast<uint8_t>(net::RegistrarMessageType::ResolveQuery),
              static_cast<uint8_t>(net::RegistrarMessageType::ResolveResponse));
}

TEST(RegistrarConfigTest, DefaultValues) {
    net::RegistrarConfig cfg;
    EXPECT_EQ(cfg.udp_port, 5353U);
    EXPECT_EQ(cfg.tcp_port, 5353U);
    EXPECT_EQ(cfg.heartbeat_interval, std::chrono::milliseconds(5000));
    EXPECT_EQ(cfg.expiration_timeout, std::chrono::milliseconds(15000));
    EXPECT_EQ(cfg.probe_interval, std::chrono::milliseconds(30000));
    EXPECT_FALSE(cfg.disable_server);
}

// ============================================================================
// MemberStatus — enum coverage
// ============================================================================

TEST(MemberStatusTest, AllValuesDistinct) {
    EXPECT_NE(static_cast<uint8_t>(net::MemberStatus::Alive),
              static_cast<uint8_t>(net::MemberStatus::Suspicious));
    EXPECT_NE(static_cast<uint8_t>(net::MemberStatus::Suspicious),
              static_cast<uint8_t>(net::MemberStatus::Dead));
    EXPECT_NE(static_cast<uint8_t>(net::MemberStatus::Dead),
              static_cast<uint8_t>(net::MemberStatus::Left));
}

// ============================================================================
// EndpointCircuitBreaker — additional edge cases
// ============================================================================

TEST(EndpointCircuitBreakerTest, CustomConfigValues) {
    net::EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 10;
    cfg.cooldown = std::chrono::milliseconds(5000);
    cfg.half_open_probe_limit = 3;
    net::EndpointCircuitBreaker cb(cfg);

    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Closed);
    // 9 failures below threshold
    for (size_t i = 0; i < 9; ++i)
        cb.record_failure();
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Closed);
    EXPECT_TRUE(cb.allow_send());
}

TEST(EndpointCircuitBreakerTest, MultipleSuccessesInHalfOpen) {
    net::EndpointCircuitBreakerConfig cfg;
    cfg.failure_threshold = 2;
    cfg.cooldown = std::chrono::milliseconds(10);
    cfg.half_open_probe_limit = 3; // allow up to 3 probes
    net::EndpointCircuitBreaker cb(cfg);

    cb.record_failure();
    cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // First probe transitions to HalfOpen
    EXPECT_TRUE(cb.allow_send());
    cb.record_success();

    // More probes allowed up to half_open_probe_limit
    EXPECT_TRUE(cb.allow_send());
    cb.record_success();
    EXPECT_EQ(cb.state(), net::EndpointCircuitBreaker::State::Closed);
}
