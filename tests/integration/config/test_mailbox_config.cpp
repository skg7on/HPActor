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

#include <hpactor/config/toml_parser.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <string>

using namespace hpactor::config;

#ifndef TEST_DATA_DIR
#    define TEST_DATA_DIR "tests/data/toml"
#endif
static const std::string DATA_DIR = TEST_DATA_DIR;

// ---------------------------------------------------------------------------
// Test 1: System-level mailbox defaults parsed correctly
// ---------------------------------------------------------------------------
TEST(MailboxConfigTest, SystemMailboxDefaults) {
    std::string path = DATA_DIR + "/mailbox_config.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    auto& mb = model.system.mailbox;

    EXPECT_EQ(mb.default_capacity, 64u);
    EXPECT_EQ(mb.default_byte_capacity, 0u);
    EXPECT_EQ(mb.default_policy, hpactor::mailbox::OverflowPolicy::DeadLetter);
    EXPECT_LT(std::abs(mb.high_watermark - 0.75), 0.001);
    EXPECT_LT(std::abs(mb.low_watermark - 0.25), 0.001);
    EXPECT_EQ(mb.protected_system_messages, 8u);
    EXPECT_EQ(mb.backpressure_mode,
              hpactor::mailbox::BackpressureMode::LocalAndRemoteSignal);
}

// ---------------------------------------------------------------------------
// Test 2: System-level dead_letters config parsed correctly
// ---------------------------------------------------------------------------
TEST(MailboxConfigTest, SystemDeadLetters) {
    std::string path = DATA_DIR + "/mailbox_config.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    auto& dl = model.system.dead_letters;

    EXPECT_EQ(dl.enabled, true);
    EXPECT_EQ(dl.capacity, 16u);
    EXPECT_EQ(dl.max_payload_sample_bytes, 12u);
    EXPECT_EQ(dl.overflow_policy,
              hpactor::mailbox::DeadLetterOverflowPolicy::MetadataOnly);
    EXPECT_EQ(dl.store_payload, false);
    // Defaults for fields not in TOML
    EXPECT_EQ(dl.byte_capacity, 0u);
    EXPECT_EQ(dl.alert_on_first_failure, false);
    EXPECT_EQ(dl.alert_threshold_per_minute, 100u);
}

// ---------------------------------------------------------------------------
// Test 3: Actor-level mailbox policy and capacity parsed correctly
// ---------------------------------------------------------------------------
TEST(MailboxConfigTest, ActorMailboxPolicy) {
    std::string path = DATA_DIR + "/mailbox_config.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    ASSERT_EQ(model.actors.size(), 1u);

    auto& actor = model.actors[0];
    EXPECT_EQ(actor.id, "worker");
    EXPECT_EQ(actor.behavior, "WorkerActor");
    EXPECT_EQ(actor.mailbox_capacity, 7);

    // Actor-level mailbox policy
    EXPECT_EQ(actor.mailbox.policy, hpactor::mailbox::OverflowPolicy::DropNewest);
    EXPECT_EQ(actor.mailbox.priority_aware, true);
    EXPECT_EQ(actor.mailbox.max_overflow_depth, 3u);
}

// ---------------------------------------------------------------------------
// Test 4: Default values when mailbox section is absent
// ---------------------------------------------------------------------------
TEST(MailboxConfigTest, MailboxDefaultsWhenAbsent) {
    // Use minimal.toml which has no mailbox or dead_letters sections
    std::string path = DATA_DIR + "/minimal.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();

    // System mailbox defaults
    auto& mb = model.system.mailbox;
    EXPECT_EQ(mb.default_capacity, 1024u);
    EXPECT_EQ(mb.default_policy, hpactor::mailbox::OverflowPolicy::RejectNewest);
    EXPECT_LT(std::abs(mb.high_watermark - 0.80), 0.001);
    EXPECT_LT(std::abs(mb.low_watermark - 0.50), 0.001);
    EXPECT_EQ(mb.protected_system_messages, 32u);
    EXPECT_EQ(mb.backpressure_mode,
              hpactor::mailbox::BackpressureMode::LocalAndRemoteSignal);

    // Dead letters defaults
    auto& dl = model.system.dead_letters;
    EXPECT_EQ(dl.enabled, true);
    EXPECT_EQ(dl.capacity, 4096u);
    EXPECT_EQ(dl.max_payload_sample_bytes, 512u);
    EXPECT_EQ(dl.overflow_policy,
              hpactor::mailbox::DeadLetterOverflowPolicy::DropOldestRecord);
    EXPECT_EQ(dl.store_payload, true);

    // Actor mailbox defaults (when no [actor.mailbox] table)
    for (const auto& actor : model.actors) {
        EXPECT_EQ(actor.mailbox.policy,
                  hpactor::mailbox::OverflowPolicy::RejectNewest);
        EXPECT_EQ(actor.mailbox.priority_aware, false);
        EXPECT_EQ(actor.mailbox.max_overflow_depth, 0u);
    }
}

// ---------------------------------------------------------------------------
// Test 5: System-level override parses scheduler_threads
// ---------------------------------------------------------------------------
TEST(MailboxConfigTest, SystemSchedulerThreads) {
    std::string path = DATA_DIR + "/mailbox_config.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    EXPECT_EQ(model.system.scheduler_threads, 2);
    EXPECT_EQ(model.system.version, "1.0");
}
