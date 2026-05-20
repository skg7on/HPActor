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

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

using namespace hpactor::config;

#ifndef TEST_DATA_DIR
#    define TEST_DATA_DIR "tests/data/toml"
#endif
static const std::string DATA_DIR = TEST_DATA_DIR;

// ---------------------------------------------------------------------------
// Test 1: System-level mailbox defaults parsed correctly
// ---------------------------------------------------------------------------
void test_system_mailbox_defaults() {
    std::string path = DATA_DIR + "/mailbox_config.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    auto& mb = model.system.mailbox;

    assert(mb.default_capacity == 64);
    assert(mb.default_byte_capacity == 0);
    assert(mb.default_policy == hpactor::mailbox::OverflowPolicy::DeadLetter);
    assert(std::abs(mb.high_watermark - 0.75) < 0.001);
    assert(std::abs(mb.low_watermark - 0.25) < 0.001);
    assert(mb.protected_system_messages == 8);
    assert(mb.backpressure_mode ==
           hpactor::mailbox::BackpressureMode::LocalAndRemoteSignal);

    std::cout << "[PASS] test_system_mailbox_defaults\n";
}

// ---------------------------------------------------------------------------
// Test 2: System-level dead_letters config parsed correctly
// ---------------------------------------------------------------------------
void test_system_dead_letters() {
    std::string path = DATA_DIR + "/mailbox_config.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    auto& dl = model.system.dead_letters;

    assert(dl.enabled == true);
    assert(dl.capacity == 16);
    assert(dl.max_payload_sample_bytes == 12);
    assert(dl.overflow_policy ==
           hpactor::mailbox::DeadLetterOverflowPolicy::MetadataOnly);
    assert(dl.store_payload == false);
    // Defaults for fields not in TOML
    assert(dl.byte_capacity == 0);
    assert(dl.alert_on_first_failure == false);
    assert(dl.alert_threshold_per_minute == 100);

    std::cout << "[PASS] test_system_dead_letters\n";
}

// ---------------------------------------------------------------------------
// Test 3: Actor-level mailbox policy and capacity parsed correctly
// ---------------------------------------------------------------------------
void test_actor_mailbox_policy() {
    std::string path = DATA_DIR + "/mailbox_config.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    assert(model.actors.size() == 1);

    auto& actor = model.actors[0];
    assert(actor.id == "worker");
    assert(actor.behavior == "WorkerActor");
    assert(actor.mailbox_capacity == 7);

    // Actor-level mailbox policy
    assert(actor.mailbox.policy == hpactor::mailbox::OverflowPolicy::DropNewest);
    assert(actor.mailbox.priority_aware == true);
    assert(actor.mailbox.max_overflow_depth == 3);

    std::cout << "[PASS] test_actor_mailbox_policy\n";
}

// ---------------------------------------------------------------------------
// Test 4: Default values when mailbox section is absent
// ---------------------------------------------------------------------------
void test_mailbox_defaults_when_absent() {
    // Use minimal.toml which has no mailbox or dead_letters sections
    std::string path = DATA_DIR + "/minimal.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();

    // System mailbox defaults
    auto& mb = model.system.mailbox;
    assert(mb.default_capacity == 1024);
    assert(mb.default_policy == hpactor::mailbox::OverflowPolicy::RejectNewest);
    assert(std::abs(mb.high_watermark - 0.80) < 0.001);
    assert(std::abs(mb.low_watermark - 0.50) < 0.001);
    assert(mb.protected_system_messages == 32);
    assert(mb.backpressure_mode ==
           hpactor::mailbox::BackpressureMode::LocalAndRemoteSignal);

    // Dead letters defaults
    auto& dl = model.system.dead_letters;
    assert(dl.enabled == true);
    assert(dl.capacity == 4096);
    assert(dl.max_payload_sample_bytes == 512);
    assert(dl.overflow_policy ==
           hpactor::mailbox::DeadLetterOverflowPolicy::DropOldestRecord);
    assert(dl.store_payload == true);

    // Actor mailbox defaults (when no [actor.mailbox] table)
    for (const auto& actor : model.actors) {
        assert(actor.mailbox.policy ==
               hpactor::mailbox::OverflowPolicy::RejectNewest);
        assert(actor.mailbox.priority_aware == false);
        assert(actor.mailbox.max_overflow_depth == 0);
    }

    std::cout << "[PASS] test_mailbox_defaults_when_absent\n";
}

// ---------------------------------------------------------------------------
// Test 5: System-level override parses scheduler_threads
// ---------------------------------------------------------------------------
void test_system_scheduler_threads() {
    std::string path = DATA_DIR + "/mailbox_config.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    assert(model.system.scheduler_threads == 2);
    assert(model.system.version == "1.0");

    std::cout << "[PASS] test_system_scheduler_threads\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== test_mailbox_config ===\n";

    test_system_mailbox_defaults();
    test_system_dead_letters();
    test_actor_mailbox_policy();
    test_mailbox_defaults_when_absent();
    test_system_scheduler_threads();

    std::cout << "\nAll tests passed!\n";
    return 0;
}
