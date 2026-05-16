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
#include <iostream>
#include <string>

using namespace hpactor::config;

#ifndef TEST_DATA_DIR
#    define TEST_DATA_DIR "tests/data/toml"
#endif
static const std::string DATA_DIR = TEST_DATA_DIR;

// ---------------------------------------------------------------------------
// Test 1: Defaults when [system.shutdown] section is absent
// ---------------------------------------------------------------------------
void test_parse_default_shutdown_config() {
    std::string path = DATA_DIR + "/minimal.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();

    // Defaults when [system.shutdown] table is absent
    assert(model.system.default_drain_policy == "Drain");
    assert(model.system.default_drain_timeout_ms == 30000);
    assert(model.system.shutdown_ingress_timeout_ms == 5000);
    assert(model.system.shutdown_cluster_leave_timeout_ms == 10000);
    assert(model.system.shutdown_force_after_timeout == true);

    std::cout << "[PASS] test_parse_default_shutdown_config\n";
}

// ---------------------------------------------------------------------------
// Test 2: Custom values from [system.shutdown] section
// ---------------------------------------------------------------------------
void test_parse_custom_shutdown_config() {
    std::string path = DATA_DIR + "/shutdown_config.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();

    assert(model.system.default_drain_policy == "DropUserMessages");
    assert(model.system.default_drain_timeout_ms == 15000);
    assert(model.system.shutdown_ingress_timeout_ms == 10000);
    assert(model.system.shutdown_cluster_leave_timeout_ms == 20000);
    assert(model.system.shutdown_force_after_timeout == false);

    std::cout << "[PASS] test_parse_custom_shutdown_config\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== test_shutdown_config ===\n";

    test_parse_default_shutdown_config();
    test_parse_custom_shutdown_config();

    std::cout << "\nAll tests passed!\n";
    return 0;
}
