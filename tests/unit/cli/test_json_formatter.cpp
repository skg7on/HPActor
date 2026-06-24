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

#include <gtest/gtest.h>

using namespace hpactor::cli;

TEST(JsonFormatterTest, HeaderIsNoop) {
    JsonFormatter f;
    f.header("Test Title");
    auto out = f.finalize();
    EXPECT_TRUE(out.empty());
}

TEST(JsonFormatterTest, Table) {
    JsonFormatter f;
    f.table({"id", "type"}, {{"1", "EchoActor"}, {"2", "Worker"}});
    auto out = f.finalize();
    EXPECT_EQ(out[0], '[');
    EXPECT_NE(out.find("\"id\""), std::string::npos);
    EXPECT_NE(out.find("\"EchoActor\""), std::string::npos);
    EXPECT_NE(out.find("\"Worker\""), std::string::npos);
    EXPECT_EQ(out.back(), ']');
}

TEST(JsonFormatterTest, TableEmptyRows) {
    JsonFormatter f;
    f.table({"col1", "col2"}, {});
    auto out = f.finalize();
    EXPECT_EQ(out, "[]");
}

TEST(JsonFormatterTest, TableSingleRow) {
    JsonFormatter f;
    f.table({"key"}, {{"value"}});
    auto out = f.finalize();
    EXPECT_EQ(out, "[{\"key\":\"value\"}]");
}

TEST(JsonFormatterTest, KeyValue) {
    JsonFormatter f;
    f.key_value({{"actor_id", "5"}, {"state", "Running"}});
    auto out = f.finalize();
    EXPECT_EQ(out[0], '{');
    EXPECT_NE(out.find("\"actor_id\""), std::string::npos);
    EXPECT_NE(out.find("\"Running\""), std::string::npos);
    EXPECT_EQ(out.back(), '}');
}

TEST(JsonFormatterTest, KeyValueEmpty) {
    JsonFormatter f;
    f.key_value({});
    auto out = f.finalize();
    EXPECT_EQ(out, "{}");
}

TEST(JsonFormatterTest, KeyValueSingle) {
    JsonFormatter f;
    f.key_value({{"k", "v"}});
    auto out = f.finalize();
    EXPECT_EQ(out, "{\"k\":\"v\"}");
}

TEST(JsonFormatterTest, Tree) {
    JsonFormatter f;
    TreeNode root{"root", "desc", {TreeNode{"child", "child desc", {}}}};
    f.tree(root);
    auto out = f.finalize();
    EXPECT_NE(out.find("\"name\""), std::string::npos);
    EXPECT_NE(out.find("\"root\""), std::string::npos);
    EXPECT_NE(out.find("\"children\""), std::string::npos);
    EXPECT_NE(out.find("\"child\""), std::string::npos);
    EXPECT_EQ(out[0], '{');
}

TEST(JsonFormatterTest, TreeNested) {
    JsonFormatter f;
    TreeNode root{"a", "", {TreeNode{"b", "", {TreeNode{"c", "", {}}}}}};
    f.tree(root);
    auto out = f.finalize();
    // Should contain nested children structure
    EXPECT_NE(out.find("\"a\""), std::string::npos);
    EXPECT_NE(out.find("\"b\""), std::string::npos);
    EXPECT_NE(out.find("\"c\""), std::string::npos);
}

TEST(JsonFormatterTest, Raw) {
    JsonFormatter f;
    f.raw("hello");
    auto out = f.finalize();
    EXPECT_EQ(out, "\"hello\"");
}

TEST(JsonFormatterTest, RawWithSpecialChars) {
    JsonFormatter f;
    f.raw("say \"hi\"");
    auto out = f.finalize();
    EXPECT_NE(out.find("\\\""), std::string::npos);
}

TEST(JsonFormatterTest, RawEmpty) {
    JsonFormatter f;
    f.raw("");
    auto out = f.finalize();
    EXPECT_EQ(out, "\"\"");
}

TEST(JsonFormatterTest, Error) {
    JsonFormatter f;
    f.error("something went wrong");
    auto out = f.finalize();
    EXPECT_NE(out.find("\"error\""), std::string::npos);
    EXPECT_NE(out.find("something went wrong"), std::string::npos);
}

TEST(JsonFormatterTest, ErrorWithQuotesInMessage) {
    JsonFormatter f;
    f.error("missing \"key\"");
    auto out = f.finalize();
    EXPECT_NE(out.find("\\\"key\\\""), std::string::npos);
}

TEST(JsonFormatterTest, FinalizeConsumesBuffer) {
    JsonFormatter f;
    f.raw("first");
    auto out1 = f.finalize();
    EXPECT_EQ(out1, "\"first\"");
    auto out2 = f.finalize();
    EXPECT_TRUE(out2.empty());
}

TEST(JsonFormatterTest, MultipleOperations) {
    JsonFormatter f;
    f.key_value({{"a", "1"}});
    f.error("bad");
    auto out = f.finalize();
    // Key_value output goes first, error goes second — they accumulate
    EXPECT_NE(out.find("{\"a\":\"1\"}"), std::string::npos);
    EXPECT_NE(out.find("{\"error\":\"bad\"}"), std::string::npos);
}
