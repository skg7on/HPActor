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

enum class TokenType {
    Keyword,         // actor, show, list, etc.
    Parameter,       // 0x123, echo-actor-1, "quoted string"
    Flag,            // --detail, --no-pager
    FlagWithArg,     // --format json, --filter Worker
    Eof,
};

struct Token {
    TokenType type = TokenType::Eof;
    std::string value;
    std::optional<std::string> arg;  // populated for FlagWithArg
};

}  // namespace cli
}  // namespace hpactor
