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

#include <hpactor/config/binary_loader.hpp>
#include <hpactor/config/binary_serializer.hpp>
#include <hpactor/config/toml_parser.hpp>

#include <gtest/gtest.h>

#include <cstdio>

#ifndef TEST_DATA_DIR
#    define TEST_DATA_DIR "tests/data/toml"
#endif
static const std::string DATA = std::string(TEST_DATA_DIR) + "/";

using namespace hpactor::config;

// ---------------------------------------------------------------------------
// Test 1: Minimal TOML roundtrip
// ---------------------------------------------------------------------------
TEST(BinaryRoundtripTest, RoundtripMinimal) {
    auto parse_result = TomlParser::parse(DATA + "minimal.toml");
    ASSERT_TRUE(parse_result.has_value());
    // Verify parser output before serialization
    EXPECT_EQ(parse_result.value().system.version, "1.0");

    auto binary = serialize_topology(parse_result.value());
    EXPECT_FALSE(binary.empty());

    // Write to temp file
    std::string tmp_path = "/tmp/hpactor_roundtrip_minimal.bin";
    FILE* f = fopen(tmp_path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(binary.data(), 1, binary.size(), f);
    fclose(f);

    auto load_result = load_binary_topology(tmp_path);
    ASSERT_TRUE(load_result.has_value());

    auto& loaded = load_result.value();
    EXPECT_EQ(loaded.actors.size(), 1u);
    EXPECT_EQ(loaded.actors[0].id, "echo_server");
    EXPECT_EQ(loaded.actors[0].behavior, "EchoActor");
    EXPECT_EQ(loaded.system.version, "1.0");

    remove(tmp_path.c_str());
}

// ---------------------------------------------------------------------------
// Test 2: Supervisor tree roundtrip -- verifies supervisor refs survive
// ---------------------------------------------------------------------------
TEST(BinaryRoundtripTest, RoundtripSupervisorTree) {
    auto parse_result = TomlParser::parse(DATA + "supervisor_tree.toml");
    ASSERT_TRUE(parse_result.has_value());

    auto binary = serialize_topology(parse_result.value());

    std::string tmp_path = "/tmp/hpactor_roundtrip_supervisor.bin";
    FILE* f = fopen(tmp_path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(binary.data(), 1, binary.size(), f);
    fclose(f);

    auto load_result = load_binary_topology(tmp_path);
    ASSERT_TRUE(load_result.has_value());

    auto& loaded = load_result.value();
    EXPECT_EQ(loaded.actors.size(), 3u);
    // Verify parent-child relationships
    EXPECT_EQ(loaded.actors[0].id, "parent");
    EXPECT_TRUE(loaded.actors[0].supervisor.empty());

    bool found_child = false;
    for (const auto& a : loaded.actors) {
        if (a.id == "child_1") {
            EXPECT_EQ(a.supervisor, "parent");
            found_child = true;
        }
    }
    EXPECT_TRUE(found_child);

    remove(tmp_path.c_str());
}

// ---------------------------------------------------------------------------
// Test 3: Template inheritance roundtrip -- verifies args merge survives
// ---------------------------------------------------------------------------
TEST(BinaryRoundtripTest, RoundtripTemplate) {
    auto parse_result = TomlParser::parse(DATA + "template_inherit.toml");
    ASSERT_TRUE(parse_result.has_value());

    auto binary = serialize_topology(parse_result.value());

    std::string tmp_path = "/tmp/hpactor_roundtrip_template.bin";
    FILE* f = fopen(tmp_path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(binary.data(), 1, binary.size(), f);
    fclose(f);

    auto load_result = load_binary_topology(tmp_path);
    ASSERT_TRUE(load_result.has_value());

    auto& loaded = load_result.value();
    EXPECT_EQ(loaded.actors.size(), 1u);
    auto& w1 = loaded.actors[0];
    EXPECT_EQ(w1.id, "w1");
    EXPECT_EQ(w1.behavior, "WorkerActor");
    EXPECT_EQ(w1.args.at("pool"), "gpu");
    EXPECT_EQ(w1.args.at("timeout"), "30");

    remove(tmp_path.c_str());
}
