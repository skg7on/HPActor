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
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/net/tls_context.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

TEST(TlsIntegrationTest, TlsConfigAndPoolConfig) {
    TlsConfig tls_config;
    tls_config.endpoint = hpactor::endpoint_ops::parse_endpoint("localhost:"
                                                                "12345");
    tls_config.verify_peer = true;
    EXPECT_EQ(hpactor::endpoint_ops::to_string(tls_config.endpoint), "127.0.0."
                                                                     "1:12345");
    EXPECT_TRUE(tls_config.verify_peer);

    PoolConfig pool_config;
    EXPECT_EQ(pool_config.min_connections, 1u);
    EXPECT_EQ(pool_config.max_connections, 4u);
    EXPECT_EQ(pool_config.max_pending, 1000u);
    EXPECT_EQ(pool_config.max_attempts, 5u);
    EXPECT_EQ(pool_config.initial_backoff.count(), 1000);
    EXPECT_EQ(pool_config.max_backoff.count(), 16000);
}

TEST(TlsIntegrationTest, TlsContextCreatedFromEmptyConfig) {
    TlsConfig tls_config;
    tls_config.endpoint = hpactor::endpoint_ops::parse_endpoint("localhost:"
                                                                "12345");
    tls_config.verify_peer = true;
    TlsContext ctx = TlsContext::from_config(tls_config);
    EXPECT_EQ(hpactor::endpoint_ops::to_string(ctx.endpoint()), "127.0.0.1:"
                                                                "12345");
}
