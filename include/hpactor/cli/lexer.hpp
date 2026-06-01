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

#include <hpactor/cli/token.hpp>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {

/// \brief Tokenizer for CLI command-line input.
///
/// Splits an input string into tokens, handling quoted strings, flags with
/// arguments, and comment characters. A leading "/" is normalized into a
/// Keyword token.
///
/// \note Thread affinity: called on the CLI daemon thread.
class Lexer {
  public:
    /// \brief Tokenize a command string into a sequence of tokens.
    ///
    /// A leading "/" becomes a Keyword "/" token. The parser auto-inserts
    /// a leading "/" if missing, so callers may safely omit it.
    ///
    /// \param[in] input Raw command-line string.
    /// \return Ordered vector of tokens. Always contains at least an Eof
    ///         token for empty input.
    static std::vector<Token> tokenize(const std::string& input);

  private:
    /// \brief Unescape backslash sequences in a quoted string.
    static std::string unescape(const std::string& s);
};

} // namespace cli
} // namespace hpactor
