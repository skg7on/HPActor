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

/// \file test_fuzz_regression_lexer.cpp
/// \brief Regression tests for CLI lexer fuzz findings.

#include <gtest/gtest.h>
#include <hpactor/cli/io/lexer.hpp>
#include <hpactor/cli/token.hpp>

using namespace hpactor;
using namespace hpactor::cli;

TEST(FuzzRegressionLexer, EmptyInput) {
    auto tokens = Lexer::tokenize("");
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
}

TEST(FuzzRegressionLexer, OnlySlash) {
    auto tokens = Lexer::tokenize("/");
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
}

TEST(FuzzRegressionLexer, SimpleCommand) {
    auto tokens = Lexer::tokenize("/actor list");
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
    // Should have at least the keyword and arg tokens
    EXPECT_GE(tokens.size(), 2u);
}

TEST(FuzzRegressionLexer, QuotedString) {
    auto tokens = Lexer::tokenize("/foo \"hello world\"");
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
}

TEST(FuzzRegressionLexer, FlagWithArg) {
    auto tokens = Lexer::tokenize("/foo --flag value");
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
}

TEST(FuzzRegressionLexer, CommentToken) {
    auto tokens = Lexer::tokenize("/foo arg # comment text");
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
}

TEST(FuzzRegressionLexer, UnterminatedQuote) {
    auto tokens = Lexer::tokenize("/foo \"hello");
    EXPECT_FALSE(tokens.empty());
    // Lexer must not crash on unterminated quotes
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
}

TEST(FuzzRegressionLexer, BinaryInput) {
    // Binary non-ASCII input — must not crash
    std::string binary_input;
    for (int i = 0; i < 256; ++i)
        binary_input.push_back(static_cast<char>(i));
    auto tokens = Lexer::tokenize(binary_input);
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
}

TEST(FuzzRegressionLexer, LongInput) {
    // 10,000-character input — must not crash or exhaust memory
    std::string long_input(10000, 'x');
    auto tokens = Lexer::tokenize(long_input);
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
}

TEST(FuzzRegressionLexer, WhitespaceOnly) {
    auto tokens = Lexer::tokenize("   \t  \n  ");
    EXPECT_FALSE(tokens.empty());
    EXPECT_EQ(tokens.back().type, TokenType::Eof);
}
