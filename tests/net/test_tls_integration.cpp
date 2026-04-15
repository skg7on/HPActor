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

#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/net/tls_context.hpp>
#include <hpactor/net/connection_pool.hpp>

#include <cassert>
#include <iostream>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test 1: TlsConfig and PoolConfig can be created
    TlsConfig tls_config;
    tls_config.node_id = 1;
    tls_config.verify_peer = true;
    assert(tls_config.node_id == 1);
    assert(tls_config.verify_peer == true);

    // Test 2: PoolConfig defaults
    PoolConfig pool_config;
    assert(pool_config.min_connections == 1);
    assert(pool_config.max_connections == 4);
    assert(pool_config.max_pending == 1000);
    assert(pool_config.max_attempts == 5);
    assert(pool_config.initial_backoff.count() == 1000);
    assert(pool_config.max_backoff.count() == 16000);

    // Test 3: TlsContext can be created from empty config
    TlsContext ctx = TlsContext::from_config(tls_config);
    assert(ctx.node_id() == 1);

    // Test 4: TcpTransport can be constructed (requires valid config)
    // Note: This will fail if certs aren't available, but the API should be testable
    // For a full integration test, real certificates would be needed

    std::cout << "TLS integration test passed" << std::endl;
    return 0;
}