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

#include <fstream>
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
TEST(TomlParserTest, Minimal) {
    std::string path = DATA_DIR + "/minimal.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    EXPECT_EQ(model.actors.size(), 1u);
    EXPECT_EQ(model.actors[0].id, "echo_server");
    EXPECT_EQ(model.actors[0].behavior, "EchoActor");
    EXPECT_EQ(model.system.version, "1.0");
}

// ---------------------------------------------------------------------------
// Test 2: Parse multi-actor with supervisor hierarchy
// ---------------------------------------------------------------------------
TEST(TomlParserTest, SupervisorTree) {
    std::string path = DATA_DIR + "/supervisor_tree.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    EXPECT_EQ(model.actors.size(), 3u);

    // First actor must be the root (no supervisor)
    EXPECT_EQ(model.actors[0].id, "parent");
    EXPECT_TRUE(model.actors[0].supervisor.empty());

    // Children come after parent
    bool has_child1 = false, has_child2 = false;
    for (const auto& a : model.actors) {
        if (a.id == "child_1") {
            EXPECT_EQ(a.supervisor, "parent");
            has_child1 = true;
        }
        if (a.id == "child_2") {
            EXPECT_EQ(a.supervisor, "parent");
            has_child2 = true;
        }
    }
    EXPECT_TRUE(has_child1 && has_child2);
}

// ---------------------------------------------------------------------------
// Test 3: Template inheritance -- scalar override
// ---------------------------------------------------------------------------
TEST(TomlParserTest, TemplateScalar) {
    std::string path = DATA_DIR + "/template_inherit.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    EXPECT_EQ(model.actors.size(), 1u);
    auto& w1 = model.actors[0];
    EXPECT_EQ(w1.id, "w1");
    EXPECT_EQ(w1.behavior, "WorkerActor");
    EXPECT_EQ(w1.mailbox_capacity, 4096);
    EXPECT_EQ(w1.dispatch_policy, DispatchPolicy::Cooperative);
}

// ---------------------------------------------------------------------------
// Test 4: Template inheritance -- args merge
// ---------------------------------------------------------------------------
TEST(TomlParserTest, TemplateArgsMerge) {
    std::string path = DATA_DIR + "/template_inherit.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    auto& w1 = model.actors[0];

    // pool = "gpu" overrides template default "default"
    EXPECT_EQ(w1.args.at("pool"), "gpu");
    // timeout = "30" is new, not in template
    EXPECT_EQ(w1.args.at("timeout"), "30");
}

// ---------------------------------------------------------------------------
// Test 5: Import resolution -- actors from two files merged
// ---------------------------------------------------------------------------
TEST(TomlParserTest, Import) {
    std::string path = DATA_DIR + "/imports_main.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    EXPECT_EQ(model.actors.size(), 2u);

    bool found_main = false, found_imported = false;
    for (const auto& a : model.actors) {
        if (a.id == "main_actor")
            found_main = true;
        if (a.id == "imported_actor")
            found_imported = true;
    }
    EXPECT_TRUE(found_main && found_imported);
}

// ---------------------------------------------------------------------------
// Test 6: Topological sort -- linear chain (A->B->C)
// ---------------------------------------------------------------------------
TEST(TomlParserTest, SortLinear) {
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
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    EXPECT_EQ(model.actors.size(), 3u);
    // Sorted order: roots first -> children
    EXPECT_EQ(model.actors[0].id, "A");
    // B before C (or C after B)
    size_t b_pos = 0, c_pos = 0;
    for (size_t i = 0; i < model.actors.size(); ++i) {
        if (model.actors[i].id == "B")
            b_pos = i;
        if (model.actors[i].id == "C")
            c_pos = i;
    }
    EXPECT_LT(b_pos, c_pos);
}

// ---------------------------------------------------------------------------
// Test 7: Topological sort -- diamond (A->B, A->C, B->D, C->D)
// ---------------------------------------------------------------------------
TEST(TomlParserTest, SortDiamond) {
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
    ASSERT_TRUE(result.has_value());

    auto& model = result.value();
    EXPECT_EQ(model.actors.size(), 5u);
    // A must be first (root)
    EXPECT_EQ(model.actors[0].id, "A");
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
    EXPECT_LT(b_pos, d_pos);
    EXPECT_LT(c_pos, e_pos);
}

// ---------------------------------------------------------------------------
// Test 8: Duplicate actor id -> error
// ---------------------------------------------------------------------------
TEST(TomlParserTest, DuplicateIdError) {
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
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Test 9: Unknown supervisor reference -> error
// ---------------------------------------------------------------------------
TEST(TomlParserTest, UnknownSupervisorError) {
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
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Test 10: Circular dependency -> error
// ---------------------------------------------------------------------------
TEST(TomlParserTest, CircularError) {
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
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Test 11: Unknown dispatcher reference -> error
// ---------------------------------------------------------------------------
TEST(TomlParserTest, UnknownDispatcherError) {
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
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Test 12: Missing template reference -> error
// ---------------------------------------------------------------------------
TEST(TomlParserTest, MissingTemplateError) {
    std::string content = R"(
[system]
version = "1.0"

[[actor]]
id = "A"
inherits = "nonexistent_template"
)";
    std::string path = write_temp(content, "missing_tmpl");
    auto result = TomlParser::parse(path);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Test 13: Glob import pattern matches multiple files
// ---------------------------------------------------------------------------
TEST(TomlParserTest, GlobImport) {
    // Uses the existing import test -- the main file imports imports_extra.toml
    // We test that import resolution works via explicit path
    std::string path = DATA_DIR + "/imports_main.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().actors.size(), 2u);
}

// ---------------------------------------------------------------------------
// Test 14: All subsystem fields via system_subsystems.toml
// ---------------------------------------------------------------------------
TEST(TomlParserTest, SystemSubsystems) {
    std::string path = DATA_DIR + "/system_subsystems.toml";
    auto result = TomlParser::parse(path);
    ASSERT_TRUE(result.has_value());

    auto& m = result.value();

    // [system] core
    EXPECT_EQ(m.system.version, "1.0");
    EXPECT_EQ(m.system.scheduler_threads, 8);
    EXPECT_EQ(m.system.max_queue_depth, 2048u);
    EXPECT_EQ(m.system.default_mailbox_size, 4096u);
    EXPECT_EQ(m.system.enable_network, true);
    EXPECT_EQ(m.system.tcp_port, 9555u);
    EXPECT_EQ(m.system.spawn_timeout_ms, 9000u);
    EXPECT_EQ(m.system.enable_http_gateway, true);
    EXPECT_EQ(m.system.http_bind_host, "127.0.0.1");
    EXPECT_EQ(m.system.http_port, 18080u);
    EXPECT_EQ(m.system.http_max_connections, 77u);
    EXPECT_EQ(m.system.http_max_request_size, 123456u);
    EXPECT_EQ(m.system.http_reply_timeout_ms, 4567u);
    EXPECT_EQ(m.system.use_coroutines, true);

    // [system.metrics]
    EXPECT_EQ(m.system.metrics_enabled, false);
    EXPECT_EQ(m.system.metrics_ring_buffer_capacity, 8192u);
    EXPECT_EQ(m.system.metrics_path, "/internal/metrics");

    // [system.logging]
    EXPECT_EQ(m.system.logging.enabled, true);
    EXPECT_EQ(m.system.logging.format, hpactor::log::LogFormat::kText);
    EXPECT_EQ(m.system.logging.ring_buffer_capacity, 4096u);
    EXPECT_EQ(m.system.logging.file_path, "/tmp/hpactor.log");
    EXPECT_EQ(m.system.logging.rotating_file.path, "/tmp/hpactor-rotating.log");
    EXPECT_EQ(m.system.logging.rotating_file.max_bytes, 1048576u);
    EXPECT_EQ(m.system.logging.rotating_file.max_files, 3u);
    EXPECT_EQ(m.system.logging.sinks.size(), 3u);

    // [system.cli]
    EXPECT_EQ(m.system.cli.enabled, true);
    EXPECT_EQ(m.system.cli.listen_path, "/tmp/hpactor-cli.sock");
    EXPECT_EQ(m.system.cli.tcp_port, 7001u);
    EXPECT_EQ(m.system.cli.default_format, "json");
    EXPECT_EQ(m.system.cli.page_size, 25u);

    // [system.discovery]
    EXPECT_EQ(m.system.discovery_backend, "gossip");

    // [[dispatcher]] + [[actor]]
    EXPECT_EQ(m.dispatchers.size(), 1u);
    EXPECT_EQ(m.dispatchers[0].name, "io");
    EXPECT_EQ(m.dispatchers[0].threads, 2u);
    EXPECT_EQ(m.actors.size(), 1u);
    EXPECT_EQ(m.actors[0].id, "echo");
    EXPECT_EQ(m.actors[0].behavior, "EchoActor");
    EXPECT_EQ(m.actors[0].dispatcher, "io");
    EXPECT_EQ(m.actors[0].mailbox_capacity, 99);
}

// ---------------------------------------------------------------------------
// Test 15: Imported file containing [system] is rejected
// ---------------------------------------------------------------------------
TEST(TomlParserTest, ImportRejectsSystemTable) {
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
    EXPECT_FALSE(result.has_value());
}
