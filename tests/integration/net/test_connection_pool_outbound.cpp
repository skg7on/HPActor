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

/// \file
/// Integration tests for ConnectionPool with EndpointOutboundQueue and
/// EndpointCircuitBreaker.

#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/endpoint_circuit_breaker.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>
#include <hpactor/net/event_loop.hpp>

#include <gtest/gtest.h>

namespace hpactor::net {
namespace {

class ConnectionPoolOutboundTest : public ::testing::Test {
  protected:
    PoolConfig make_config() {
        PoolConfig cfg;
        cfg.outbound_limits.max_messages = 10;
        cfg.outbound_limits.max_bytes = 10 * 1024;
        cfg.outbound_limits.control_lane_reserve = 2;
        cfg.outbound_limits.reliable_headroom_pct = 0.20;
        cfg.circuit_breaker_cfg.failure_threshold = 3;
        cfg.circuit_breaker_cfg.cooldown = std::chrono::milliseconds{100};
        return cfg;
    }
};

TEST_F(ConnectionPoolOutboundTest, TrySendReturnsFalseWhenQueueFull) {
    PoolConfig cfg = make_config();
    cfg.outbound_limits.max_messages = 2;
    cfg.outbound_limits.control_lane_reserve = 0;
    cfg.outbound_limits.reliable_headroom_pct = 0.0;

    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    // No EventLoop needed for this test -- try_send only enqueues locally
    // when no connection is available.
    ConnectionPool pool(ep, cfg, nullptr);

    ActorAddress addr;
    addr.endpoint = ep;

    // First two sends fill the queue
    EXPECT_TRUE(pool.try_send(addr, StreamBuffer(128, 0xBB)));
    EXPECT_TRUE(pool.try_send(addr, StreamBuffer(128, 0xBB)));
    // Third send should be rejected (queue full)
    EXPECT_FALSE(pool.try_send(addr, StreamBuffer(128, 0xBB)));
}

TEST_F(ConnectionPoolOutboundTest, CircuitBreakerOpensAfterFailures) {
    EventLoop loop;

    PoolConfig cfg = make_config();
    cfg.circuit_breaker_cfg.failure_threshold = 2;

    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    ConnectionPool pool(ep, cfg, &loop);

    EXPECT_EQ(pool.circuit_breaker().state(), EndpointCircuitBreaker::State::Closed);

    pool.on_connection_error(nullptr, error(errors::unknown, "test error"));
    EXPECT_EQ(pool.circuit_breaker().state(), EndpointCircuitBreaker::State::Closed);
    pool.on_connection_error(nullptr, error(errors::unknown, "test error"));
    EXPECT_EQ(pool.circuit_breaker().state(), EndpointCircuitBreaker::State::Open);
}

TEST_F(ConnectionPoolOutboundTest, QueueDepthReflectedInStats) {
    PoolConfig cfg = make_config();
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    // No EventLoop needed -- try_send only enqueues locally.
    ConnectionPool pool(ep, cfg, nullptr);

    ActorAddress addr;
    addr.endpoint = ep;

    pool.try_send(addr, StreamBuffer(128, 0xCC));
    auto s = pool.stats();
    EXPECT_GT(s.pending_messages, 0u);
    EXPECT_EQ(s.circuit_state,
              static_cast<uint8_t>(EndpointCircuitBreaker::State::Closed));
}

TEST_F(ConnectionPoolOutboundTest, CircuitBreakerBlocksSendsWhenOpen) {
    EventLoop loop;

    PoolConfig cfg = make_config();
    cfg.circuit_breaker_cfg.failure_threshold = 1;
    cfg.circuit_breaker_cfg.cooldown = std::chrono::milliseconds{60000};

    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    ConnectionPool pool(ep, cfg, &loop);

    ActorAddress addr;
    addr.endpoint = ep;

    // Open the circuit (on_connection_error triggers schedule_reconnect
    // which requires a live EventLoop).
    pool.on_connection_error(nullptr, error(errors::unknown, "test error"));
    EXPECT_EQ(pool.circuit_breaker().state(), EndpointCircuitBreaker::State::Open);

    // Sends should be blocked
    EXPECT_FALSE(pool.try_send(addr, StreamBuffer(128, 0xDD)));
}

} // anonymous namespace
} // namespace hpactor::net
