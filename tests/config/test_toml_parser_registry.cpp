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

#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

using namespace hpactor;
using namespace hpactor::config;

// ---------------------------------------------------------------------------
// Helper: write inline TOML to a temp file
// ---------------------------------------------------------------------------
static std::string write_temp(const std::string& content, const std::string& name) {
    std::string path = "/tmp/hpactor_test_" + name + ".toml";
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

// ---------------------------------------------------------------------------
// Custom test parsers — defined at file scope to test self-registration
// ---------------------------------------------------------------------------
namespace {

class CustomVersionParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.test_custom";
    static constexpr int kOrder = 1000;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto custom = system.table("test_custom");
        if (!custom.valid())
            return result<void>::make();
        out.version = custom.read_string("version_override", out.version);
        return result<void>::make();
    }
};

class DuplicateCustomVersionParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.test_custom";
    static constexpr int kOrder = 1001;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& /*system*/, SystemDef& /*out*/,
                       TomlParseContext& /*ctx*/) const override {
        return result<void>::make();
    }
};

const TomlSystemParserRegistration<CustomVersionParser> kRegisterCustomVersionParser;
const TomlSystemParserRegistration<DuplicateCustomVersionParser> kRegisterDuplicateCustomVersionParser;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test 1: Custom parser is registered, duplicate is rejected
// ---------------------------------------------------------------------------
void test_custom_parser_registration() {
    assert(kRegisterCustomVersionParser.registered());
    assert(!kRegisterDuplicateCustomVersionParser.registered());
    std::cout << "[PASS] test_custom_parser_registration\n";
}

// ---------------------------------------------------------------------------
// Test 2: Custom parser overrides version field
// ---------------------------------------------------------------------------
void test_custom_parser_overrides_version() {
    std::string content = R"(
[system]
version = "1.0"

[system.test_custom]
version_override = "custom-version"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "registry_custom");
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    assert(model.system.version == "custom-version");
    assert(model.actors.size() == 1);
    assert(model.actors[0].id == "echo");
    std::cout << "[PASS] test_custom_parser_overrides_version\n";
}

// ---------------------------------------------------------------------------
// Test 3: Custom parser leaves model unchanged when section is absent
// ---------------------------------------------------------------------------
void test_custom_parser_absent_section() {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "registry_absent");
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    // Custom parser should leave version unchanged when section absent
    assert(model.system.version == "1.0");
    assert(model.actors.size() == 1);
    std::cout << "[PASS] test_custom_parser_absent_section\n";
}

// ---------------------------------------------------------------------------
// Test 4: Built-in parsers still run with custom parser registered
// ---------------------------------------------------------------------------
void test_builtin_parsers_still_run() {
    std::string content = R"(
[system]
version = "1.0"

[system.metrics]
enabled = false
ring_buffer_capacity = 1234
metrics_path = "/custom-metrics"

[[actor]]
id = "echo"
behavior = "EchoActor"
)";
    std::string path = write_temp(content, "registry_builtin");
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    assert(model.system.metrics_enabled == false);
    assert(model.system.metrics_ring_buffer_capacity == 1234);
    assert(model.system.metrics_path == "/custom-metrics");
    std::cout << "[PASS] test_builtin_parsers_still_run\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== test_toml_parser_registry ===\n";

    test_custom_parser_registration();
    test_custom_parser_overrides_version();
    test_custom_parser_absent_section();
    test_builtin_parsers_still_run();

    std::cout << "\nAll tests passed!\n";
    return 0;
}
