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
#include <hpactor/cli/lexer.hpp>
#include <string>

using namespace hpactor::cli;

TEST(LexerTest, SimpleKeywords) {
    auto tokens = Lexer::tokenize("actor list");
    EXPECT_EQ(tokens.size(), 3u); // keyword, keyword, eof
    EXPECT_EQ(tokens[0].type, TokenType::Keyword);
    EXPECT_EQ(tokens[0].value, "actor");
    EXPECT_EQ(tokens[1].type, TokenType::Keyword);
    EXPECT_EQ(tokens[1].value, "list");
    EXPECT_EQ(tokens[2].type, TokenType::Eof);
}

TEST(LexerTest, LeadingSlash) {
    auto tokens = Lexer::tokenize("/actor 5 show");
    EXPECT_EQ(tokens.size(), 5u); // "/", "actor", "5", "show", Eof
    EXPECT_EQ(tokens[0].type, TokenType::Keyword);
    EXPECT_EQ(tokens[0].value, "/");
    EXPECT_EQ(tokens[1].type, TokenType::Keyword);
    EXPECT_EQ(tokens[1].value, "actor");
    EXPECT_EQ(tokens[2].type, TokenType::Keyword);
    EXPECT_EQ(tokens[2].value, "5");
    EXPECT_EQ(tokens[3].type, TokenType::Keyword);
    EXPECT_EQ(tokens[3].value, "show");
    EXPECT_EQ(tokens[4].type, TokenType::Eof);
}

TEST(LexerTest, Flags) {
    auto tokens = Lexer::tokenize("/actor list --detail --no-pager");
    EXPECT_GE(tokens.size(), 4u);
    bool found_detail = false, found_nopager = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::Flag && t.value == "detail")
            found_detail = true;
        if (t.type == TokenType::Flag && t.value == "no-pager")
            found_nopager = true;
    }
    EXPECT_TRUE(found_detail);
    EXPECT_TRUE(found_nopager);
}

TEST(LexerTest, FlagWithArg) {
    auto tokens = Lexer::tokenize("/actor list --format json --filter Worker");
    bool found_format = false, found_filter = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::FlagWithArg && t.value == "format") {
            found_format = true;
            ASSERT_TRUE(t.arg.has_value() && *t.arg == "json");
        }
        if (t.type == TokenType::FlagWithArg && t.value == "filter") {
            found_filter = true;
            ASSERT_TRUE(t.arg.has_value() && *t.arg == "Worker");
        }
    }
    EXPECT_TRUE(found_format);
    EXPECT_TRUE(found_filter);
}

TEST(LexerTest, HexActorId) {
    auto tokens = Lexer::tokenize("/actor 0x123 show");
    bool found_id = false;
    for (auto& t : tokens) {
        if (t.value == "0x123")
            found_id = true;
    }
    EXPECT_TRUE(found_id);
}

TEST(LexerTest, QuotedString) {
    auto tokens = Lexer::tokenize("/actor \"My Worker 3\" show");
    bool found = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::Parameter && t.value == "My Worker 3")
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(LexerTest, EscapedQuotes) {
    auto tokens = Lexer::tokenize(R"(/actor "hello\nworld" show)");
    bool found = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::Parameter && t.value == "hello\nworld")
            found = true;
    }
    EXPECT_TRUE(found);
}

TEST(LexerTest, EmptyInput) {
    auto tokens = Lexer::tokenize("");
    EXPECT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::Eof);
}

TEST(LexerTest, WhitespaceOnly) {
    auto tokens = Lexer::tokenize("   \t  \n  ");
    EXPECT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::Eof);
}

TEST(LexerTest, SystemStats) {
    auto tokens = Lexer::tokenize("/system stats");
    EXPECT_EQ(tokens.size(), 4u); // "/", "system", "stats", Eof
    EXPECT_EQ(tokens[0].value, "/");
    EXPECT_EQ(tokens[1].value, "system");
    EXPECT_EQ(tokens[2].value, "stats");
    EXPECT_EQ(tokens[3].type, TokenType::Eof);
}

TEST(LexerTest, MonitorStartWithFilter) {
    auto tokens = Lexer::tokenize("/monitor start --filter EchoActor");
    bool found_monitor = false, found_start = false, found_filter = false;
    for (auto& t : tokens) {
        if (t.value == "monitor")
            found_monitor = true;
        if (t.value == "start")
            found_start = true;
        if (t.type == TokenType::FlagWithArg && t.value == "filter") {
            found_filter = true;
            ASSERT_TRUE(t.arg.has_value() && *t.arg == "EchoActor");
        }
    }
    EXPECT_TRUE(found_monitor);
    EXPECT_TRUE(found_start);
    EXPECT_TRUE(found_filter);
}
