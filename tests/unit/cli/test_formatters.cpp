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

#include <hpactor/cli/format/json_formatter.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/cli/format/pretty_formatter.hpp>
#include <hpactor/cli/format/tabular_formatter.hpp>

#include <gtest/gtest.h>

using namespace hpactor::cli;

TEST(FormattersTest, PrettyKeyValue) {
    PrettyFormatter f;
    f.key_value({{"State", "Running"}, {"Uptime", "12m 03s"}});
    auto out = f.finalize();
    EXPECT_NE(out.find("State"), std::string::npos);
    EXPECT_NE(out.find("Running"), std::string::npos);
}

TEST(FormattersTest, PrettyTable) {
    PrettyFormatter f;
    f.table({"ID", "Type", "State"},
            {{"0x0001", "EchoActor", "Running"}, {"0x0002", "Worker", "Idle"}});
    auto out = f.finalize();
    EXPECT_NE(out.find("ID"), std::string::npos);
    EXPECT_NE(out.find("EchoActor"), std::string::npos);
    EXPECT_NE(out.find("Idle"), std::string::npos);
}

TEST(FormattersTest, PrettyHeader) {
    PrettyFormatter f;
    f.header("Actor 0x0005");
    auto out = f.finalize();
    EXPECT_NE(out.find("Actor 0x0005"), std::string::npos);
}

TEST(FormattersTest, PrettyError) {
    PrettyFormatter f;
    f.error("actor not found");
    auto out = f.finalize();
    EXPECT_NE(out.find("Error"), std::string::npos);
    EXPECT_NE(out.find("actor not found"), std::string::npos);
}

TEST(FormattersTest, JsonKeyValue) {
    JsonFormatter f;
    f.key_value({{"actor_id", "5"}, {"state", "Running"}});
    auto out = f.finalize();
    EXPECT_NE(out.find("\"actor_id\""), std::string::npos);
    EXPECT_NE(out.find("\"Running\""), std::string::npos);
    EXPECT_EQ(out[0], '{');
}

TEST(FormattersTest, JsonTable) {
    JsonFormatter f;
    f.table({"id", "type"}, {{"1", "EchoActor"}});
    auto out = f.finalize();
    EXPECT_EQ(out[0], '[');
    EXPECT_NE(out.find("\"id\""), std::string::npos);
    EXPECT_NE(out.find("\"EchoActor\""), std::string::npos);
}

TEST(FormattersTest, JsonError) {
    JsonFormatter f;
    f.error("something went wrong");
    auto out = f.finalize();
    EXPECT_NE(out.find("\"error\""), std::string::npos);
}

TEST(FormattersTest, TabularNoAnsi) {
    TabularFormatter f;
    f.key_value({{"key", "value"}});
    auto out = f.finalize();
    EXPECT_EQ(out.find('\033'), std::string::npos);
    EXPECT_NE(out.find("key: value"), std::string::npos);
}

TEST(FormattersTest, TabularTable) {
    TabularFormatter f;
    f.table({"ID", "Type"}, {{"1", "Echo"}, {"2", "Worker"}});
    auto out = f.finalize();
    EXPECT_NE(out.find("ID"), std::string::npos);
    EXPECT_NE(out.find("Echo"), std::string::npos);
    EXPECT_EQ(out.find('\033'), std::string::npos);
}

TEST(FormattersTest, Factory) {
    EXPECT_NE(OutputFormatter::create("pretty"), nullptr);
    EXPECT_NE(OutputFormatter::create("json"), nullptr);
    EXPECT_NE(OutputFormatter::create("tabular"), nullptr);
    EXPECT_NE(OutputFormatter::create("bogus"), nullptr); // fallback to pretty
}

TEST(FormattersTest, PrettyTree) {
    PrettyFormatter f;
    TreeNode child{"child", "child help", {}};
    TreeNode root{"root", "root description", {std::move(child)}};
    f.tree(root);
    auto out = f.finalize();
    EXPECT_NE(out.find("root"), std::string::npos);
    EXPECT_NE(out.find("child"), std::string::npos);
}

TEST(FormattersTest, PrettyTreeNoChildren) {
    PrettyFormatter f;
    TreeNode leaf{"leaf", "", {}};
    f.tree(leaf);
    auto out = f.finalize();
    EXPECT_NE(out.find("leaf"), std::string::npos);
}

TEST(FormattersTest, PrettyTreeNested) {
    PrettyFormatter f;
    TreeNode grandchild{"gc", "", {}};
    TreeNode child{"b", "", {std::move(grandchild)}};
    TreeNode root{"a", "", {std::move(child)}};
    f.tree(root);
    auto out = f.finalize();
    EXPECT_NE(out.find('a'), std::string::npos);
    EXPECT_NE(out.find('b'), std::string::npos);
    EXPECT_NE(out.find("gc"), std::string::npos);
}

TEST(FormattersTest, PrettyRawEmpty) {
    PrettyFormatter f;
    f.raw("");
    auto out = f.finalize();
    EXPECT_TRUE(out.empty());
}

TEST(FormattersTest, PrettyRawAddsNewline) {
    PrettyFormatter f;
    f.raw("hello");
    auto out = f.finalize();
    EXPECT_EQ(out, "hello\n");
}

TEST(FormattersTest, PrettyBufferEmpty) {
    PrettyFormatter f;
    auto out = f.finalize();
    EXPECT_TRUE(out.empty());
}

TEST(FormattersTest, TabularHeader) {
    TabularFormatter f;
    f.header("My Header");
    auto out = f.finalize();
    EXPECT_NE(out.find("# My Header"), std::string::npos);
}

TEST(FormattersTest, TabularError) {
    TabularFormatter f;
    f.error("something failed");
    auto out = f.finalize();
    EXPECT_NE(out.find("ERROR: something failed"), std::string::npos);
}

TEST(FormattersTest, TabularTree) {
    TabularFormatter f;
    TreeNode child{"child", "child desc", {}};
    TreeNode root{"root", "", {std::move(child)}};
    f.tree(root);
    auto out = f.finalize();
    EXPECT_NE(out.find("root"), std::string::npos);
    EXPECT_NE(out.find("child"), std::string::npos);
    EXPECT_NE(out.find("child desc"), std::string::npos);
}
