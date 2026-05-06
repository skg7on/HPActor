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

class Lexer {
public:
    // Tokenize a command string into a sequence of tokens.
    // A leading "/" becomes a Keyword "/" (optional, parser auto-inserts if missing).
    static std::vector<Token> tokenize(const std::string& input);

private:
    static std::string unescape(const std::string& s);
};

}  // namespace cli
}  // namespace hpactor
