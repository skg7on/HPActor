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

#include <hpactor/net/connection_pool.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

TEST(ConnectionPoolTest, PoolConfigDefaults) {
    PoolConfig config;
    EXPECT_EQ(config.min_connections, 1u);
    EXPECT_EQ(config.max_connections, 4u);
    EXPECT_EQ(config.max_attempts, 5u);
    EXPECT_EQ(config.initial_backoff.count(), 1000);
    EXPECT_EQ(config.max_backoff.count(), 16000);
    EXPECT_EQ(config.outbound_limits.max_messages, 1000u);
    EXPECT_EQ(config.circuit_breaker_cfg.failure_threshold, 5u);
}

TEST(ConnectionPoolTest, PoolStatsInitialState) {
    PoolStats stats;
    EXPECT_EQ(stats.active_connections, 0u);
    EXPECT_EQ(stats.pending_messages, 0u);
    EXPECT_EQ(stats.reconnect_attempts, 0u);
    EXPECT_EQ(stats.is_connected, false);
    EXPECT_EQ(stats.pending_control_messages, 0u);
    EXPECT_EQ(stats.pending_data_messages, 0u);
    EXPECT_EQ(stats.pending_bytes, 0u);
    EXPECT_EQ(stats.pressure_state, 0u);
    EXPECT_EQ(stats.circuit_state, 0u);
}

TEST(ConnectionPoolTest, StatsInitial) {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9001");
    PoolConfig cfg;
    ConnectionPool pool(ep, cfg, nullptr);
    auto s = pool.stats();
    EXPECT_EQ(s.active_connections, 0u);
    EXPECT_EQ(s.pending_messages, 0u);
    EXPECT_EQ(s.reconnect_attempts, 0u);
    EXPECT_FALSE(s.is_connected);
}

TEST(ConnectionPoolTest, DrainEmpty) {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9002");
    PoolConfig cfg;
    ConnectionPool pool(ep, cfg, nullptr);
    size_t unsent = pool.drain();
    EXPECT_EQ(unsent, 0u);
}

TEST(ConnectionPoolTest, AbortEmpty) {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9003");
    PoolConfig cfg;
    ConnectionPool pool(ep, cfg, nullptr);
    pool.abort();
    // No crash
}

TEST(ConnectionPoolTest, IsConnectedFalse) {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9004");
    PoolConfig cfg;
    ConnectionPool pool(ep, cfg, nullptr);
    EXPECT_FALSE(pool.is_connected());
}
