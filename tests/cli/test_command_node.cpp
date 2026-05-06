#include <hpactor/cli/command_node.hpp>
#include <cassert>
#include <cstdio>

using namespace hpactor::cli;

void test_tree_building() {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    assert(root.children.size() == 2);
    assert(root.children[0]->keyword == "actor");
    assert(root.children[1]->keyword == "system");
}

void test_exact_match() {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    std::string param;
    auto* node = root.find_child("actor", param);
    assert(node != nullptr);
    assert(node->keyword == "actor");
}

void test_missing() {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");

    std::string param;
    auto* node = root.find_child("bogus", param);
    assert(node == nullptr);
}

void test_parameter_node() {
    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    actor->add_child("<id>", "Target actor ID", true);

    std::string param;
    auto* actor_node = root.find_child("actor", param);
    assert(actor_node != nullptr);

    auto* id_node = actor_node->find_child("0x123", param);
    assert(id_node != nullptr);
    assert(param == "0x123");
    assert(id_node->is_parameter);
}

void test_parameter_node_matches_any_non_keyword() {
    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    auto* id_node = actor->add_child("<id>", "Target actor ID", true);
    id_node->add_child("show", "Display actor metadata");

    std::string param;
    auto* match = actor->find_child("5", param);
    assert(match != nullptr);
    assert(match->is_parameter);
    assert(param == "5");
}

void test_nested_commands() {
    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    auto* id_node = actor->add_child("<id>", "Target actor ID", true);
    id_node->add_child("show", "Display actor metadata");
    id_node->add_child("kill", "Terminate actor");

    std::string p1, p2;
    auto* n1 = root.find_child("actor", p1);
    assert(n1 != nullptr);
    auto* n2 = n1->find_child("5", p2);
    assert(n2 != nullptr);
    assert(p2 == "5");
    auto* n3 = n2->find_child("show", p1);
    assert(n3 != nullptr);
    assert(n3->keyword == "show");
}

void test_suggest() {
    CommandNode root{"", "root"};
    root.add_child("show", "Display actor metadata");
    root.add_child("list", "List actors");

    auto sug = root.suggest("shwo");
    assert(sug == "show");

    auto sug2 = root.suggest("lisst");
    assert(sug2 == "list");
}

void test_help_text() {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    auto text = root.help();
    assert(text.find("actor") != std::string::npos);
    assert(text.find("Actor operations") != std::string::npos);
    assert(text.find("system") != std::string::npos);
}

int main() {
    test_tree_building();
    test_exact_match();
    test_missing();
    test_parameter_node();
    test_parameter_node_matches_any_non_keyword();
    test_nested_commands();
    test_suggest();
    test_help_text();
    printf("test_command_node: PASSED\n");
    return 0;
}
