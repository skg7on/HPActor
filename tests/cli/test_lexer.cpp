#include <hpactor/cli/lexer.hpp>
#include <cassert>
#include <cstdio>
#include <string>

using namespace hpactor::cli;

void test_simple_keywords() {
    auto tokens = Lexer::tokenize("actor list");
    assert(tokens.size() == 3);  // keyword, keyword, eof
    assert(tokens[0].type == TokenType::Keyword);
    assert(tokens[0].value == "actor");
    assert(tokens[1].type == TokenType::Keyword);
    assert(tokens[1].value == "list");
    assert(tokens[2].type == TokenType::Eof);
}

void test_leading_slash() {
    auto tokens = Lexer::tokenize("/actor 5 show");
    assert(tokens.size() == 5);  // "/", "actor", "5", "show", Eof
    assert(tokens[0].type == TokenType::Keyword);
    assert(tokens[0].value == "/");
    assert(tokens[1].type == TokenType::Keyword);
    assert(tokens[1].value == "actor");
    assert(tokens[2].type == TokenType::Keyword);
    assert(tokens[2].value == "5");
    assert(tokens[3].type == TokenType::Keyword);
    assert(tokens[3].value == "show");
    assert(tokens[4].type == TokenType::Eof);
}

void test_flags() {
    auto tokens = Lexer::tokenize("/actor list --detail --no-pager");
    assert(tokens.size() >= 4);
    bool found_detail = false, found_nopager = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::Flag && t.value == "detail") found_detail = true;
        if (t.type == TokenType::Flag && t.value == "no-pager") found_nopager = true;
    }
    assert(found_detail);
    assert(found_nopager);
}

void test_flag_with_arg() {
    auto tokens = Lexer::tokenize("/actor list --format json --filter Worker");
    bool found_format = false, found_filter = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::FlagWithArg && t.value == "format") {
            found_format = true;
            assert(t.arg && *t.arg == "json");
        }
        if (t.type == TokenType::FlagWithArg && t.value == "filter") {
            found_filter = true;
            assert(t.arg && *t.arg == "Worker");
        }
    }
    assert(found_format);
    assert(found_filter);
}

void test_hex_actor_id() {
    auto tokens = Lexer::tokenize("/actor 0x123 show");
    bool found_id = false;
    for (auto& t : tokens) {
        if (t.value == "0x123") found_id = true;
    }
    assert(found_id);
}

void test_quoted_string() {
    auto tokens = Lexer::tokenize("/actor \"My Worker 3\" show");
    bool found = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::Parameter && t.value == "My Worker 3") found = true;
    }
    assert(found);
}

void test_escaped_quotes() {
    auto tokens = Lexer::tokenize(R"(/actor "hello\nworld" show)");
    bool found = false;
    for (auto& t : tokens) {
        if (t.type == TokenType::Parameter && t.value == "hello\nworld") found = true;
    }
    assert(found);
}

void test_empty_input() {
    auto tokens = Lexer::tokenize("");
    assert(tokens.size() == 1);
    assert(tokens[0].type == TokenType::Eof);
}

void test_whitespace_only() {
    auto tokens = Lexer::tokenize("   \t  \n  ");
    assert(tokens.size() == 1);
    assert(tokens[0].type == TokenType::Eof);
}

void test_system_stats() {
    auto tokens = Lexer::tokenize("/system stats");
    assert(tokens.size() == 4);  // "/", "system", "stats", Eof
    assert(tokens[0].value == "/");
    assert(tokens[1].value == "system");
    assert(tokens[2].value == "stats");
    assert(tokens[3].type == TokenType::Eof);
}

void test_monitor_start_with_filter() {
    auto tokens = Lexer::tokenize("/monitor start --filter EchoActor");
    bool found_monitor = false, found_start = false, found_filter = false;
    for (auto& t : tokens) {
        if (t.value == "monitor") found_monitor = true;
        if (t.value == "start") found_start = true;
        if (t.type == TokenType::FlagWithArg && t.value == "filter") {
            found_filter = true;
            assert(t.arg && *t.arg == "EchoActor");
        }
    }
    assert(found_monitor);
    assert(found_start);
    assert(found_filter);
}

int main() {
    test_simple_keywords();
    test_leading_slash();
    test_flags();
    test_flag_with_arg();
    test_hex_actor_id();
    test_quoted_string();
    test_escaped_quotes();
    test_empty_input();
    test_whitespace_only();
    test_system_stats();
    test_monitor_start_with_filter();
    printf("test_lexer: PASSED\n");
    return 0;
}
