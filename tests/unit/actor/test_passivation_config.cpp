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
#include <hpactor/actor/lifecycle/passivation_config.hpp>

#include <chrono>

using namespace hpactor;
using namespace std::chrono_literals;

TEST(PassivationConfigTest, DefaultConstruction) {
    PassivationConfig cfg;
    EXPECT_EQ(cfg.idle_timeout.count(), 0);
    EXPECT_FALSE(cfg.durable);
    EXPECT_TRUE(cfg.allow_memory_pressure);
    EXPECT_EQ(cfg.schema_version, 1u);
}

TEST(PassivationConfigTest, IdleTimeoutDisabledByDefault) {
    PassivationConfig cfg;
    EXPECT_EQ(cfg.idle_timeout, std::chrono::milliseconds{0});
}

TEST(PassivationConfigTest, SetIdleTimeout) {
    PassivationConfig cfg;
    cfg.idle_timeout = 5min;
    EXPECT_EQ(cfg.idle_timeout, std::chrono::milliseconds{300000});
}

TEST(PassivationConfigTest, DurableFlag) {
    PassivationConfig cfg;
    cfg.durable = true;
    EXPECT_TRUE(cfg.durable);
}

TEST(PassivationConfigTest, MemoryPressureDefault) {
    PassivationConfig cfg;
    EXPECT_TRUE(cfg.allow_memory_pressure);
    cfg.allow_memory_pressure = false;
    EXPECT_FALSE(cfg.allow_memory_pressure);
}

TEST(PassivationConfigTest, SchemaVersion) {
    PassivationConfig cfg;
    cfg.schema_version = 3;
    EXPECT_EQ(cfg.schema_version, 3u);
}

TEST(PassivationRecordTest, DefaultRecord) {
    PassivationRecord rec;
    EXPECT_EQ(rec.snapshot_sequence, 0u);
    EXPECT_EQ(rec.schema_version, 1u);
    EXPECT_EQ(rec.trigger, PassivationRecord::Trigger::kIdle);
}

TEST(PassivationRecordTest, TriggerValues) {
    EXPECT_EQ(static_cast<uint8_t>(PassivationRecord::Trigger::kIdle), 0);
    EXPECT_EQ(static_cast<uint8_t>(PassivationRecord::Trigger::kSelf), 1);
    EXPECT_EQ(static_cast<uint8_t>(PassivationRecord::Trigger::kMemoryPressure), 2);
    EXPECT_EQ(static_cast<uint8_t>(PassivationRecord::Trigger::kCli), 3);
}
