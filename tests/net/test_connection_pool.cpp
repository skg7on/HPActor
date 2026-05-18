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

#include <cassert>
#include <cstdio>

using namespace hpactor;
using namespace hpactor::net;

void test_stats_initial() {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9001");
    PoolConfig cfg;
    ConnectionPool pool(ep, cfg, nullptr);
    auto s = pool.stats();
    assert(s.active_connections == 0);
    assert(s.pending_messages == 0);
    assert(s.reconnect_attempts == 0);
    assert(!s.is_connected);
    printf("  PASSED test_stats_initial\n");
}

void test_drain_empty() {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9002");
    PoolConfig cfg;
    ConnectionPool pool(ep, cfg, nullptr);
    size_t unsent = pool.drain();
    assert(unsent == 0);
    printf("  PASSED test_drain_empty\n");
}

void test_abort_empty() {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9003");
    PoolConfig cfg;
    ConnectionPool pool(ep, cfg, nullptr);
    pool.abort();
    printf("  PASSED test_abort_empty\n");
}

void test_is_connected_false() {
    EndPoint ep = endpoint_ops::parse_endpoint("127.0.0.1:9004");
    PoolConfig cfg;
    ConnectionPool pool(ep, cfg, nullptr);
    assert(!pool.is_connected());
    printf("  PASSED test_is_connected_false\n");
}

int main() {
    // Test PoolConfig default values
    PoolConfig config;
    assert(config.min_connections == 1);
    assert(config.max_connections == 4);
    assert(config.max_pending == 1000);
    assert(config.max_attempts == 5);
    assert(config.initial_backoff.count() == 1000);
    assert(config.max_backoff.count() == 16000);

    // Test PoolStats initial state
    PoolStats stats;
    assert(stats.active_connections == 0);
    assert(stats.pending_messages == 0);
    assert(stats.reconnect_attempts == 0);
    assert(stats.is_connected == false);

    test_stats_initial();
    test_drain_empty();
    test_abort_empty();
    test_is_connected_false();

    return 0;
}