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

#include <hpactor/types/types.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {

struct CommandContext;

struct CommandNode {
    std::string keyword;
    std::string help_text;
    bool is_parameter = false;  // true for <id>, <filter>, etc.

    // Leaf action — set only on terminal nodes.
    std::function<result<void>(CommandContext&)> execute{};

    // Ordered children for deterministic traversal.
    std::vector<std::unique_ptr<CommandNode>> children{};

    // Builder API
    CommandNode* add_child(std::string kw, std::string help, bool is_param = false);

    // Lookup child by token. If is_parameter, matches any token
    // that is not itself a child keyword, and stores the matched value.
    CommandNode* find_child(const std::string& token, std::string& param_value) const;

    // Generate help text for this node's children.
    std::string help(int indent = 0) const;

    // Find a non-parameter child by prefix match.
    // Returns the child if exactly one matches, nullptr if zero or multiple.
    CommandNode* find_child_prefix(const std::string& prefix) const;

    // Collect all non-parameter children whose keyword starts with prefix.
    void collect_completions(const std::string& prefix,
                             std::vector<std::string>& out) const;

    // Suggest closest match for typos (Levenshtein distance <= 2).
    std::string suggest(const std::string& token) const;
};

}  // namespace cli
}  // namespace hpactor
