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

#include <hpactor/cli/io/lexer.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <cctype>

namespace hpactor {
namespace cli {

std::string Lexer::unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool escape = false;
    for (char c : s) {
        if (escape) {
            switch (c) {
            case 'n':  out += '\n'; break;
            case 't':  out += '\t'; break;
            case '\\': out += '\\'; break;
            case '"':  out += '"';  break;
            default:   out += '\\'; out += c; break;
            }
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else {
            out += c;
        }
    }
    return out;
}

std::vector<Token> Lexer::tokenize(const std::string& input) {
    FAULT_INJECT("hpactor.cli.lexer.tokenize.corrupt") {
        return {};  // return empty tokens
    }
    std::vector<Token> tokens;
    size_t i = 0;
    size_t n = input.size();

    while (i < n) {
        // Skip whitespace
        while (i < n && std::isspace(static_cast<unsigned char>(input[i]))) ++i;
        if (i >= n) break;

        Token tok;

        // Leading slash is a keyword "/"
        if (input[i] == '/' && (tokens.empty() || tokens.back().type == TokenType::Eof)) {
            tok.type = TokenType::Keyword;
            tok.value = "/";
            ++i;
            tokens.push_back(std::move(tok));
            continue;
        }

        // Flag (--flag or --flag value)
        if (input[i] == '-' && i + 1 < n && input[i + 1] == '-') {
            i += 2;  // skip --
            size_t start = i;
            while (i < n && !std::isspace(static_cast<unsigned char>(input[i]))) ++i;
            std::string flag_name = input.substr(start, i - start);

            // Peek ahead: if the next token is not a flag, it's a flag argument
            size_t j = i;
            while (j < n && std::isspace(static_cast<unsigned char>(input[j]))) ++j;
            bool next_is_flag = (j < n && input[j] == '-' && j + 1 < n && input[j + 1] == '-');
            if (j < n && !next_is_flag) {
                // Next token is the argument
                i = j;  // skip whitespace
                size_t arg_start = i;
                if (input[i] == '"') {
                    ++i;  // skip opening quote
                    arg_start = i;
                    while (i < n && input[i] != '"') {
                        if (input[i] == '\\') ++i;
                        ++i;
                    }
                    std::string arg = unescape(input.substr(arg_start, i - arg_start));
                    ++i;  // skip closing quote
                    tok.type = TokenType::FlagWithArg;
                    tok.value = std::move(flag_name);
                    tok.arg = std::move(arg);
                } else {
                    while (i < n && !std::isspace(static_cast<unsigned char>(input[i]))) ++i;
                    tok.type = TokenType::FlagWithArg;
                    tok.value = std::move(flag_name);
                    tok.arg = input.substr(arg_start, i - arg_start);
                }
            } else {
                tok.type = TokenType::Flag;
                tok.value = std::move(flag_name);
            }
            tokens.push_back(std::move(tok));
            continue;
        }

        // Quoted parameter
        if (input[i] == '"') {
            ++i;  // skip opening quote
            size_t start = i;
            while (i < n && input[i] != '"') {
                if (input[i] == '\\') ++i;
                ++i;
            }
            tok.type = TokenType::Parameter;
            tok.value = unescape(input.substr(start, i - start));
            ++i;  // skip closing quote
            tokens.push_back(std::move(tok));
            continue;
        }

        // Regular keyword or parameter
        {
            size_t start = i;
            while (i < n && !std::isspace(static_cast<unsigned char>(input[i]))) ++i;
            tok.type = TokenType::Keyword;
            tok.value = input.substr(start, i - start);
            tokens.push_back(std::move(tok));
        }
    }

    tokens.push_back(Token{TokenType::Eof, "", std::nullopt});
    return tokens;
}

}  // namespace cli
}  // namespace hpactor
