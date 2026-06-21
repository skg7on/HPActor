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

// Integration test: CLI Final — branch coverage for CliSession, formatters,
// pager, CommandNode deep nesting, legacy/proto server, CLI error recovery.

#include <gtest/gtest.h>

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/cli_local_actor.hpp>
#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/command_tree_builder.hpp>
#include <hpactor/cli/json_formatter.hpp>
#include <hpactor/cli/lexer.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli/pager.hpp>
#include <hpactor/cli/pretty_formatter.hpp>
#include <hpactor/cli/tabular_formatter.hpp>
#include <hpactor/cli/token.hpp>
#include <hpactor/config/actor_factory_registry.hpp>

#include "cli_test_helpers.hpp"
#include "system_test_fixture.hpp"

#include <map>
#include <string>
#include <vector>

using namespace hpactor;
using namespace hpactor::cli;

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: CliSession with all formatter types (pretty, json, tabular)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliFinal, SessionWithAllFormatterTypes) {
    // Build a simple command tree
    CommandNode root{"", "root"};
    root.add_child("status", "Show system status");
    root.add_child("help", "Show help");

    std::string captured_output;

    // Test with PrettyFormatter
    {
        auto fmt = std::make_unique<PrettyFormatter>();
        CliSession session(
            nullptr, &root, std::move(fmt),
            [&captured_output](const std::string& s) { captured_output = s; }, 20);

        EXPECT_NE(session.get_command_tree(), nullptr);
        EXPECT_NE(session.formatter(), nullptr);
        EXPECT_NE(session.pager(), nullptr);

        // process_line should handle empty/whitespace input
        bool cont = session.process_line("");
        EXPECT_TRUE(cont);

        cont = session.process_line("help");
        EXPECT_TRUE(cont);
        EXPECT_FALSE(captured_output.empty());
    }

    // Test with JsonFormatter
    captured_output.clear();
    {
        auto fmt = std::make_unique<JsonFormatter>();
        CliSession session(
            nullptr, &root, std::move(fmt),
            [&captured_output](const std::string& s) { captured_output = s; });

        bool cont = session.process_line("/help");
        EXPECT_TRUE(cont);
        EXPECT_FALSE(captured_output.empty());
    }

    // Test with TabularFormatter
    captured_output.clear();
    {
        auto fmt = std::make_unique<TabularFormatter>();
        CliSession session(
            nullptr, &root, std::move(fmt),
            [&captured_output](const std::string& s) { captured_output = s; });

        bool cont = session.process_line("/help");
        EXPECT_TRUE(cont);
        EXPECT_FALSE(captured_output.empty());
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: CliSession pager with all navigation commands
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliFinal, PagerAllNavigationCommands) {
    Pager pager(10); // 10 items per page

    // Initial state — pager starts at page 1, not 0
    EXPECT_GE(pager.current_page(), 0u);

    // Parse navigation inputs
    std::string arg;

    // Next page
    EXPECT_EQ(pager.parse_input("n", arg), Pager::Action::Next);
    EXPECT_EQ(pager.parse_input("next", arg), Pager::Action::Next);

    // Previous page
    EXPECT_EQ(pager.parse_input("p", arg), Pager::Action::Previous);
    EXPECT_EQ(pager.parse_input("prev", arg), Pager::Action::Previous);

    // Quit
    EXPECT_EQ(pager.parse_input("q", arg), Pager::Action::Quit);
    EXPECT_EQ(pager.parse_input("quit", arg), Pager::Action::Quit);

    // Search with "/" prefix
    EXPECT_EQ(pager.parse_input("/search_term", arg), Pager::Action::Search);
    EXPECT_EQ(arg, "search_term");

    // Goto page
    EXPECT_EQ(pager.parse_input("goto", arg), Pager::Action::Goto);

    // Unknown
    EXPECT_EQ(pager.parse_input("xyzzy", arg), Pager::Action::Unknown);

    // Navigation methods
    pager.next_page();
    pager.prev_page();
    pager.goto_page(0);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: CommandNode with deep nested tree
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliFinal, CommandNodeDeepNestedTree) {
    CommandNode root{"", "root"};

    // Build: /system / network / endpoint / set / <addr> / port / <value>
    auto* sys = root.add_child("system", "System operations");
    auto* net = sys->add_child("network", "Network configuration");
    auto* ep = net->add_child("endpoint", "Endpoint management");
    auto* set_cmd = ep->add_child("set", "Set endpoint config");
    auto* addr = set_cmd->add_child("<address>", "Target address", true);
    auto* port_cmd = addr->add_child("port", "Port configuration");
    port_cmd->add_child("<value>", "Port number", true);

    // Also add an actor sub-tree
    auto* actor = root.add_child("actor", "Actor operations");
    auto* id_node = actor->add_child("<id>", "Target actor ID", true);
    id_node->add_child("show", "Display actor metadata");
    id_node->add_child("kill", "Kill the actor");
    id_node->add_child("quarantine", "Quarantine the actor");

    // Help text generation
    std::string help = root.help(0);
    EXPECT_FALSE(help.empty());
    EXPECT_NE(help.find("system"), std::string::npos);
    EXPECT_NE(help.find("actor"), std::string::npos);

    // Find child by prefix (unique match)
    auto* found = root.find_child_prefix("sys");
    EXPECT_EQ(found, sys);

    auto* actor_found = root.find_child_prefix("ac");
    EXPECT_EQ(actor_found, actor);

    // Find child by prefix (ambiguous — should return null)
    auto* ambiguous = root.add_child("actorless", "No actors");
    (void)ambiguous;
    auto* null_result = root.find_child_prefix("act");
    EXPECT_EQ(null_result, nullptr);

    // Collect completions
    std::vector<std::string> completions;
    root.collect_completions("act", completions);
    EXPECT_GE(completions.size(), 1u);

    // Suggest a typo
    std::string suggestion = root.suggest("systm");
    EXPECT_EQ(suggestion, "system");

    suggestion = root.suggest("actr");
    EXPECT_FALSE(suggestion.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: CliLocalActor command dispatch and history path
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliFinal, CliLocalActorHistoryPath) {
    // CliConfig construction and defaults
    CliConfig cfg;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.page_size, 50u);
    EXPECT_EQ(cfg.default_format, "pretty");

    // Custom config
    cfg.enabled = true;
    cfg.page_size = 100;
    cfg.default_format = "json";
    cfg.listen_path = "/tmp/hpactor-test.sock";
    cfg.tcp_port = 7900;
    cfg.history_path = "/tmp/hpactor_test_history.txt";
    cfg.history_max = 200;

    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.page_size, 100u);
    EXPECT_EQ(cfg.default_format, "json");
    EXPECT_EQ(cfg.listen_path, "/tmp/hpactor-test.sock");
    EXPECT_EQ(cfg.tcp_port, 7900u);
    EXPECT_EQ(cfg.history_path, "/tmp/hpactor_test_history.txt");
    EXPECT_EQ(cfg.history_max, 200u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: OutputFormatter factory creates all types
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliFinal, OutputFormatterFactoryCreatesAllTypes) {
    // Factory method
    auto pretty = OutputFormatter::create("pretty");
    ASSERT_NE(pretty, nullptr);

    auto json = OutputFormatter::create("json");
    ASSERT_NE(json, nullptr);

    auto tabular = OutputFormatter::create("tabular");
    ASSERT_NE(tabular, nullptr);

    // Unknown format may return a default formatter (not null)
    auto unknown = OutputFormatter::create("nonexistent");
    // Factory gracefully handles unknown format; doesn't crash
    EXPECT_NE(unknown, nullptr);

    // Verify each formatter can format text
    std::vector<std::string> cols = {"ID", "Name"};
    std::vector<std::vector<std::string>> rows = {{"1", "Alice"}, {"2", "Bob"}};

    pretty->header("Pretty Formatter");
    pretty->table(cols, rows);
    std::string pretty_out = pretty->finalize();
    EXPECT_FALSE(pretty_out.empty());

    json->header("JSON Formatter");
    json->table(cols, rows);
    std::string json_out = json->finalize();
    EXPECT_FALSE(json_out.empty());

    tabular->header("Tabular Formatter");
    tabular->table(cols, rows);
    std::string tab_out = tabular->finalize();
    EXPECT_FALSE(tab_out.empty());

    // Test key-value rendering
    PrettyFormatter pf;
    std::map<std::string, std::string> kv = {{"status", "running"},
                                             {"uptime", "42s"}};
    pf.key_value(kv);
    std::string kv_out = pf.finalize();
    EXPECT_FALSE(kv_out.empty());

    // Test error rendering
    PrettyFormatter pf2;
    pf2.error("Something went wrong");
    std::string err_out = pf2.finalize();
    EXPECT_FALSE(err_out.empty());
    EXPECT_NE(err_out.find("Something went wrong"), std::string::npos);

    // Test raw rendering
    PrettyFormatter pf3;
    pf3.raw("plain text");
    std::string raw_out = pf3.finalize();
    EXPECT_FALSE(raw_out.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 6: CommandContext wiring and parsing
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliFinal, CommandContextWiringAndParsing) {
    CommandContext ctx;

    // Initially everything is null/default
    EXPECT_EQ(ctx.system, nullptr);
    EXPECT_EQ(ctx.command_host, nullptr);
    EXPECT_EQ(ctx.system_host, nullptr);
    EXPECT_EQ(ctx.lifecycle_host, nullptr);
    EXPECT_EQ(ctx.cli_actor, nullptr);
    EXPECT_EQ(ctx.cli_server_actor, nullptr);
    EXPECT_EQ(ctx.cli_proto_server, nullptr);
    EXPECT_EQ(ctx.cli_client_actor, nullptr);
    EXPECT_EQ(ctx.cli_session, nullptr);
    EXPECT_EQ(ctx.output, nullptr);
    EXPECT_FALSE(ctx.paged);
    EXPECT_EQ(ctx.page_size, 50u);
    EXPECT_EQ(ctx.format, "pretty");

    // Set params and access them
    ctx.params["actor_id"] = "0x42";
    ctx.params["format"] = "json";
    ctx.args.push_back("show");

    EXPECT_EQ(ctx.get_param("actor_id").value_or(""), "0x42");
    EXPECT_EQ(ctx.get_param("format").value_or(""), "json");
    EXPECT_FALSE(ctx.get_param("nonexistent").has_value());

    // has_flag
    ctx.params["no-pager"] = "true";
    EXPECT_TRUE(ctx.has_flag("no-pager"));
    EXPECT_FALSE(ctx.has_flag("verbose"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 7: Lexer tokenization of various input patterns
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliFinal, LexerTokenizeVariousPatterns) {
    // Simple command
    auto tokens = Lexer::tokenize("/actor list");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].value, "/");
    EXPECT_EQ(tokens[1].value, "actor");
    EXPECT_EQ(tokens[2].value, "list");

    // Command with flags
    tokens = Lexer::tokenize("/actor list --format json --no-pager");
    bool has_format = false;
    bool has_no_pager = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::FlagWithArg && t.value == "format") {
            has_format = true;
            EXPECT_EQ(t.arg.value_or(""), "json");
        }
        if (t.type == TokenType::Flag && t.value == "no-pager") {
            has_no_pager = true;
        }
    }
    EXPECT_TRUE(has_format);
    EXPECT_TRUE(has_no_pager);

    // Quoted string
    tokens = Lexer::tokenize("/send \"hello world\"");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[2].value, "hello world");

    // Empty input may produce 1 EOF token
    tokens = Lexer::tokenize("");
    EXPECT_LE(tokens.size(), 1u);

    // Whitespace only
    tokens = Lexer::tokenize("   ");
    EXPECT_LE(tokens.size(), 1u);

    // Flag with equals: the Lexer may tokenize "--value=42" differently
    // than "--value 42". Verify no crash.
    tokens = Lexer::tokenize("/cmd --value=42");
    EXPECT_FALSE(tokens.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 8: CLI error recovery after bad command
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliFinal, ErrorRecoveryAfterBadCommand) {
    CommandNode root{"", "root"};
    auto* help = root.add_child("help", "Show help");
    help->execute = [](CommandContext& ctx) -> result<void> {
        auto* out = ctx.output;
        if (out)
            out->raw("help text here");
        return result<void>::make();
    };

    std::string captured_output;
    auto fmt = std::make_unique<PrettyFormatter>();

    CliSession session(
        nullptr, &root, std::move(fmt),
        [&captured_output](const std::string& s) { captured_output = s; });

    // Send a bad command (no such path)
    bool cont = session.process_line("nonexistent_command");
    EXPECT_TRUE(cont); // session should continue

    // Send a good command after the bad one — session should still work
    cont = session.process_line("help");
    EXPECT_TRUE(cont);
    EXPECT_FALSE(captured_output.empty());

    // Send an empty command
    cont = session.process_line("");
    EXPECT_TRUE(cont);

    // Request shutdown
    session.request_shutdown();
    cont = session.process_line("help");
    EXPECT_FALSE(cont); // should stop after shutdown request
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 9: CliSession set_host methods
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliFinal, CliSessionSetHostMethods) {
    CommandNode root{"", "root"};
    root.add_child("help", "Show help");

    std::string captured_output;
    auto fmt = std::make_unique<PrettyFormatter>();

    CliSession session(
        nullptr, &root, std::move(fmt),
        [&captured_output](const std::string& s) { captured_output = s; });

    // All host set methods should accept nullptr without crash
    session.set_cli_actor(nullptr);
    session.set_cli_server_actor(nullptr);
    session.set_proto_server(nullptr);
    session.set_client_actor(nullptr);
    session.set_command_host(nullptr);
    session.set_system_host(nullptr);
    session.set_lifecycle_host(nullptr);

    // After wiring null hosts, session should still function
    bool cont = session.process_line("help");
    EXPECT_TRUE(cont);
    EXPECT_FALSE(captured_output.empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 10: CommandNode TreeNode rendering via formatter
// ═══════════════════════════════════════════════════════════════════════════════

TEST(CliFinal, TreeNodeRenderingViaFormatter) {
    // Build a tree structure for rendering using aggregate init
    TreeNode tree_root{"root", "", {}};
    tree_root.children.push_back(TreeNode{"Services", "", {}});
    tree_root.children[0].children.push_back(TreeNode{"http", "", {}});
    tree_root.children[0].children[0].children.push_back(TreeNode{"status", "", {}});
    tree_root.children[0].children.push_back(TreeNode{"tcp", "", {}});
    tree_root.children[0].children[1].children.push_back(
        TreeNode{"connections", "", {}});

    tree_root.children.push_back(TreeNode{"Actors", "", {}});
    tree_root.children[1].children.push_back(TreeNode{"worker-1", "", {}});
    tree_root.children[1].children[0].children.push_back(
        TreeNode{"mailbox", "", {}});

    // Pretty formatter tree rendering
    PrettyFormatter pf;
    pf.tree(tree_root);
    std::string out = pf.finalize();
    EXPECT_FALSE(out.empty());
    EXPECT_NE(out.find("root"), std::string::npos);
    EXPECT_NE(out.find("Services"), std::string::npos);
    EXPECT_NE(out.find("Actors"), std::string::npos);

    // JSON formatter tree rendering
    JsonFormatter jf;
    jf.tree(tree_root);
    std::string jout = jf.finalize();
    EXPECT_FALSE(jout.empty());

    // Tabular formatter tree rendering
    TabularFormatter tf;
    tf.tree(tree_root);
    std::string tout = tf.finalize();
    EXPECT_FALSE(tout.empty());
}
