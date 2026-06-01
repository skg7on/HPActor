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

#pragma once

#include <optional>
#include <string>

namespace hpactor {
namespace cli {

/// \brief Token type classification produced by the Lexer.
enum class TokenType {
    Keyword,     ///< Literal command/verb such as \c actor, \c show, \c list.
    Parameter,   ///< A value: hex id (\c 0x123), string, or quoted string.
    Flag,        ///< Boolean flag: \c --detail, \c --no-pager.
    FlagWithArg, ///< Flag with a value: \c --format \c json, \c --filter \c
                 ///< Worker.
    Eof,         ///< End-of-input sentinel.
};

/// \brief A single token produced by the Lexer from a command-line string.
struct Token {
    /// \brief Token classification.
    TokenType type = TokenType::Eof;
    /// \brief The raw token text.
    std::string value;
    /// \brief Flag argument value, populated only for FlagWithArg tokens.
    std::optional<std::string> arg;
};

} // namespace cli
} // namespace hpactor
