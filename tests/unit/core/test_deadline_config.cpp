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

#include <hpactor/actor/system/actor_system.hpp>

#include <chrono>

// ── Default TTL config defaults ─────────────────────────────────────
TEST(DeadlineConfigTest, DefaultMessageTtlZeroByDefault) {
    hpactor::Config cfg{};
    // default_message_ttl_ms should be 0 (disabled) by default so
    // existing behavior is unchanged.
    EXPECT_EQ(cfg.default_message_ttl_ms, std::chrono::milliseconds{0});
}

TEST(DeadlineConfigTest, DefaultMessageTtlSettable) {
    hpactor::Config cfg{};
    cfg.default_message_ttl_ms = std::chrono::milliseconds{30000};
    EXPECT_EQ(cfg.default_message_ttl_ms, std::chrono::milliseconds{30000});
}
