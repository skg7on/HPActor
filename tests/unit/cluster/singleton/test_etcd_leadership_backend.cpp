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

#include <gtest/gtest.h>

#include <hpactor/cluster/singleton/etcd_leadership_backend.hpp>

namespace hpactor::cluster::singleton {

using namespace std::chrono_literals;

TEST(EtcdLeadershipBackendTest, ConstructionWithConfig) {
    EtcdLeadershipBackend::Config cfg;
    cfg.endpoints = {"https://localhost:2379"};
    cfg.key_prefix = "/hpactor";
    cfg.request_timeout = std::chrono::milliseconds(500);

    EtcdLeadershipBackend backend(cfg);
    // Construction should not throw or crash
    SUCCEED();
}

TEST(EtcdLeadershipBackendTest, CurrentOwnerReturnsNotOwnerWhenKeyAbsent) {
    EtcdLeadershipBackend::Config cfg;
    cfg.endpoints = {"https://localhost:2379"};
    EtcdLeadershipBackend backend(cfg);

    auto result = backend.current_owner("nonexistent");
    EXPECT_EQ(result.status, LeadershipStatusCode::NotOwner);
}

TEST(EtcdLeadershipBackendTest, TryAcquireTimesOutWhenEtcdUnreachable) {
    EtcdLeadershipBackend::Config cfg;
    cfg.endpoints = {"https://192.0.2.1:2379"}; // TEST-NET-1, unreachable
    cfg.request_timeout = std::chrono::milliseconds(100);
    EtcdLeadershipBackend backend(cfg);

    LeadershipAttempt attempt{"test-singleton", "node-a", 0, 10s};
    auto result = backend.try_acquire(attempt);
    // Should time out or report backend unavailable
    bool expected = result.status == LeadershipStatusCode::TimedOut ||
                    result.status == LeadershipStatusCode::BackendUnavailable;
    EXPECT_TRUE(expected) << "Got status: " << static_cast<int>(result.status);
}

} // namespace hpactor::cluster::singleton
