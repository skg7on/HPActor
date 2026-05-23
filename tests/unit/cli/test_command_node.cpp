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

using namespace hpactor::cli;

TEST(CommandNodeTest, TreeBuilding) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    EXPECT_EQ(root.children.size(), 2u);
    EXPECT_EQ(root.children[0]->keyword, "actor");
    EXPECT_EQ(root.children[1]->keyword, "system");
}

TEST(CommandNodeTest, ExactMatch) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    std::string param;
    auto* node = root.find_child("actor", param);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->keyword, "actor");
}

TEST(CommandNodeTest, Missing) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");

    std::string param;
    auto* node = root.find_child("bogus", param);
    EXPECT_EQ(node, nullptr);
}

TEST(CommandNodeTest, ParameterNode) {
    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    actor->add_child("<id>", "Target actor ID", true);

    std::string param;
    auto* actor_node = root.find_child("actor", param);
    ASSERT_NE(actor_node, nullptr);

    auto* id_node = actor_node->find_child("0x123", param);
    ASSERT_NE(id_node, nullptr);
    EXPECT_EQ(param, "0x123");
    EXPECT_TRUE(id_node->is_parameter);
}

TEST(CommandNodeTest, ParameterNodeMatchesAnyNonKeyword) {
    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    auto* id_node = actor->add_child("<id>", "Target actor ID", true);
    id_node->add_child("show", "Display actor metadata");

    std::string param;
    auto* match = actor->find_child("5", param);
    ASSERT_NE(match, nullptr);
    EXPECT_TRUE(match->is_parameter);
    EXPECT_EQ(param, "5");
}

TEST(CommandNodeTest, NestedCommands) {
    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    auto* id_node = actor->add_child("<id>", "Target actor ID", true);
    id_node->add_child("show", "Display actor metadata");
    id_node->add_child("kill", "Terminate actor");

    std::string p1, p2;
    auto* n1 = root.find_child("actor", p1);
    ASSERT_NE(n1, nullptr);
    auto* n2 = n1->find_child("5", p2);
    ASSERT_NE(n2, nullptr);
    EXPECT_EQ(p2, "5");
    auto* n3 = n2->find_child("show", p1);
    ASSERT_NE(n3, nullptr);
    EXPECT_EQ(n3->keyword, "show");
}

TEST(CommandNodeTest, Suggest) {
    CommandNode root{"", "root"};
    root.add_child("show", "Display actor metadata");
    root.add_child("list", "List actors");

    auto sug = root.suggest("shwo");
    EXPECT_EQ(sug, "show");

    auto sug2 = root.suggest("lisst");
    EXPECT_EQ(sug2, "list");
}

TEST(CommandNodeTest, FindChildPrefixSingleMatch) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    auto* n = root.find_child_prefix("ac");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->keyword, "actor");
}

TEST(CommandNodeTest, FindChildPrefixAmbiguous) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("action", "Action operations");

    auto* n = root.find_child_prefix("act");
    EXPECT_EQ(n, nullptr); // ambiguous
}

TEST(CommandNodeTest, FindChildPrefixNoMatch) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");

    auto* n = root.find_child_prefix("xyz");
    EXPECT_EQ(n, nullptr);
}

TEST(CommandNodeTest, CollectCompletions) {
    CommandNode root{"", "root"};
    root.add_child("show", "Display");
    root.add_child("list", "List");
    root.add_child("kill", "Kill");

    std::vector<std::string> out;
    root.collect_completions("s", out);
    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "show");

    out.clear();
    root.collect_completions("", out);
    EXPECT_EQ(out.size(), 3u);
}

TEST(CommandNodeTest, CollectCompletionsSkipsParameters) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor");
    root.add_child("<id>", "param", true);

    std::vector<std::string> out;
    root.collect_completions("", out);
    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "actor");
}

TEST(CommandNodeTest, HelpText) {
    CommandNode root{"", "root"};
    root.add_child("actor", "Actor operations");
    root.add_child("system", "System operations");

    auto text = root.help();
    EXPECT_NE(text.find("actor"), std::string::npos);
    EXPECT_NE(text.find("Actor operations"), std::string::npos);
    EXPECT_NE(text.find("system"), std::string::npos);
}
