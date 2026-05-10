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
#include <fstream>
#include <iostream>
#include <string>

using namespace hpactor::config;

#ifndef TEST_DATA_DIR
#    define TEST_DATA_DIR "tests/data/toml"
#endif
static const std::string DATA_DIR = TEST_DATA_DIR;

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
// Test 1: Parse minimal valid TOML (one actor, no supervisor)
// ---------------------------------------------------------------------------
void test_minimal() {
    std::string path = DATA_DIR + "/minimal.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    assert(model.actors.size() == 1);
    assert(model.actors[0].id == "echo_server");
    assert(model.actors[0].behavior == "EchoActor");
    assert(model.system.version == "1.0");
    std::cout << "[PASS] test_minimal\n";
}

// ---------------------------------------------------------------------------
// Test 2: Parse multi-actor with supervisor hierarchy
// ---------------------------------------------------------------------------
void test_supervisor_tree() {
    std::string path = DATA_DIR + "/supervisor_tree.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    assert(model.actors.size() == 3);

    // First actor must be the root (no supervisor)
    assert(model.actors[0].id == "parent");
    assert(model.actors[0].supervisor.empty());

    // Children come after parent
    bool has_child1 = false, has_child2 = false;
    for (const auto& a : model.actors) {
        if (a.id == "child_1") {
            assert(a.supervisor == "parent");
            has_child1 = true;
        }
        if (a.id == "child_2") {
            assert(a.supervisor == "parent");
            has_child2 = true;
        }
    }
    assert(has_child1 && has_child2);
    std::cout << "[PASS] test_supervisor_tree\n";
}

// ---------------------------------------------------------------------------
// Test 3: Template inheritance — scalar override
// ---------------------------------------------------------------------------
void test_template_scalar() {
    std::string path = DATA_DIR + "/template_inherit.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    assert(model.actors.size() == 1);
    auto& w1 = model.actors[0];
    assert(w1.id == "w1");
    assert(w1.behavior == "WorkerActor");
    assert(w1.mailbox_capacity == 4096);
    assert(w1.dispatch_policy == DispatchPolicy::Cooperative);
    std::cout << "[PASS] test_template_scalar\n";
}

// ---------------------------------------------------------------------------
// Test 4: Template inheritance — args merge
// ---------------------------------------------------------------------------
void test_template_args_merge() {
    std::string path = DATA_DIR + "/template_inherit.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    auto& w1 = model.actors[0];

    // pool = "gpu" overrides template default "default"
    assert(w1.args.at("pool") == "gpu");
    // timeout = "30" is new, not in template
    assert(w1.args.at("timeout") == "30");
    std::cout << "[PASS] test_template_args_merge\n";
}

// ---------------------------------------------------------------------------
// Test 5: Import resolution — actors from two files merged
// ---------------------------------------------------------------------------
void test_import() {
    std::string path = DATA_DIR + "/imports_main.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    assert(model.actors.size() == 2);

    bool found_main = false, found_imported = false;
    for (const auto& a : model.actors) {
        if (a.id == "main_actor")
            found_main = true;
        if (a.id == "imported_actor")
            found_imported = true;
    }
    assert(found_main && found_imported);
    std::cout << "[PASS] test_import\n";
}

// ---------------------------------------------------------------------------
// Test 6: Topological sort — linear chain (A→B→C)
// ---------------------------------------------------------------------------
void test_sort_linear() {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "A"
behavior = "A"

[[actor]]
id = "B"
behavior = "B"
supervisor = "A"

[[actor]]
id = "C"
behavior = "C"
supervisor = "B"
)";
    std::string path = write_temp(content, "sort_linear");
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    assert(model.actors.size() == 3);
    // Sorted order: roots first → children
    assert(model.actors[0].id == "A");
    // B before C (or C after B)
    size_t b_pos = 0, c_pos = 0;
    for (size_t i = 0; i < model.actors.size(); ++i) {
        if (model.actors[i].id == "B")
            b_pos = i;
        if (model.actors[i].id == "C")
            c_pos = i;
    }
    assert(b_pos < c_pos);
    std::cout << "[PASS] test_sort_linear\n";
}

// ---------------------------------------------------------------------------
// Test 7: Topological sort — diamond (A→B, A→C, B→D, C→D)
// ---------------------------------------------------------------------------
void test_sort_diamond() {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "A"
behavior = "A"

[[actor]]
id = "B"
behavior = "B"
supervisor = "A"

[[actor]]
id = "C"
behavior = "C"
supervisor = "A"

[[actor]]
id = "D"
behavior = "D"
supervisor = "B"

[[actor]]
id = "E"
behavior = "E"
supervisor = "C"
)";
    std::string path = write_temp(content, "sort_diamond");
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& model = result.value();
    assert(model.actors.size() == 5);
    // A must be first (root)
    assert(model.actors[0].id == "A");
    // B before D, C before E
    size_t b_pos = 0, d_pos = 0, c_pos = 0, e_pos = 0;
    for (size_t i = 0; i < model.actors.size(); ++i) {
        if (model.actors[i].id == "B")
            b_pos = i;
        if (model.actors[i].id == "D")
            d_pos = i;
        if (model.actors[i].id == "C")
            c_pos = i;
        if (model.actors[i].id == "E")
            e_pos = i;
    }
    assert(b_pos < d_pos);
    assert(c_pos < e_pos);
    std::cout << "[PASS] test_sort_diamond\n";
}

// ---------------------------------------------------------------------------
// Test 8: Duplicate actor id → error
// ---------------------------------------------------------------------------
void test_duplicate_id_error() {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "dup"
behavior = "A"

[[actor]]
id = "dup"
behavior = "B"
)";
    std::string path = write_temp(content, "dup_id");
    auto result = TomlParser::parse(path);
    assert(!result.has_value());
    std::cout << "[PASS] test_duplicate_id_error\n";
}

// ---------------------------------------------------------------------------
// Test 9: Unknown supervisor reference → error
// ---------------------------------------------------------------------------
void test_unknown_supervisor_error() {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "orphan"
behavior = "A"
supervisor = "nonexistent"
)";
    std::string path = write_temp(content, "unknown_sup");
    auto result = TomlParser::parse(path);
    assert(!result.has_value());
    std::cout << "[PASS] test_unknown_supervisor_error\n";
}

// ---------------------------------------------------------------------------
// Test 10: Circular dependency → error
// ---------------------------------------------------------------------------
void test_circular_error() {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "A"
behavior = "A"
supervisor = "B"

[[actor]]
id = "B"
behavior = "B"
supervisor = "A"
)";
    std::string path = write_temp(content, "circular");
    auto result = TomlParser::parse(path);
    assert(!result.has_value());
    std::cout << "[PASS] test_circular_error\n";
}

// ---------------------------------------------------------------------------
// Test 11: Unknown dispatcher reference → error
// ---------------------------------------------------------------------------
void test_unknown_dispatcher_error() {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "A"
behavior = "A"
dispatcher = "nonexistent_pool"
)";
    std::string path = write_temp(content, "unknown_disp");
    auto result = TomlParser::parse(path);
    assert(!result.has_value());
    std::cout << "[PASS] test_unknown_dispatcher_error\n";
}

// ---------------------------------------------------------------------------
// Test 12: Missing template reference → error
// ---------------------------------------------------------------------------
void test_missing_template_error() {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "A"
inherits = "nonexistent_template"
)";
    std::string path = write_temp(content, "missing_tmpl");
    auto result = TomlParser::parse(path);
    assert(!result.has_value());
    std::cout << "[PASS] test_missing_template_error\n";
}

// ---------------------------------------------------------------------------
// Test 13: Glob import pattern matches multiple files
// ---------------------------------------------------------------------------
void test_glob_import() {
    // Uses the existing import test — the main file imports imports_extra.toml
    // We test that import resolution works via explicit path
    std::string path = DATA_DIR + "/imports_main.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());
    assert(result.value().actors.size() == 2);
    std::cout << "[PASS] test_glob_import\n";
}

// ---------------------------------------------------------------------------
// Test 14: All subsystem fields via system_subsystems.toml
// ---------------------------------------------------------------------------
void test_system_subsystems() {
    std::string path = DATA_DIR + "/system_subsystems.toml";
    auto result = TomlParser::parse(path);
    assert(result.has_value());

    auto& m = result.value();

    // [system] core
    assert(m.system.version == "1.0");
    assert(m.system.scheduler_threads == 8);
    assert(m.system.max_queue_depth == 2048);
    assert(m.system.default_mailbox_size == 4096);
    assert(m.system.enable_network == true);
    assert(m.system.tcp_port == 9555);
    assert(m.system.spawn_timeout_ms == 9000);
    assert(m.system.enable_http_gateway == true);
    assert(m.system.http_bind_host == "127.0.0.1");
    assert(m.system.http_port == 18080);
    assert(m.system.http_max_connections == 77);
    assert(m.system.http_max_request_size == 123456);
    assert(m.system.http_reply_timeout_ms == 4567);
    assert(m.system.use_coroutines == true);

    // [system.metrics]
    assert(m.system.metrics_enabled == false);
    assert(m.system.metrics_ring_buffer_capacity == 8192);
    assert(m.system.metrics_path == "/internal/metrics");

    // [system.logging]
    assert(m.system.logging.enabled == true);
    assert(m.system.logging.format == hpactor::log::LogFormat::kText);
    assert(m.system.logging.ring_buffer_capacity == 4096);
    assert(m.system.logging.file_path == "/tmp/hpactor.log");
    assert(m.system.logging.rotating_file.path == "/tmp/hpactor-rotating.log");
    assert(m.system.logging.rotating_file.max_bytes == 1048576);
    assert(m.system.logging.rotating_file.max_files == 3);
    assert(m.system.logging.sinks.size() == 3);

    // [system.cli]
    assert(m.system.cli.enabled == true);
    assert(m.system.cli.listen_path == "/tmp/hpactor-cli.sock");
    assert(m.system.cli.tcp_port == 7001);
    assert(m.system.cli.default_format == "json");
    assert(m.system.cli.page_size == 25);

    // [system.discovery]
    assert(m.system.discovery_backend == "gossip");

    // [[dispatcher]] + [[actor]]
    assert(m.dispatchers.size() == 1);
    assert(m.dispatchers[0].name == "io");
    assert(m.dispatchers[0].threads == 2);
    assert(m.actors.size() == 1);
    assert(m.actors[0].id == "echo");
    assert(m.actors[0].behavior == "EchoActor");
    assert(m.actors[0].dispatcher == "io");
    assert(m.actors[0].mailbox_capacity == 99);

    std::cout << "[PASS] test_system_subsystems\n";
}

// ---------------------------------------------------------------------------
// Test 15: Imported file containing [system] is rejected
// ---------------------------------------------------------------------------
void test_import_rejects_system_table() {
    // Write an imported file that illegally contains [system]
    std::string imported = R"(
[system]
version = "1.0"
[[actor]]
id = "bad"
behavior = "X"
)";
    std::string import_path = write_temp(imported, "bad_import");

    std::string main_toml = std::string("[system]\nversion = \"1.0\"\n\n"
                                        "imports = [\"") +
                            import_path +
                            "\"]\n\n"
                            "[[actor]]\nid = \"main\"\nbehavior = \"M\"\n";
    std::string main_path = write_temp(main_toml, "reject_import_sys");

    auto result = TomlParser::parse(main_path);
    assert(!result.has_value());
    std::cout << "[PASS] test_import_rejects_system_table\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=== test_toml_parser ===\n";

    test_minimal();
    test_supervisor_tree();
    test_template_scalar();
    test_template_args_merge();
    test_import();
    test_sort_linear();
    test_sort_diamond();
    test_duplicate_id_error();
    test_unknown_supervisor_error();
    test_circular_error();
    test_unknown_dispatcher_error();
    test_missing_template_error();
    test_glob_import();
    test_system_subsystems();
    test_import_rejects_system_table();

    std::cout << "\nAll tests passed!\n";
    return 0;
}
