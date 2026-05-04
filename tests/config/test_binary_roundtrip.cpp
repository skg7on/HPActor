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

#include <cassert>
#include <cstdio>
#include <iostream>

#ifndef TEST_DATA_DIR
#define TEST_DATA_DIR "tests/data/toml"
#endif
static const std::string DATA = std::string(TEST_DATA_DIR) + "/";

using namespace hpactor::config;

// ---------------------------------------------------------------------------
// Test 1: Minimal TOML roundtrip
// ---------------------------------------------------------------------------
void test_roundtrip_minimal() {
    auto parse_result = TomlParser::parse(DATA + "minimal.toml");
    assert(parse_result.has_value());
    // Verify parser output before serialization
    assert(parse_result.value().system.version == "1.0");

    auto binary = serialize_topology(parse_result.value());
    assert(!binary.empty());

    // Write to temp file
    std::string tmp_path = "/tmp/hpactor_roundtrip_minimal.bin";
    FILE* f = fopen(tmp_path.c_str(), "wb");
    fwrite(binary.data(), 1, binary.size(), f);
    fclose(f);

    auto load_result = load_binary_topology(tmp_path);
    assert(load_result.has_value());

    auto& loaded = load_result.value();
    assert(loaded.actors.size() == 1);
    assert(loaded.actors[0].id == "echo_server");
    assert(loaded.actors[0].behavior == "EchoActor");
    assert(loaded.system.version == "1.0");

    remove(tmp_path.c_str());
    std::cout << "[PASS] test_roundtrip_minimal\n";
}

// ---------------------------------------------------------------------------
// Test 2: Supervisor tree roundtrip — verifies supervisor refs survive
// ---------------------------------------------------------------------------
void test_roundtrip_supervisor_tree() {
    auto parse_result = TomlParser::parse(DATA + "supervisor_tree.toml");
    assert(parse_result.has_value());

    auto binary = serialize_topology(parse_result.value());

    std::string tmp_path = "/tmp/hpactor_roundtrip_supervisor.bin";
    FILE* f = fopen(tmp_path.c_str(), "wb");
    fwrite(binary.data(), 1, binary.size(), f);
    fclose(f);

    auto load_result = load_binary_topology(tmp_path);
    assert(load_result.has_value());

    auto& loaded = load_result.value();
    assert(loaded.actors.size() == 3);
    // Verify parent-child relationships
    assert(loaded.actors[0].id == "parent");
    assert(loaded.actors[0].supervisor.empty());

    bool found_child = false;
    for (const auto& a : loaded.actors) {
        if (a.id == "child_1") {
            assert(a.supervisor == "parent");
            found_child = true;
        }
    }
    assert(found_child);

    remove(tmp_path.c_str());
    std::cout << "[PASS] test_roundtrip_supervisor_tree\n";
}

// ---------------------------------------------------------------------------
// Test 3: Template inheritance roundtrip — verifies args merge survives
// ---------------------------------------------------------------------------
void test_roundtrip_template() {
    auto parse_result = TomlParser::parse(DATA + "template_inherit.toml");
    assert(parse_result.has_value());

    auto binary = serialize_topology(parse_result.value());

    std::string tmp_path = "/tmp/hpactor_roundtrip_template.bin";
    FILE* f = fopen(tmp_path.c_str(), "wb");
    fwrite(binary.data(), 1, binary.size(), f);
    fclose(f);

    auto load_result = load_binary_topology(tmp_path);
    assert(load_result.has_value());

    auto& loaded = load_result.value();
    assert(loaded.actors.size() == 1);
    auto& w1 = loaded.actors[0];
    assert(w1.id == "w1");
    assert(w1.behavior == "WorkerActor");
    assert(w1.args.at("pool") == "gpu");
    assert(w1.args.at("timeout") == "30");

    remove(tmp_path.c_str());
    std::cout << "[PASS] test_roundtrip_template\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== test_binary_roundtrip ===\n";

    test_roundtrip_minimal();
    test_roundtrip_supervisor_tree();
    test_roundtrip_template();

    std::cout << "\nAll tests passed!\n";
    return 0;
}
