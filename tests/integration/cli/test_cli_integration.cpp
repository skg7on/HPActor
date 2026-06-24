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

#include <hpactor/cli/command/command_context.hpp>
#include <hpactor/cli/command/command_node.hpp>
#include <hpactor/cli/format/json_formatter.hpp>
#include <hpactor/cli/format/pretty_formatter.hpp>
#include <hpactor/cli/format/tabular_formatter.hpp>
#include <hpactor/cli/io/lexer.hpp>
#include <hpactor/cli/io/pager.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace hpactor::cli;

TEST(CliIntegrationTest, CommandParsingRoundtrip) {
    auto tokens = Lexer::tokenize("/actor 0x123 show --format json");

    CommandNode root{"", "root"};
    auto* actor = root.add_child("actor", "Actor operations");
    auto* id_node = actor->add_child("<id>", "Target actor ID", true);
    id_node->add_child("show", "Display actor metadata");

    CommandContext ctx;
    CommandNode* node = &root;

    // Skip leading "/" keyword (same as CliActor::execute_tokens)
    size_t i = 0;
    if (i < tokens.size() && tokens[i].value == "/") {
        ++i;
    }

    for (; i < tokens.size(); ++i) {
        auto& tok = tokens[i];
        if (tok.type == TokenType::Eof)
            break;
        if (tok.type == TokenType::Flag) {
            ctx.params[tok.value] = "true";
            continue;
        }
        if (tok.type == TokenType::FlagWithArg) {
            ctx.params[tok.value] = tok.arg.value_or("true");
            continue;
        }
        std::string param;
        auto* child = node->find_child(tok.value, param);
        ASSERT_NE(child, nullptr);
        if (child->is_parameter)
            ctx.params[child->keyword] = param;
        node = child;
    }

    EXPECT_EQ(ctx.params["<id>"], "0x123");
    EXPECT_EQ(ctx.params["format"], "json");
    EXPECT_EQ(node->keyword, "show");
}

TEST(CliIntegrationTest, FullCommandWithFlags) {
    auto tokens = Lexer::tokenize("/actor list --format json --no-pager "
                                  "--filter Worker");

    int flags = 0;
    int flag_with_args = 0;
    for (auto& t : tokens) {
        if (t.type == TokenType::Flag)
            flags++;
        if (t.type == TokenType::FlagWithArg)
            flag_with_args++;
    }
    EXPECT_EQ(flags, 1);
    EXPECT_EQ(flag_with_args, 2);
}

TEST(CliIntegrationTest, PagerWithFormatter) {
    Pager pager(5);
    PrettyFormatter fmt;

    int call_count = 0;
    pager.show_page(
        12,
        [&](uint32_t offset, uint32_t limit) {
            call_count++;
            EXPECT_EQ(offset, 0u);
            EXPECT_EQ(limit, 5u);
        },
        &fmt);

    EXPECT_EQ(call_count, 1);
    auto out = fmt.finalize();
    EXPECT_NE(out.find("Page 1 of 3"), std::string::npos);
}

TEST(CliIntegrationTest, FormatterIntegration) {
    PrettyFormatter pf;
    pf.header("Test Header");
    pf.key_value({{"Key1", "Val1"}, {"Key2", "Val2"}});
    pf.raw("Raw text");

    auto out = pf.finalize();
    EXPECT_NE(out.find("Test Header"), std::string::npos);
    EXPECT_NE(out.find("Key1"), std::string::npos);
    EXPECT_NE(out.find("Val2"), std::string::npos);
    EXPECT_NE(out.find("Raw text"), std::string::npos);
}

TEST(CliIntegrationTest, TokenizeAllCommands) {
    struct TestCase {
        const char* input;
        int expected_non_eof;
    };
    TestCase cases[] = {
        {"/actor 5 show", 4},                     // "/", "actor", "5", "show"
        {"/actor 5 kill", 4}, {"/actor list", 3}, // "/", "actor", "list"
        {"/system stats", 3}, {"/system memory", 3},
        {"/metrics show", 3}, {"/topology show", 3},
    };

    for (auto& tc : cases) {
        auto tokens = Lexer::tokenize(tc.input);
        int count = 0;
        for (auto& t : tokens) {
            if (t.type == TokenType::Eof)
                break;
            count++;
        }
        EXPECT_EQ(count, tc.expected_non_eof);
    }
}
