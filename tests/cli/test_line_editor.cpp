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

#include <cassert>
#include <cstdio>
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

void test_root_traversal() {
    auto root = build_test_tree();
    std::string param;
    auto* n = root->find_child("actor", param);
    assert(n != nullptr);
    assert(n->keyword == "actor");
}

void test_unknown_command() {
    auto root = build_test_tree();
    std::string param;
    auto* n = root->find_child("bogus", param);
    assert(n == nullptr);
}

void test_parameter_node_matches_any() {
    auto root = build_test_tree();
    std::string param;
    auto* actor = root->find_child("actor", param);
    assert(actor != nullptr);
    auto* id_node = actor->find_child("0x123", param);
    assert(id_node != nullptr);
    assert(id_node->is_parameter);
    assert(param == "0x123");
}

void test_nested_completion_path() {
    auto root = build_test_tree();
    std::string p1, p2, p3;
    auto* actor = root->find_child("actor", p1);
    assert(actor != nullptr);
    auto* id_node = actor->find_child("5", p2);
    assert(id_node != nullptr);
    auto* show_node = id_node->find_child("show", p3);
    assert(show_node != nullptr);
    assert(show_node->keyword == "show");
}

void test_completion_matches() {
    auto root = build_test_tree();
    std::string param;
    auto* actor = root->find_child("actor", param);
    assert(actor != nullptr);

    // Under /actor: <id> (parameter) and "list" (keyword)
    bool found_list = false, found_param = false;
    for (auto& c : actor->children) {
        if (c->keyword == "list" && !c->is_parameter)
            found_list = true;
        if (c->keyword == "<id>" && c->is_parameter)
            found_param = true;
    }
    assert(found_list);
    assert(found_param);

    // Under /actor <id>: "show" and "kill"
    auto* id_node = actor->find_child("5", param);
    bool found_show = false, found_kill = false;
    for (auto& c : id_node->children) {
        if (c->keyword == "show")
            found_show = true;
        if (c->keyword == "kill")
            found_kill = true;
    }
    assert(found_show && found_kill);
}

void test_prefix_match_for_completion() {
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
    assert(has_list);
}

void test_root_children() {
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
    assert(found_actor && found_system && found_help && found_quit);
}

void test_prefix_match_intermediate_token() {
    // Simulates "/act" — "act" is an intermediate token that should
    // prefix-match "actor" at the root level.
    auto root = build_test_tree();
    auto* n = root->find_child_prefix("act");
    assert(n != nullptr);
    assert(n->keyword == "actor");
}

void test_prefix_match_last_token() {
    // Simulates "actor l" — "l" should prefix-match "list" under /actor
    auto root = build_test_tree();
    std::string param;
    auto* actor = root->find_child("actor", param);
    assert(actor != nullptr);

    std::vector<std::string> completions;
    actor->collect_completions("l", completions);
    assert(completions.size() == 1);
    assert(completions[0] == "list");
}

void test_prefix_match_ambiguous_shows_all() {
    CommandNode root{"", "root"};
    root.add_child("show", "Display");
    root.add_child("stats", "Statistics");
    root.add_child("stop", "Stop");

    // "s" prefix — three matches
    std::vector<std::string> out;
    root.collect_completions("s", out);
    assert(out.size() == 3);
}

void test_suggest_levenshtein() {
    CommandNode root{"", "root"};
    root.add_child("show", "Display");
    root.add_child("list", "List");

    assert(root.suggest("shwo") == "show");
    assert(root.suggest("lisst") == "list");
    assert(root.suggest("xyz").empty());
}

void test_help_text() {
    auto root = build_test_tree();
    auto text = root->help();
    assert(!text.empty());
    assert(text.find("actor") != std::string::npos);
    assert(text.find("system") != std::string::npos);
}

void test_construct_destruct() {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    auto root = std::make_unique<CommandNode>("/", "root");
    {
        LineEditor editor(cfg, root.get());
    }
    printf("  PASSED test_construct_destruct\n");
}

void test_construct_with_history_path() {
    LineEditorConfig cfg;
    cfg.history_max = 200;
    cfg.history_path = "/tmp/hpactor_test_line_editor_history.txt";
    cfg.multiline = true;
    std::remove(cfg.history_path.c_str());
    auto root = std::make_unique<CommandNode>("/", "root");
    LineEditor editor(cfg, root.get());
    std::remove(cfg.history_path.c_str());
    printf("  PASSED test_construct_with_history_path\n");
}

void test_add_history_no_path() {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    auto root = std::make_unique<CommandNode>("/", "root");
    LineEditor editor(cfg, root.get());
    editor.add_history("/help");
    printf("  PASSED test_add_history_no_path\n");
}

void test_add_history_with_path() {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "/tmp/hpactor_test_line_editor_history2.txt";
    cfg.multiline = false;
    std::remove(cfg.history_path.c_str());
    auto root = std::make_unique<CommandNode>("/", "root");
    LineEditor editor(cfg, root.get());
    editor.add_history("/system stats");
    struct stat st;
    assert(stat(cfg.history_path.c_str(), &st) == 0);
    std::remove(cfg.history_path.c_str());
    printf("  PASSED test_add_history_with_path\n");
}

void test_load_save_history_no_path() {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    auto root = std::make_unique<CommandNode>("/", "root");
    LineEditor editor(cfg, root.get());
    editor.load_history();
    editor.save_history();
    printf("  PASSED test_load_save_history_no_path\n");
}

void test_set_root() {
    LineEditorConfig cfg;
    cfg.history_max = 100;
    cfg.history_path = "";
    cfg.multiline = false;
    auto root1 = std::make_unique<CommandNode>("/", "root");
    LineEditor editor(cfg, root1.get());
    auto root2 = std::make_unique<CommandNode>("/", "root2");
    editor.set_root(root2.get());
    printf("  PASSED test_set_root\n");
}

int main() {
    test_root_traversal();
    test_unknown_command();
    test_parameter_node_matches_any();
    test_nested_completion_path();
    test_completion_matches();
    test_prefix_match_for_completion();
    test_root_children();
    test_prefix_match_intermediate_token();
    test_prefix_match_last_token();
    test_prefix_match_ambiguous_shows_all();
    test_suggest_levenshtein();
    test_help_text();
    test_construct_destruct();
    test_construct_with_history_path();
    test_add_history_no_path();
    test_add_history_with_path();
    test_load_save_history_no_path();
    test_set_root();

    printf("test_line_editor: PASSED\n");
    return 0;
}
