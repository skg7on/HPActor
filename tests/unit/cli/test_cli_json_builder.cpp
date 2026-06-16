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

#include <hpactor/adt/json_helpers.hpp>

#include <gtest/gtest.h>

using hpactor::adt::JsonBuilder;

TEST(JsonBuilderTest, RootObjectEmpty) {
    auto result = JsonBuilder::root_object().build();
    EXPECT_EQ(result, "{}");
}

TEST(JsonBuilderTest, SingleStringField) {
    auto result =
        JsonBuilder::root_object().field("key", std::string("value")).build();
    EXPECT_EQ(result, R"({"key":"value"})");
}

TEST(JsonBuilderTest, SingleUint64Field) {
    auto result =
        JsonBuilder::root_object().field("count", static_cast<uint64_t>(42)).build();
    EXPECT_EQ(result, R"({"count":42})");
}

TEST(JsonBuilderTest, BooleanAndNull) {
    auto result = JsonBuilder::root_object()
                      .field("enabled", true)
                      .field("disabled", false)
                      .null_field("empty")
                      .build();
    EXPECT_EQ(result, R"({"enabled":true,"disabled":false,"empty":null})");
}

TEST(JsonBuilderTest, NestedObject) {
    auto result = JsonBuilder::root_object()
                      .object("outer")
                      .object("inner")
                      .field("value", std::string("deep"))
                      .end_object()
                      .end_object()
                      .build();
    EXPECT_EQ(result, R"({"outer":{"inner":{"value":"deep"}}})");
}

TEST(JsonBuilderTest, SimpleArray) {
    auto result = JsonBuilder::root_object()
                      .array("items")
                      .element(static_cast<uint64_t>(1))
                      .element(static_cast<uint64_t>(2))
                      .element(static_cast<uint64_t>(3))
                      .end_array()
                      .build();
    EXPECT_EQ(result, R"({"items":[1,2,3]})");
}

TEST(JsonBuilderTest, ArrayOfObjects) {
    auto result = JsonBuilder::root_object()
                      .array("children")
                      .object()
                      .field("id", static_cast<uint64_t>(1))
                      .end_object()
                      .object()
                      .field("id", static_cast<uint64_t>(2))
                      .end_object()
                      .end_array()
                      .build();
    EXPECT_EQ(result, R"({"children":[{"id":1},{"id":2}]})");
}

TEST(JsonBuilderTest, StringEscaping) {
    auto result = JsonBuilder::root_object()
                      .field("text", std::string("say \"hello\"\nnew\\line"))
                      .build();
    // json_escape handles: " -> \", \ -> \\, \n -> \n
    EXPECT_EQ(result, R"({"text":"say \"hello\"\nnew\\line"})");
}

TEST(JsonBuilderTest, ResetAndReuse) {
    JsonBuilder builder = JsonBuilder::root_object();
    builder.field("first", static_cast<uint64_t>(1));
    auto first = builder.build();
    EXPECT_EQ(first, R"({"first":1})");

    builder.reset();
    builder = JsonBuilder::root_object();
    builder.field("second", static_cast<uint64_t>(2));
    auto second = builder.build();
    EXPECT_EQ(second, R"({"second":2})");
}

TEST(JsonBuilderTest, EmptyArray) {
    auto result = JsonBuilder::root_object().array("empty").end_array().build();
    EXPECT_EQ(result, R"({"empty":[]})");
}

TEST(JsonBuilderTest, NoTrailingComma) {
    auto result = JsonBuilder::root_object()
                      .field("a", static_cast<uint64_t>(1))
                      .field("b", static_cast<uint64_t>(2))
                      .build();
    // Verify no trailing comma: last non-whitespace char before } should not be
    // ,
    EXPECT_FALSE(result.empty());
    EXPECT_EQ(result.back(), '}');
    // The character just before the closing } should be a digit, not a comma
    EXPECT_NE(result[result.size() - 2], ',');
}

TEST(JsonBuilderTest, DoublePrecision) {
    auto result = JsonBuilder::root_object().field("rate", 0.95).build();
    EXPECT_EQ(result, R"({"rate":0.95})");
}

TEST(JsonBuilderTest, ErrorEnvelope) {
    auto result = JsonBuilder::root_object()
                      .object("error")
                      .field("code", std::string("ACTOR_NOT_FOUND"))
                      .field("message", std::string("Actor 42 does not exist"))
                      .end_object()
                      .build();
    EXPECT_EQ(result,
              R"({"error":{"code":"ACTOR_NOT_FOUND","message":"Actor 42 does not exist"}})");
}

TEST(JsonBuilderTest, DeepNesting) {
    JsonBuilder builder = JsonBuilder::root_object();
    // Build 10 levels of nested objects:
    // {"l0":{"l1":{"l2":...{"l9":{"deep":"value"}}}}}
    for (int i = 0; i < 10; ++i) {
        std::string key = "l" + std::to_string(i);
        builder.object(key.c_str());
    }
    builder.field("deep", std::string("value"));
    for (int i = 0; i < 10; ++i) {
        builder.end_object();
    }
    auto result = builder.build();
    // Verify it starts with {"l0":{"l1":
    EXPECT_TRUE(result.find(R"({"l0":{"l1")") == 0);
    // Verify it contains the deep value
    EXPECT_NE(result.find(R"("deep":"value")"), std::string::npos);
    // Verify it ends with 10 closing braces
    EXPECT_EQ(result.substr(result.size() - 10), std::string(10, '}'));
}

TEST(JsonBuilderTest, Int32AndInt64) {
    auto result = JsonBuilder::root_object()
                      .field("neg", static_cast<int32_t>(-42))
                      .field("big", static_cast<int64_t>(9223372036854775807LL))
                      .build();
    EXPECT_EQ(result, R"({"neg":-42,"big":9223372036854775807})");
}
