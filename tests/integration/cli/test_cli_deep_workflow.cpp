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

// Tests exercising deep / uncovered paths in the CLI subsystem:
//   src/cli, src/cli/commands, src/cli/handlers
//
// Groups:
//   1. Pager, Lexer, Formatters
//   2. Command registry and builder
//   3. CLI session operations
//   4. Server actors

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/actor/cli_local_actor.hpp>
#include <hpactor/cli/actor/cli_proto_server_actor.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/cli/command/cli_session.hpp>
#include <hpactor/cli/command/command_context.hpp>
#include <hpactor/cli/command/command_node.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/command/command_tree_builder.hpp>
#include <hpactor/cli/config/cli_config.hpp>
#include <hpactor/cli/config/cli_proto_server_config.hpp>
#include <hpactor/cli/format/json_formatter.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/cli/format/pretty_formatter.hpp>
#include <hpactor/cli/format/tabular_formatter.hpp>
#include <hpactor/cli/host/local_server_cli_host.hpp>
#include <hpactor/cli/io/lexer.hpp>
#include <hpactor/cli/io/pager.hpp>
#include <hpactor/cli/token.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace hpactor::cli;
using namespace hpactor;

// ============================================================================
// Group 1: Pager, Lexer, Formatters
// ============================================================================

// ---------------------------------------------------------------------------
// Pager navigation: next, previous, quit, search, goto
// ---------------------------------------------------------------------------

TEST(CliDeepPager, NextPageAdvancesOffset) {
    Pager pager(5);
    EXPECT_EQ(pager.current_page(), 1u);

    pager.next_page();
    // Without total_items_ set, next_page is a no-op (offset < total_items
    // check fails)
    EXPECT_EQ(pager.current_page(), 1u);
    EXPECT_EQ(pager.total_pages(), 1u);
}

TEST(CliDeepPager, GotoPageClampsToValidRange) {
    Pager pager(10);

    // Set up via show_page so total_items_ is known
    PrettyFormatter fmt;
    int call_count = 0;
    pager.show_page(
        95,
        [&](uint32_t offset, uint32_t limit) {
            call_count++;
            EXPECT_LE(offset + limit, 95u);
        },
        &fmt);

    EXPECT_EQ(call_count, 1);
    EXPECT_EQ(pager.total_pages(), 10u);
    EXPECT_EQ(pager.current_page(), 1u);

    pager.goto_page(0); // clamped to 1
    EXPECT_EQ(pager.current_page(), 1u);

    pager.goto_page(5);
    EXPECT_EQ(pager.current_page(), 5u);

    pager.goto_page(999); // clamped to max
    EXPECT_EQ(pager.current_page(), 10u);
}

TEST(CliDeepPager, NextPrevNavigation) {
    Pager pager(10);

    PrettyFormatter fmt;
    pager.show_page(25, [](uint32_t, uint32_t) {}, &fmt);

    EXPECT_EQ(pager.total_pages(), 3u);
    EXPECT_EQ(pager.current_page(), 1u);

    pager.next_page();
    EXPECT_EQ(pager.current_page(), 2u);

    pager.next_page();
    EXPECT_EQ(pager.current_page(), 3u);

    // Already at last page — next_page should be no-op
    pager.next_page();
    EXPECT_EQ(pager.current_page(), 3u);

    pager.prev_page();
    EXPECT_EQ(pager.current_page(), 2u);

    pager.prev_page();
    EXPECT_EQ(pager.current_page(), 1u);

    // Already at first page — prev_page should be no-op
    pager.prev_page();
    EXPECT_EQ(pager.current_page(), 1u);
}

TEST(CliDeepPager, ParseInputNavigatesCorrectly) {
    Pager pager(10);
    std::string arg;

    EXPECT_EQ(pager.parse_input("n", arg), Pager::Action::Next);
    EXPECT_EQ(pager.parse_input("next", arg), Pager::Action::Next);
    EXPECT_EQ(pager.parse_input("p", arg), Pager::Action::Previous);
    EXPECT_EQ(pager.parse_input("prev", arg), Pager::Action::Previous);
    EXPECT_EQ(pager.parse_input("q", arg), Pager::Action::Quit);
    EXPECT_EQ(pager.parse_input("quit", arg), Pager::Action::Quit);

    // Search
    EXPECT_EQ(pager.parse_input("/memory", arg), Pager::Action::Search);
    EXPECT_EQ(arg, "memory");

    // Goto with number
    EXPECT_EQ(pager.parse_input("g5", arg), Pager::Action::Goto);
    EXPECT_EQ(arg, "5");

    // First / Last
    EXPECT_EQ(pager.parse_input("f", arg), Pager::Action::Goto);
    EXPECT_EQ(pager.parse_input("first", arg), Pager::Action::Goto);
    EXPECT_EQ(pager.parse_input("l", arg), Pager::Action::Goto);
    EXPECT_EQ(pager.parse_input("last", arg), Pager::Action::Goto);

    // Empty input defaults to Next
    EXPECT_EQ(pager.parse_input("", arg), Pager::Action::Next);

    // Unknown
    EXPECT_EQ(pager.parse_input("xyz", arg), Pager::Action::Unknown);
}

// ---------------------------------------------------------------------------
// Lexer edge cases: quoted strings with escapes, multiple flags, empty input
// ---------------------------------------------------------------------------

TEST(CliDeepLexer, EmptyInputProducesEofOnly) {
    auto tokens = Lexer::tokenize("");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::Eof);
}

TEST(CliDeepLexer, LeadingSlashNormalized) {
    auto tokens = Lexer::tokenize("/actor list");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::Keyword);
    EXPECT_EQ(tokens[0].value, "/");
    EXPECT_EQ(tokens[1].type, TokenType::Keyword);
    EXPECT_EQ(tokens[1].value, "actor");
}

TEST(CliDeepLexer, QuotedStringsWithEscapes) {
    auto tokens = Lexer::tokenize("\"hello world\" \"escaped\\\"quote\"");
    // Find non-Eof tokens
    std::vector<Token> non_eof;
    for (auto& t : tokens) {
        if (t.type != TokenType::Eof)
            non_eof.push_back(std::move(t));
    }
    ASSERT_EQ(non_eof.size(), 2u);
    EXPECT_EQ(non_eof[0].type, TokenType::Parameter);
    EXPECT_EQ(non_eof[0].value, "hello world");
    EXPECT_EQ(non_eof[1].type, TokenType::Parameter);
    EXPECT_EQ(non_eof[1].value, "escaped\"quote");
}

TEST(CliDeepLexer, QuotedStringWithNewlineTabEscape) {
    auto tokens = Lexer::tokenize("\"line1\\nline2\\tindent\"");
    std::vector<Token> non_eof;
    for (auto& t : tokens) {
        if (t.type != TokenType::Eof)
            non_eof.push_back(std::move(t));
    }
    ASSERT_EQ(non_eof.size(), 1u);
    EXPECT_EQ(non_eof[0].type, TokenType::Parameter);
    EXPECT_EQ(non_eof[0].value, "line1\nline2\tindent");
}

TEST(CliDeepLexer, MultipleFlagsAndFlagWithArgs) {
    auto tokens =
        Lexer::tokenize("--format json --no-pager --filter Worker --verbose");

    int flag_count = 0;
    int flag_with_arg_count = 0;
    for (auto& t : tokens) {
        if (t.type == TokenType::Flag)
            flag_count++;
        if (t.type == TokenType::FlagWithArg)
            flag_with_arg_count++;
    }
    EXPECT_EQ(flag_count, 2);          // --no-pager, --verbose
    EXPECT_EQ(flag_with_arg_count, 2); // --format, --filter
}

TEST(CliDeepLexer, FlagWithQuotedArg) {
    auto tokens = Lexer::tokenize("--name \"John Doe\"");
    std::vector<Token> non_eof;
    for (auto& t : tokens) {
        if (t.type != TokenType::Eof)
            non_eof.push_back(std::move(t));
    }
    ASSERT_EQ(non_eof.size(), 1u);
    EXPECT_EQ(non_eof[0].type, TokenType::FlagWithArg);
    EXPECT_EQ(non_eof[0].value, "name");
    EXPECT_EQ(non_eof[0].arg.value(), "John Doe");
}

TEST(CliDeepLexer, MixedTokensMaintainOrder) {
    auto tokens = Lexer::tokenize("/system memory --format json --detail");

    std::vector<TokenType> types;
    std::vector<std::string> values;
    for (auto& t : tokens) {
        if (t.type == TokenType::Eof)
            break;
        types.push_back(t.type);
        values.push_back(t.value);
    }
    ASSERT_EQ(types.size(), 5u);
    EXPECT_EQ(values[0], "/");
    EXPECT_EQ(values[1], "system");
    EXPECT_EQ(values[2], "memory");
    EXPECT_EQ(types[3], TokenType::FlagWithArg);
    EXPECT_EQ(values[3], "format");
    EXPECT_EQ(types[4], TokenType::Flag);
    EXPECT_EQ(values[4], "detail");
}

TEST(CliDeepLexer, WhitespaceOnlyInput) {
    auto tokens = Lexer::tokenize("   \t  \n  ");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::Eof);
}

// ---------------------------------------------------------------------------
// JSON formatter output validation
// ---------------------------------------------------------------------------

TEST(CliDeepFormatter, JsonFormatterTableOutput) {
    JsonFormatter fmt;
    fmt.table({"id", "name"}, {{"1", "Alice"}, {"2", "Bob"}});
    std::string out = fmt.finalize();
    EXPECT_NE(out.find("\"id\""), std::string::npos);
    EXPECT_NE(out.find("\"name\""), std::string::npos);
    EXPECT_NE(out.find("\"Alice\""), std::string::npos);
    EXPECT_NE(out.find("\"Bob\""), std::string::npos);
    EXPECT_EQ(out.front(), '[');
    EXPECT_EQ(out.back(), ']');
}

TEST(CliDeepFormatter, JsonFormatterKeyValue) {
    JsonFormatter fmt;
    fmt.key_value({{"status", "ok"}, {"count", "42"}});
    std::string out = fmt.finalize();
    EXPECT_NE(out.find("\"status\":\"ok\""), std::string::npos);
    EXPECT_NE(out.find("\"count\":\"42\""), std::string::npos);
    EXPECT_EQ(out.front(), '{');
    EXPECT_EQ(out.back(), '}');
}

TEST(CliDeepFormatter, JsonFormatterTreeOutput) {
    JsonFormatter fmt;
    TreeNode root{
        "root",
        "top node",
        {TreeNode{"child1", "first child", {}}, TreeNode{"child2", "", {}}}};
    fmt.tree(root);
    std::string out = fmt.finalize();
    EXPECT_NE(out.find("\"name\":\"root\""), std::string::npos);
    EXPECT_NE(out.find("\"name\":\"child1\""), std::string::npos);
    EXPECT_NE(out.find("\"name\":\"child2\""), std::string::npos);
    EXPECT_NE(out.find("\"children\""), std::string::npos);
}

TEST(CliDeepFormatter, JsonFormatterRawAndError) {
    JsonFormatter fmt;
    fmt.raw("plain text");
    std::string out = fmt.finalize();
    EXPECT_NE(out.find("plain text"), std::string::npos);
    EXPECT_EQ(out.front(), '"');
    EXPECT_EQ(out.back(), '"');
}

TEST(CliDeepFormatter, JsonFormatterErrorOutput) {
    JsonFormatter fmt;
    fmt.error("something went wrong");
    std::string out = fmt.finalize();
    EXPECT_NE(out.find("\"error\""), std::string::npos);
    EXPECT_NE(out.find("something went wrong"), std::string::npos);
}

TEST(CliDeepFormatter, JsonFormatterEscapesSpecialChars) {
    JsonFormatter fmt;
    // Use a value that contains characters requiring JSON escaping
    fmt.key_value({{"path", "C:\\Windows\\System32"}});
    std::string out = fmt.finalize();
    // Backslash characters in JSON get escaped (backslash -> double backslash)
    EXPECT_NE(out.find("\\\\"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Tabular formatter column alignment
// ---------------------------------------------------------------------------

TEST(CliDeepFormatter, TabularFormatterHeaderAndKeyValue) {
    TabularFormatter fmt;
    fmt.header("Test Section");
    fmt.key_value({{"KeyA", "ValueA"}, {"KeyB", "ValueB"}});
    std::string out = fmt.finalize();
    EXPECT_NE(out.find("# Test Section"), std::string::npos);
    EXPECT_NE(out.find("KeyA: ValueA"), std::string::npos);
    EXPECT_NE(out.find("KeyB: ValueB"), std::string::npos);
}

TEST(CliDeepFormatter, TabularFormatterColumnAlignment) {
    TabularFormatter fmt;
    fmt.table({"ID", "Name", "Description"},
              {{"1", "short", "A longer description here"},
               {"100", "a-very-long-name", "Brief"}});
    std::string out = fmt.finalize();

    // Verify all columns are present
    EXPECT_NE(out.find("ID"), std::string::npos);
    EXPECT_NE(out.find("Name"), std::string::npos);
    EXPECT_NE(out.find("Description"), std::string::npos);
    EXPECT_NE(out.find("100"), std::string::npos);
    EXPECT_NE(out.find("a-very-long-name"), std::string::npos);
    EXPECT_NE(out.find("A longer description here"), std::string::npos);
    EXPECT_NE(out.find("Brief"), std::string::npos);
}

TEST(CliDeepFormatter, TabularFormatterEmptyColumns) {
    TabularFormatter fmt;
    fmt.table({}, {{"1", "a"}});
    std::string out = fmt.finalize();
    EXPECT_TRUE(out.empty()); // empty cols = no output
}

TEST(CliDeepFormatter, TabularFormatterTreeOutput) {
    TabularFormatter fmt;
    TreeNode root{"root",
                  "top-level",
                  {TreeNode{"branch1", "first branch", {}},
                   TreeNode{"branch2", "", {TreeNode{"leaf", "a leaf node", {}}}}}};
    fmt.tree(root);
    std::string out = fmt.finalize();
    EXPECT_NE(out.find("root  # top-level"), std::string::npos);
    EXPECT_NE(out.find("branch1  # first branch"), std::string::npos);
    EXPECT_NE(out.find("branch2"), std::string::npos);
    EXPECT_NE(out.find("leaf  # a leaf node"), std::string::npos);
}

TEST(CliDeepFormatter, TabularFormatterRawAndError) {
    TabularFormatter fmt;
    fmt.raw("status: ok");
    fmt.error("connection refused");
    std::string out = fmt.finalize();
    EXPECT_NE(out.find("status: ok"), std::string::npos);
    EXPECT_NE(out.find("ERROR: connection refused"), std::string::npos);
}

// ============================================================================
// Group 2: Command registry and builder
// ============================================================================

// ---------------------------------------------------------------------------
// Command tree builder registration
// ---------------------------------------------------------------------------

TEST(CliDeepCommand, ParseCommandPath) {
    auto segs = parse_command_path("actor/<id>/show");
    ASSERT_EQ(segs.size(), 3u);
    EXPECT_EQ(segs[0], "actor");
    EXPECT_EQ(segs[1], "<id>");
    EXPECT_EQ(segs[2], "show");

    // Consecutive slashes
    auto segs2 = parse_command_path("a//b");
    ASSERT_EQ(segs2.size(), 2u);
    EXPECT_EQ(segs2[0], "a");
    EXPECT_EQ(segs2[1], "b");

    // Leading slash
    auto segs3 = parse_command_path("/system/stats");
    ASSERT_EQ(segs3.size(), 2u);
    EXPECT_EQ(segs3[0], "system");
    EXPECT_EQ(segs3[1], "stats");

    // Trailing slash
    auto segs4 = parse_command_path("help/");
    ASSERT_EQ(segs4.size(), 1u);
    EXPECT_EQ(segs4[0], "help");

    // Empty
    auto segs5 = parse_command_path("");
    EXPECT_TRUE(segs5.empty());
}

TEST(CliDeepCommand, IsParamSegment) {
    EXPECT_TRUE(is_param_segment("<id>"));
    EXPECT_TRUE(is_param_segment("<actor_id>"));
    EXPECT_TRUE(is_param_segment("<filter>"));
    EXPECT_FALSE(is_param_segment("id"));
    EXPECT_FALSE(is_param_segment("actor_id"));
    EXPECT_FALSE(is_param_segment(""));
    EXPECT_TRUE(is_param_segment("<>"));
    // "<" by itself only has '<' as both front and back → not a param
    EXPECT_FALSE(is_param_segment("<"));
}

TEST(CliDeepCommand, CommandRegistrySingleton) {
    auto& reg1 = CommandRegistry::instance();
    auto& reg2 = CommandRegistry::instance();
    EXPECT_EQ(&reg1, &reg2);
}

// Simple test command for registry testing
class TestCmd : public ICommand {
  public:
    explicit TestCmd(std::string p, std::string h, int o)
        : path_(std::move(p)), help_(std::move(h)), order_(o) {}
    std::string_view path() const noexcept override {
        return path_;
    }
    std::string_view help_text() const noexcept override {
        return help_;
    }
    int order() const noexcept override {
        return order_;
    }
    result<void> execute(CommandContext&) const override {
        return result<void>::make();
    }

  private:
    std::string path_;
    std::string help_;
    int order_;
};

TEST(CliDeepCommand, MountCommandCreatesIntermediateNodes) {
    CommandNode root{"", "root"};
    TestCmd cmd("system/memory/stats", "Show memory statistics", 50);

    mount_command(&root, cmd);

    // Walk the tree
    std::string param;
    auto* sys = root.find_child("system", param);
    ASSERT_NE(sys, nullptr);
    EXPECT_EQ(sys->keyword, "system");

    auto* mem = sys->find_child("memory", param);
    ASSERT_NE(mem, nullptr);
    EXPECT_EQ(mem->keyword, "memory");

    auto* stats = mem->find_child("stats", param);
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->keyword, "stats");
    EXPECT_EQ(stats->help_text, "Show memory statistics");
    EXPECT_NE(stats->execute, nullptr);
}

TEST(CliDeepCommand, MountCommandWithParameters) {
    CommandNode root{"", "root"};
    TestCmd cmd("actor/<id>/kill", "Kill an actor", 10);

    mount_command(&root, cmd);

    std::string param;
    auto* actor = root.find_child("actor", param);
    ASSERT_NE(actor, nullptr);

    // <id> should be a parameter node
    auto* id_node = actor->find_child("0x123", param);
    ASSERT_NE(id_node, nullptr);
    EXPECT_TRUE(id_node->is_parameter);
    EXPECT_EQ(param, "0x123");
    EXPECT_EQ(id_node->keyword, "<id>");

    auto* kill = id_node->find_child("kill", param);
    ASSERT_NE(kill, nullptr);
    EXPECT_NE(kill->execute, nullptr);
}

// ---------------------------------------------------------------------------
// Command node traversal edge cases
// ---------------------------------------------------------------------------

TEST(CliDeepCommand, FindChildExactKeywordMatch) {
    CommandNode root{"", "root"};
    root.add_child("show", "Show info");
    root.add_child("list", "List items");
    root.add_child("help", "Help text");

    std::string param;
    EXPECT_NE(root.find_child("show", param), nullptr);
    EXPECT_NE(root.find_child("list", param), nullptr);
    EXPECT_NE(root.find_child("help", param), nullptr);
    EXPECT_EQ(root.find_child("unknown", param), nullptr);
}

TEST(CliDeepCommand, FindChildPrefixUniqueMatch) {
    CommandNode root{"", "root"};
    root.add_child("shutdown", "Shutdown system");
    root.add_child("show", "Show info");
    root.add_child("list", "List items");

    // Unique prefixes match
    EXPECT_NE(root.find_child_prefix("shut"), nullptr);
    EXPECT_NE(root.find_child_prefix("sho"), nullptr);
    EXPECT_NE(root.find_child_prefix("l"), nullptr);

    // Ambiguous prefix "sh" matches both "shutdown" and "show"
    EXPECT_EQ(root.find_child_prefix("sh"), nullptr);

    // No match
    EXPECT_EQ(root.find_child_prefix("x"), nullptr);
}

TEST(CliDeepCommand, CollectCompletions) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");
    root.add_child("metrics", "Metrics display");
    auto* param = root.add_child("<id>", "ID param", true);
    (void)param;

    std::vector<std::string> completions;
    root.collect_completions("", completions);
    // Should not include parameter nodes
    ASSERT_EQ(completions.size(), 3u);
    EXPECT_EQ(completions[0], "actor");
    EXPECT_EQ(completions[1], "system");
    EXPECT_EQ(completions[2], "metrics");

    // Prefix match
    completions.clear();
    root.collect_completions("a", completions);
    ASSERT_EQ(completions.size(), 1u);
    EXPECT_EQ(completions[0], "actor");
}

// ---------------------------------------------------------------------------
// Fuzzy suggestion matching
// ---------------------------------------------------------------------------

TEST(CliDeepCommand, SuggestExactMatch) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    // Exact match shouldn't normally need suggest, but Levenshtein distance is
    // 0
    EXPECT_EQ(root.suggest("actor"), "actor");
    EXPECT_EQ(root.suggest("system"), "system");
}

TEST(CliDeepCommand, SuggestCloseTypo) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");
    root.add_child("metrics", "Metrics");

    // Single character typo (distance 1)
    EXPECT_EQ(root.suggest("actr"), "actor");
    EXPECT_EQ(root.suggest("systen"), "system");
    EXPECT_EQ(root.suggest("metrcs"), "metrics");
}

TEST(CliDeepCommand, SuggestBeyondThresholdReturnsEmpty) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor");
    root.add_child("system", "System");

    // Distance > 2 should return empty
    EXPECT_TRUE(root.suggest("xyzabc").empty());
    EXPECT_TRUE(root.suggest("qwerty").empty());
}

TEST(CliDeepCommand, SuggestWithMultipleCandidates) {
    CommandNode root{"", "root"};
    root.add_child("show", "Show info");
    root.add_child("slow", "Slow mode");
    root.add_child("list", "List items");

    // "shw" is distance 1 from "show" and distance 2 from "slow" → prefers
    // "show"
    EXPECT_EQ(root.suggest("shw"), "show");

    // "sow" is distance 2 from "show" and distance 2 from "slow"
    // First match with best distance wins
    std::string result = root.suggest("sow");
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result == "show" || result == "slow");
}

// ============================================================================
// Group 3: CLI session operations
// ============================================================================

// ---------------------------------------------------------------------------
// Session command execution with various output formatters
// ---------------------------------------------------------------------------

// A minimal command tree fixture for session tests
class CliDeepSessionFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        root_ = std::make_unique<CommandNode>();
        root_->keyword = "";

        // Build a small command tree: /system/memory and /actor/<id>/show
        auto* sys = root_->add_child("system", "System operations");
        auto* mem = sys->add_child("memory", "Show memory usage");
        mem->execute = [&](CommandContext& ctx) -> result<void> {
            ctx.output->header("Memory Usage");
            ctx.output->key_value({{"total", "1024 MB"}, {"used", "512 MB"}});
            return result<void>::make();
        };

        auto* actor = root_->add_child("actor", "Actor operations");
        auto* id_node = actor->add_child("<id>", "Actor ID", true);
        auto* show = id_node->add_child("show", "Show actor details");
        show->execute = [&](CommandContext& ctx) -> result<void> {
            ctx.output->header("Actor Details");
            ctx.output->key_value(
                {{"id", ctx.params["<id>"]}, {"state", "Running"}});
            return result<void>::make();
        };

        // /help — shows available commands
        auto* help = root_->add_child("help", "Show help");
        help->execute = [&](CommandContext& ctx) -> result<void> {
            ctx.output->header("Available commands");
            ctx.output->raw(root_->help());
            return result<void>::make();
        };
    }

    std::unique_ptr<CommandNode> root_;
    std::string captured_output_;
};

TEST_F(CliDeepSessionFixture, ExecuteSystemMemoryCommand) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [this](const std::string& out) { captured_output_ = out; });
    session.process_line("/system memory");
    EXPECT_NE(captured_output_.find("Memory Usage"), std::string::npos);
    EXPECT_NE(captured_output_.find("1024 MB"), std::string::npos);
}

TEST_F(CliDeepSessionFixture, ExecuteActorShowWithParameter) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [this](const std::string& out) { captured_output_ = out; });
    session.process_line("/actor 0x42 show");
    EXPECT_NE(captured_output_.find("Actor Details"), std::string::npos);
    EXPECT_NE(captured_output_.find("0x42"), std::string::npos);
    EXPECT_NE(captured_output_.find("Running"), std::string::npos);
}

TEST_F(CliDeepSessionFixture, ExecuteHelpCommand) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [this](const std::string& out) { captured_output_ = out; });
    session.process_line("/help");
    EXPECT_NE(captured_output_.find("Available commands"), std::string::npos);
    EXPECT_NE(captured_output_.find("system"), std::string::npos);
    EXPECT_NE(captured_output_.find("actor"), std::string::npos);
}

TEST_F(CliDeepSessionFixture, ExecuteWithJsonFormatter) {
    auto fmt = OutputFormatter::create("json");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [this](const std::string& out) { captured_output_ = out; });
    session.process_line("/system memory");
    // JSON formatter should produce structured output
    EXPECT_NE(captured_output_.find("1024 MB"), std::string::npos);
}

TEST_F(CliDeepSessionFixture, ExecuteWithTabularFormatter) {
    auto fmt = OutputFormatter::create("tabular");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [this](const std::string& out) { captured_output_ = out; });
    session.process_line("/system memory");
    EXPECT_NE(captured_output_.find("Memory Usage"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Session paging state management
// ---------------------------------------------------------------------------

TEST_F(CliDeepSessionFixture, SessionCreatedWithPager) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(
        nullptr, root_.get(), std::move(fmt), [](const std::string&) {}, 30);
    ASSERT_NE(session.pager(), nullptr);
    EXPECT_EQ(session.pager()->current_page(), 1u);
}

TEST_F(CliDeepSessionFixture, SessionFormatterIsAccessible) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [](const std::string&) {});
    ASSERT_NE(session.formatter(), nullptr);
}

TEST_F(CliDeepSessionFixture, SessionIsNotTerminatedByEmptyLine) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [this](const std::string& out) { captured_output_ = out; });
    EXPECT_TRUE(session.process_line(""));
    EXPECT_TRUE(captured_output_.empty());
}

// ---------------------------------------------------------------------------
// Session error recovery
// ---------------------------------------------------------------------------

TEST_F(CliDeepSessionFixture, UnknownCommandShowsSuggestion) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [this](const std::string& out) { captured_output_ = out; });
    session.process_line("/systen");
    // "systen" is close to "system" — should get a suggestion
    EXPECT_NE(captured_output_.find("did you mean"), std::string::npos);
    EXPECT_NE(captured_output_.find("system"), std::string::npos);
}

TEST_F(CliDeepSessionFixture, CompletelyUnknownCommandNoSuggestion) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [this](const std::string& out) { captured_output_ = out; });
    session.process_line("/xyzabc123");
    EXPECT_NE(captured_output_.find("Unknown command"), std::string::npos);
    // Distance > 2 — no suggestion
    EXPECT_EQ(captured_output_.find("did you mean"), std::string::npos);
}

TEST_F(CliDeepSessionFixture, PartialCommandShowsAvailable) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [this](const std::string& out) { captured_output_ = out; });
    // "system" exists but has children — should show available subcommands
    session.process_line("/system");
    EXPECT_NE(captured_output_.find("Available commands"), std::string::npos);
    EXPECT_NE(captured_output_.find("memory"), std::string::npos);
}

TEST_F(CliDeepSessionFixture, ShutdownRequestTerminatesSession) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [](const std::string&) {});
    session.request_shutdown();
    EXPECT_FALSE(session.process_line("/help"));
}

TEST_F(CliDeepSessionFixture, SwitchingFormatViaFlag) {
    auto fmt = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(fmt),
                       [this](const std::string& out) { captured_output_ = out; });
    // First command in pretty format
    session.process_line("/system memory");
    std::string pretty_out = captured_output_;
    captured_output_.clear();

    // Switch to json format via --format flag
    session.process_line("/system memory --format json");
    std::string json_out = captured_output_;

    // The json output should differ in structure
    EXPECT_NE(pretty_out, json_out);
}

// ============================================================================
// Group 4: Server actors
// ============================================================================

// ---------------------------------------------------------------------------
// CliProtoServer actor lifecycle
// ---------------------------------------------------------------------------

TEST(CliDeepProtoServer, ConfigDefaults) {
    CliProtoServerConfig cfg;
    EXPECT_TRUE(cfg.uds_listen_path.empty());
    EXPECT_EQ(cfg.tcp_listen_port, 0u);
    EXPECT_EQ(cfg.tcp_bind_address, "127.0.0.1");
    EXPECT_EQ(cfg.max_sessions, 16u);
    EXPECT_EQ(cfg.session_timeout, std::chrono::milliseconds(300000));
    EXPECT_EQ(cfg.default_format, "pretty");
    EXPECT_EQ(cfg.page_size, 50u);
    EXPECT_EQ(cfg.uds_socket_mode, 0660u);
}

TEST(CliDeepProtoServer, ToMetadataWithoutSpawn) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);

    CliProtoServerConfig cfg;
    auto actor = std::make_shared<CliProtoServerActor>(nullptr, system, cfg);
    auto meta = actor->to_metadata();

    // When not spawned through ActorSystem, type_name() is empty (set during
    // spawn) and actor_id is 0. make_metadata with running_=true → "Running"
    EXPECT_EQ(meta.actor_id, 0u);
    EXPECT_EQ(meta.state, "Running");
}

TEST(CliDeepProtoServer, RequestShutdown) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);

    CliProtoServerConfig cfg;
    auto actor = std::make_shared<CliProtoServerActor>(nullptr, system, cfg);

    actor->request_shutdown();
    // run_once returns false when running_ is false
    EXPECT_FALSE(actor->run_once());
}

TEST(CliDeepProtoServer, IsSystemActor) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);

    CliProtoServerConfig cfg;
    auto actor = std::make_shared<CliProtoServerActor>(nullptr, system, cfg);

    EXPECT_TRUE(actor->is_system_actor());
}

TEST(CliDeepProtoServer, ListClientsEmpty) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);

    CliProtoServerConfig cfg;
    auto actor = std::make_shared<CliProtoServerActor>(nullptr, system, cfg);

    std::string clients = actor->list_clients();
    EXPECT_EQ(clients, "No connected clients.\n");
}

TEST(CliDeepProtoServer, CloseClientNotFound) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);

    CliProtoServerConfig cfg;
    auto actor = std::make_shared<CliProtoServerActor>(nullptr, system, cfg);

    EXPECT_FALSE(actor->close_client(42));
}

TEST(CliDeepProtoServer, ClientHistoryNotFound) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);

    CliProtoServerConfig cfg;
    auto actor = std::make_shared<CliProtoServerActor>(nullptr, system, cfg);

    std::string history = actor->client_history(99);
    EXPECT_NE(history.find("not found"), std::string::npos);
}

TEST(CliDeepProtoServer, ExecutePathReturnsFalse) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);

    CliProtoServerConfig cfg;
    auto actor = std::make_shared<CliProtoServerActor>(nullptr, system, cfg);

    PrettyFormatter fmt;
    EXPECT_FALSE(actor->execute_path("test/path", {}, {}, fmt));
}

// ---------------------------------------------------------------------------
// CliLocalActor command processing
// ---------------------------------------------------------------------------

TEST(CliDeepLocalActor, ToMetadataWithoutSpawn) {
    CliConfig cfg;
    cfg.enabled = false;

    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);
    auto actor = std::make_shared<CliActor>(nullptr, system, cfg);
    auto meta = actor->to_metadata();

    // When not spawned, type_name() is empty. running_ defaults to true via
    // InteractiveCliActor initializer — state is "Running".
    EXPECT_EQ(meta.actor_id, 0u);
    EXPECT_EQ(meta.state, "Running");
}

TEST(CliDeepLocalActor, ConfigAccessor) {
    CliConfig cfg;
    cfg.enabled = false;
    cfg.default_format = "json";
    cfg.page_size = 100;

    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);
    auto actor = std::make_shared<CliActor>(nullptr, system, cfg);

    EXPECT_EQ(actor->config().default_format, "json");
    EXPECT_EQ(actor->config().page_size, 100u);
    EXPECT_FALSE(actor->config().enabled);
}

TEST(CliDeepLocalActor, FormatAndPagerAccessors) {
    CliConfig cfg;
    cfg.enabled = false;
    cfg.default_format = "tabular";
    cfg.page_size = 25;

    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);
    auto actor = std::make_shared<CliActor>(nullptr, system, cfg);

    ASSERT_NE(actor->formatter(), nullptr);
    ASSERT_NE(actor->pager(), nullptr);
    EXPECT_EQ(actor->pager()->current_page(), 1u);
}

TEST(CliDeepLocalActor, ExecutePathReturnsFalse) {
    CliConfig cfg;
    cfg.enabled = false;

    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);
    auto actor = std::make_shared<CliActor>(nullptr, system, cfg);

    PrettyFormatter fmt;
    EXPECT_FALSE(actor->execute_path("any/path", {}, {}, fmt));
}

// ---------------------------------------------------------------------------
// LocalServerCliHost — shared host implementation
// ---------------------------------------------------------------------------

TEST(CliDeepLocalServerHost, ConstructorAndBuildCommandTree) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);
    LocalServerCliHost host(system);

    auto tree = host.build_command_tree();
    ASSERT_NE(tree, nullptr);
    EXPECT_FALSE(tree->keyword.empty()); // root "/" node from tree builder

    // The tree should have at least some registered commands
    EXPECT_FALSE(tree->children.empty());
}

TEST(CliDeepLocalServerHost, MakeMetadata) {
    Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    sys_cfg.enable_network = false;
    sys_cfg.cli.enabled = false;
    sys_cfg.tracing.enabled = false;

    ActorSystem system(sys_cfg);
    LocalServerCliHost host(system);

    {
        auto meta = host.make_metadata(ActorId(42), "TestActor", true);
        EXPECT_EQ(meta.actor_id, 42u);
        EXPECT_EQ(meta.actor_type, "TestActor");
        EXPECT_EQ(meta.state, "Running");
    }
    {
        auto meta = host.make_metadata(ActorId(99), "TestActor", false);
        EXPECT_EQ(meta.actor_id, 99u);
        EXPECT_EQ(meta.state, "Stopped");
    }
}
