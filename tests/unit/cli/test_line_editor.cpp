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
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/line_editor.hpp>
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
