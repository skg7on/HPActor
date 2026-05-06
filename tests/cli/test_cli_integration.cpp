#include <hpactor/cli/lexer.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/pretty_formatter.hpp>
#include <hpactor/cli/json_formatter.hpp>
#include <hpactor/cli/tabular_formatter.hpp>
#include <hpactor/cli/pager.hpp>
#include <cassert>
#include <cstdio>
#include <string>

using namespace hpactor::cli;

void test_command_parsing_roundtrip() {
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
        if (tok.type == TokenType::Eof) break;
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
        assert(child != nullptr);
        if (child->is_parameter) ctx.params[child->keyword] = param;
        node = child;
    }

    assert(ctx.params["<id>"] == "0x123");
    assert(ctx.params["format"] == "json");
    assert(node->keyword == "show");
}

void test_full_command_with_flags() {
    auto tokens = Lexer::tokenize("/actor list --format json --no-pager --filter Worker");

    int flags = 0;
    int flag_with_args = 0;
    for (auto& t : tokens) {
        if (t.type == TokenType::Flag) flags++;
        if (t.type == TokenType::FlagWithArg) flag_with_args++;
    }
    assert(flags == 1);
    assert(flag_with_args == 2);
}

void test_pager_with_formatter() {
    Pager pager(5);
    PrettyFormatter fmt;

    int call_count = 0;
    pager.show_page(12,
        [&](uint32_t offset, uint32_t limit) {
            call_count++;
            assert(offset == 0);
            assert(limit == 5);
        },
        &fmt);

    assert(call_count == 1);
    auto out = fmt.finalize();
    assert(out.find("Page 1 of 3") != std::string::npos);
}

void test_formatter_integration() {
    PrettyFormatter pf;
    pf.header("Test Header");
    pf.key_value({{"Key1", "Val1"}, {"Key2", "Val2"}});
    pf.raw("Raw text");

    auto out = pf.finalize();
    assert(out.find("Test Header") != std::string::npos);
    assert(out.find("Key1") != std::string::npos);
    assert(out.find("Val2") != std::string::npos);
    assert(out.find("Raw text") != std::string::npos);
}

void test_tokenize_all_commands() {
    struct TestCase { const char* input; int expected_non_eof; };
    TestCase cases[] = {
        {"/actor 5 show", 4},   // "/", "actor", "5", "show"
        {"/actor 5 kill", 4},
        {"/actor list", 3},     // "/", "actor", "list"
        {"/system stats", 3},
        {"/system memory", 3},
        {"/metrics show", 3},
        {"/topology show", 3},
    };

    for (auto& tc : cases) {
        auto tokens = Lexer::tokenize(tc.input);
        int count = 0;
        for (auto& t : tokens) {
            if (t.type == TokenType::Eof) break;
            count++;
        }
        assert(count == tc.expected_non_eof);
    }
}

int main() {
    test_command_parsing_roundtrip();
    test_full_command_with_flags();
    test_pager_with_formatter();
    test_formatter_integration();
    test_tokenize_all_commands();
    printf("test_cli_integration: PASSED\n");
    return 0;
}
