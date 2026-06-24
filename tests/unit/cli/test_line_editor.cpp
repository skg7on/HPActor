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

#include <gtest/gtest.h>
#include <hpactor/cli/command/command_node.hpp>
#include <hpactor/cli/io/line_editor.hpp>
#include <sys/stat.h>

using namespace hpactor::cli;

static std::unique_ptr<CommandNode> build_test_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");
    auto* actor = root->add_child("actor", "Actor operations");
    auto* id = actor->add_child("<id>", "Target actor ID", true);
    id->add_child("show", "Display actor metadata");
    id->add_child("kill", "Terminate actor");
    actor->add_child("list", "List all actors");
    auto* sys = root->add_child("system", "System operations");
    sys->add_child("stats", "System statistics");
    sys->add_child("memory", "Memory stats");
    root->add_child("help", "Show help");
    root->add_child("quit", "Exit CLI");
    return root;
}

// The CommandNode tree traversal is what powers completion and hints.
// Test that the tree structure is correct and find_child works.

TEST(LineEditorTest, RootTraversal) {
    auto root = build_test_tree();
    std::string param;
    auto* n = root->find_child("actor", param);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->keyword, "actor");
}

TEST(LineEditorTest, UnknownCommand) {
    auto root = build_test_tree();
    std::string param;
    auto* n = root->find_child("bogus", param);
    EXPECT_EQ(n, nullptr);
}

TEST(LineEditorTest, ParameterNodeMatchesAny) {
    auto root = build_test_tree();
    std::string param;
    auto* actor = root->find_child("actor", param);
    ASSERT_NE(actor, nullptr);
    auto* id_node = actor->find_child("0x123", param);
    ASSERT_NE(id_node, nullptr);
    EXPECT_TRUE(id_node->is_parameter);
    EXPECT_EQ(param, "0x123");
}

TEST(LineEditorTest, NestedCompletionPath) {
    auto root = build_test_tree();
    std::string p1, p2, p3;
    auto* actor = root->find_child("actor", p1);
    ASSERT_NE(actor, nullptr);
    auto* id_node = actor->find_child("5", p2);
    ASSERT_NE(id_node, nullptr);
    auto* show_node = id_node->find_child("show", p3);
    ASSERT_NE(show_node, nullptr);
    EXPECT_EQ(show_node->keyword, "show");
}

TEST(LineEditorTest, CompletionMatches) {
    auto root = build_test_tree();
    std::string param;
    auto* actor = root->find_child("actor", param);
    ASSERT_NE(actor, nullptr);

    // Under /actor: <id> (parameter) and "list" (keyword)
    bool found_list = false, found_param = false;
    for (auto& c : actor->children) {
        if (c->keyword == "list" && !c->is_parameter)
            found_list = true;
        if (c->keyword == "<id>" && c->is_parameter)
            found_param = true;
    }
    EXPECT_TRUE(found_list);
    EXPECT_TRUE(found_param);

    // Under /actor <id>: "show" and "kill"
    auto* id_node = actor->find_child("5", param);
    bool found_show = false, found_kill = false;
    for (auto& c : id_node->children) {
        if (c->keyword == "show")
            found_show = true;
        if (c->keyword == "kill")
            found_kill = true;
    }
    EXPECT_TRUE(found_show && found_kill);
}

TEST(LineEditorTest, PrefixMatchForCompletion) {
    // Simulate what on_completion does: find children matching prefix
    auto root = build_test_tree();
    std::string param;
    auto* actor = root->find_child("actor", param);

    // "list" starts with "l" -> should match
    bool has_list = false;
    for (auto& c : actor->children) {
        if (c->keyword.starts_with("l") && !c->is_parameter)
            has_list = true;
    }
    EXPECT_TRUE(has_list);
}

TEST(LineEditorTest, RootChildren) {
    auto root = build_test_tree();
    // Root should have actor, system, help, quit
    bool found_actor = false, found_system = false, found_help = false,
         found_quit = false;
    for (auto& c : root->children) {
        if (c->keyword == "actor")
            found_actor = true;
        if (c->keyword == "system")
            found_system = true;
        if (c->keyword == "help")
            found_help = true;
        if (c->keyword == "quit")
            found_quit = true;
    }
    EXPECT_TRUE(found_actor && found_system && found_help && found_quit);
}

TEST(LineEditorTest, PrefixMatchIntermediateToken) {
    // Simulates "/act" — "act" is an intermediate token that should
    // prefix-match "actor" at the root level.
    auto root = build_test_tree();
    auto* n = root->find_child_prefix("act");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->keyword, "actor");
}

TEST(LineEditorTest, PrefixMatchLastToken) {
    // Simulates "actor l" — "l" should prefix-match "list" under /actor
    auto root = build_test_tree();
    std::string param;
    auto* actor = root->find_child("actor", param);
    ASSERT_NE(actor, nullptr);

    std::vector<std::string> completions;
    actor->collect_completions("l", completions);
    EXPECT_EQ(completions.size(), 1u);
    EXPECT_EQ(completions[0], "list");
}

TEST(LineEditorTest, PrefixMatchAmbiguousShowsAll) {
    CommandNode root{"", "root"};
    root.add_child("show", "Display");
    root.add_child("stats", "Statistics");
    root.add_child("stop", "Stop");

    // "s" prefix — three matches
    std::vector<std::string> out;
    root.collect_completions("s", out);
    EXPECT_EQ(out.size(), 3u);
}

TEST(LineEditorTest, SuggestLevenshtein) {
    CommandNode root{"", "root"};
    root.add_child("show", "Display");
    root.add_child("list", "List");

    EXPECT_EQ(root.suggest("shwo"), "show");
    EXPECT_EQ(root.suggest("lisst"), "list");
    EXPECT_TRUE(root.suggest("xyz").empty());
}

TEST(LineEditorTest, HelpText) {
    auto root = build_test_tree();
    auto text = root->help();
    EXPECT_FALSE(text.empty());
    EXPECT_NE(text.find("actor"), std::string::npos);
    EXPECT_NE(text.find("system"), std::string::npos);
}

TEST(LineEditorTest, ConstructDestruct) {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    auto root = std::make_unique<CommandNode>("/", "root");
    {
        LineEditor editor(cfg, root.get());
    }
    // No crash = pass
}

TEST(LineEditorTest, ConstructWithHistoryPath) {
    LineEditorConfig cfg;
    cfg.history_max = 200;
    cfg.history_path = "/tmp/hpactor_test_line_editor_history.txt";
    cfg.multiline = true;
    std::remove(cfg.history_path.c_str());
    auto root = std::make_unique<CommandNode>("/", "root");
    LineEditor editor(cfg, root.get());
    std::remove(cfg.history_path.c_str());
}

TEST(LineEditorTest, AddHistoryNoPath) {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    auto root = std::make_unique<CommandNode>("/", "root");
    LineEditor editor(cfg, root.get());
    editor.add_history("/help");
}

TEST(LineEditorTest, AddHistoryWithPath) {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "/tmp/hpactor_test_line_editor_history2.txt";
    cfg.multiline = false;
    std::remove(cfg.history_path.c_str());
    auto root = std::make_unique<CommandNode>("/", "root");
    LineEditor editor(cfg, root.get());
    editor.add_history("/system stats");
    struct stat st;
    EXPECT_EQ(stat(cfg.history_path.c_str(), &st), 0);
    std::remove(cfg.history_path.c_str());
}

TEST(LineEditorTest, LoadSaveHistoryNoPath) {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    auto root = std::make_unique<CommandNode>("/", "root");
    LineEditor editor(cfg, root.get());
    editor.load_history();
    editor.save_history();
}

TEST(LineEditorTest, SetRoot) {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    auto root1 = std::make_unique<CommandNode>("/", "root");
    LineEditor editor(cfg, root1.get());
    auto root2 = std::make_unique<CommandNode>("/", "root2");
    editor.set_root(root2.get());
}

// =============================================================================
// compute_completions
// =============================================================================

TEST(LineEditorTest, ComputeCompletionsEmptyBuffer) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_completions("");
    EXPECT_TRUE(result.matches.empty());
    EXPECT_TRUE(result.prefix.empty());
}

TEST(LineEditorTest, ComputeCompletionsSlash) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_completions("/");
    // "/" is skipped but becomes partial=words.back()="/";
    // collect_completions("/") matches nothing → 0 results
    EXPECT_EQ(result.matches.size(), 0u);
}

TEST(LineEditorTest, ComputeCompletionsPrefixMatch) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_completions("/ac");
    // "/" skipped, i advances to 1, but words_to_consume=1 → loop skipped.
    // partial="ac", no exact match → collect_completions("ac") at root
    // → "actor" starts with "ac" → 1 match
    EXPECT_EQ(result.matches.size(), 1u);
    EXPECT_EQ(result.matches[0], "actor");
    EXPECT_EQ(result.prefix, "/");
}

TEST(LineEditorTest, ComputeCompletionsExactMatch) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_completions("/actor");
    // exact match "actor" → consumed, partial="" → all non-param children
    bool found_list = false;
    for (auto& m : result.matches) {
        if (m == "list")
            found_list = true;
        // <id> is parameter, filtered by collect_completions
    }
    EXPECT_TRUE(found_list);
    EXPECT_EQ(result.matches.size(), 1u); // only "list", not "<id>"
    EXPECT_EQ(result.prefix, "/actor ");
}

TEST(LineEditorTest, ComputeCompletionsTrailingSpace) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_completions("/actor ");
    // trailing space → "actor" fully consumed, partial="" → all non-param
    bool found_list = false;
    for (auto& m : result.matches) {
        if (m == "list")
            found_list = true;
    }
    EXPECT_TRUE(found_list);
    EXPECT_EQ(result.matches.size(), 1u); // only "list"
}

TEST(LineEditorTest, ComputeCompletionsParameterConsumed) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_completions("/actor 0x123 ");
    // trailing space → both tokens consumed, partial="" → all children
    bool found_show = false, found_kill = false;
    for (auto& m : result.matches) {
        if (m == "show")
            found_show = true;
        if (m == "kill")
            found_kill = true;
    }
    EXPECT_TRUE(found_show);
    EXPECT_TRUE(found_kill);
}

TEST(LineEditorTest, ComputeCompletionsParameterWithoutSpace) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_completions("/actor 0x123");
    // no trailing space → partial="0x123" → filters collect_completions
    // Neither "show" nor "kill" starts with "0x123" → 0 matches
    EXPECT_EQ(result.matches.size(), 0u);
}

TEST(LineEditorTest, ComputeCompletionsLeafNode) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_completions("/help ");
    // trailing space → "help" consumed, leaf node has no children
    EXPECT_EQ(result.matches.size(), 0u);
}

TEST(LineEditorTest, ComputeCompletionsNoMatch) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_completions("/xyz");
    // "xyz" has no match, no prefix match at root → collect_completions("xyz")
    // at root → no child starts with "xyz" → 0 matches
    EXPECT_EQ(result.matches.size(), 0u);
}

TEST(LineEditorTest, ComputeCompletionsNullRoot) {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, nullptr);

    auto result = editor.compute_completions("/actor");
    EXPECT_TRUE(result.matches.empty());
    EXPECT_TRUE(result.prefix.empty());
}

// =============================================================================
// compute_hint
// =============================================================================

TEST(LineEditorTest, ComputeHintEmptyBuffer) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_hint("");
    EXPECT_FALSE(result.active);
}

TEST(LineEditorTest, ComputeHintSlash) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_hint("/");
    // "/" is skipped, partial="/" → no child matches "/" → no hint
    EXPECT_FALSE(result.active);
}

TEST(LineEditorTest, ComputeHintPrefixMatch) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_hint("/a");
    // "a" prefix-matches "actor" → hint "ctor"
    EXPECT_TRUE(result.active);
    EXPECT_EQ(result.text, "ctor");
}

TEST(LineEditorTest, ComputeHintExactKeywordAdvances) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_hint("/actor");
    // exact match "actor" → advance, hint first non-param child
    EXPECT_TRUE(result.active);
    // "memory" comes before "stats" under system, but under actor it's "list"
    // Children of /actor: <id> (param, skip), "list" (keyword)
    EXPECT_EQ(result.text, "list");
}

TEST(LineEditorTest, ComputeHintTrailingSpace) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_hint("/actor ");
    EXPECT_TRUE(result.active);
    EXPECT_EQ(result.text, "list");
}

TEST(LineEditorTest, ComputeHintLeafNode) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_hint("/actor list");
    // exact match "list" → advance, leaf has no children → no hint
    EXPECT_FALSE(result.active);
}

TEST(LineEditorTest, ComputeHintNoMatch) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_hint("/unknown");
    EXPECT_FALSE(result.active);
}

TEST(LineEditorTest, ComputeHintNullRoot) {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, nullptr);

    auto result = editor.compute_hint("/actor");
    EXPECT_FALSE(result.active);
}

TEST(LineEditorTest, ComputeHintSystem) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_hint("/system");
    // exact match "system" → advance, children: "stats", "memory"
    // first non-param child in insertion order is "stats"
    EXPECT_TRUE(result.active);
    EXPECT_EQ(result.text, "stats");
}

TEST(LineEditorTest, ComputeHintPartialSubcommand) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_hint("/system st");
    // "st" prefix-matches "stats" → hint "ats"
    EXPECT_TRUE(result.active);
    EXPECT_EQ(result.text, "ats");
}

// =============================================================================
// Additional completion tests
// =============================================================================

TEST(LineEditorTest, ComputeCompletionsTwoLevelPrefix) {
    auto root = build_test_tree();
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, root.get());

    auto result = editor.compute_completions("/actor 0x123 sh ");
    // trailing space → "sh" consumed via prefix match → "show"
    // partial="" → all children of show node (none, show is leaf) → 0 matches
    EXPECT_EQ(result.matches.size(), 0u);
}

TEST(LineEditorTest, ComputeCompletionsAmbiguousPrefix) {
    CommandNode root{"", "root"};
    root.add_child("show", "Display");
    root.add_child("stats", "Statistics");
    root.add_child("stop", "Stop");

    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    LineEditor editor(cfg, &root);

    auto result = editor.compute_completions("/s ");
    EXPECT_EQ(result.matches.size(), 3u);
}
