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

#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli/tabular_formatter.hpp>

#include <gtest/gtest.h>

using namespace hpactor::cli;

TEST(TabularFormatterTest, Header) {
    TabularFormatter f;
    f.header("Test Title");
    auto out = f.finalize();
    EXPECT_EQ(out, "# Test Title\n");
}

TEST(TabularFormatterTest, TableWithData) {
    TabularFormatter f;
    f.table({"ID", "Type", "State"},
            {{"0x0001", "EchoActor", "Running"}, {"0x0002", "Worker", "Idle"}});
    auto out = f.finalize();
    EXPECT_NE(out.find("ID"), std::string::npos);
    EXPECT_NE(out.find("EchoActor"), std::string::npos);
    EXPECT_NE(out.find("Idle"), std::string::npos);
}

TEST(TabularFormatterTest, TableEmptyColumns) {
    TabularFormatter f;
    f.table({}, {{"a", "b"}});
    auto out = f.finalize();
    EXPECT_TRUE(out.empty());
}

TEST(TabularFormatterTest, TableMissingValues) {
    TabularFormatter f;
    f.table({"Col1", "Col2"}, {{"only_one"}});
    auto out = f.finalize();
    EXPECT_NE(out.find('-'), std::string::npos);
}

TEST(TabularFormatterTest, TableColumnWidthPadding) {
    TabularFormatter f;
    f.table({"Short", "LongerColumn"}, {{"x", "y"}});
    auto out = f.finalize();
    EXPECT_NE(out.find("Short"), std::string::npos);
    EXPECT_NE(out.find("LongerColumn"), std::string::npos);
    // LongerColumn should have been widened to fit header
    EXPECT_NE(out.find("LongerColumn  "), std::string::npos);
}

TEST(TabularFormatterTest, KeyValue) {
    TabularFormatter f;
    f.key_value({{"State", "Running"}, {"Uptime", "12m 03s"}});
    auto out = f.finalize();
    EXPECT_NE(out.find("State: Running"), std::string::npos);
    EXPECT_NE(out.find("Uptime: 12m 03s"), std::string::npos);
}

TEST(TabularFormatterTest, KeyValueEmpty) {
    TabularFormatter f;
    f.key_value({});
    auto out = f.finalize();
    EXPECT_TRUE(out.empty());
}

TEST(TabularFormatterTest, Tree) {
    TabularFormatter f;
    TreeNode root{"root",
                  "root desc",
                  {TreeNode{"child1", "first child", {}},
                   TreeNode{"child2",
                            "second child",
                            {TreeNode{"grandchild", "gc desc", {}}}}}};
    f.tree(root);
    auto out = f.finalize();
    EXPECT_NE(out.find("root"), std::string::npos);
    EXPECT_NE(out.find("root desc"), std::string::npos);
    EXPECT_NE(out.find("child1"), std::string::npos);
    EXPECT_NE(out.find("grandchild"), std::string::npos);
    // grandchild should be indented more than child1
    auto gc_pos = out.find("grandchild");
    auto c1_pos = out.find("child1");
    EXPECT_GT(gc_pos, c1_pos);
}

TEST(TabularFormatterTest, TreeNoDescription) {
    TabularFormatter f;
    TreeNode root{"root", "", {}};
    f.tree(root);
    auto out = f.finalize();
    EXPECT_EQ(out, "root\n");
}

TEST(TabularFormatterTest, Raw) {
    TabularFormatter f;
    f.raw("plain text");
    auto out = f.finalize();
    EXPECT_EQ(out, "plain text\n");
}

TEST(TabularFormatterTest, RawEmpty) {
    TabularFormatter f;
    f.raw("");
    auto out = f.finalize();
    EXPECT_TRUE(out.empty());
}

TEST(TabularFormatterTest, RawWithTrailingNewline) {
    TabularFormatter f;
    f.raw("line\n");
    auto out = f.finalize();
    EXPECT_EQ(out, "line\n");
}

TEST(TabularFormatterTest, Error) {
    TabularFormatter f;
    f.error("actor not found");
    auto out = f.finalize();
    EXPECT_EQ(out, "ERROR: actor not found\n");
}

TEST(TabularFormatterTest, FinalizeConsumesBuffer) {
    TabularFormatter f;
    f.raw("first");
    auto out1 = f.finalize();
    EXPECT_EQ(out1, "first\n");
    auto out2 = f.finalize();
    EXPECT_TRUE(out2.empty());
}

TEST(TabularFormatterTest, MultipleOperations) {
    TabularFormatter f;
    f.header("Overview");
    f.key_value({{"total", "5"}});
    f.error("one problem");
    auto out = f.finalize();
    EXPECT_NE(out.find("# Overview"), std::string::npos);
    EXPECT_NE(out.find("total: 5"), std::string::npos);
    EXPECT_NE(out.find("ERROR: one problem"), std::string::npos);
}
