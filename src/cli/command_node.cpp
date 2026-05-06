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

#include <hpactor/cli/command_node.hpp>
#include <algorithm>
#include <string>

namespace hpactor {
namespace cli {

CommandNode* CommandNode::add_child(std::string kw, std::string help, bool is_param) {
    auto node = std::make_unique<CommandNode>();
    node->keyword = std::move(kw);
    node->help_text = std::move(help);
    node->is_parameter = is_param;
    CommandNode* ptr = node.get();
    children.push_back(std::move(node));
    return ptr;
}

CommandNode* CommandNode::find_child(const std::string& token,
                                      std::string& param_value) const {
    // Exact keyword match first.
    for (auto& child : children) {
        if (!child->is_parameter && child->keyword == token) {
            return child.get();
        }
    }
    // Parameter match: any child with is_parameter=true matches any token.
    for (auto& child : children) {
        if (child->is_parameter) {
            param_value = token;
            return child.get();
        }
    }
    return nullptr;
}

static int levenshtein(const std::string& a, const std::string& b) {
    size_t m = a.size(), n = b.size();
    // Small strings — simple DP on stack is fine.
    int d[32][32];
    for (size_t i = 0; i <= m; ++i) d[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= n; ++j) d[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
        }
    }
    return d[m][n];
}

CommandNode* CommandNode::find_child_prefix(const std::string& prefix) const {
    CommandNode* found = nullptr;
    for (auto& child : children) {
        if (child->is_parameter) continue;
        if (child->keyword.starts_with(prefix)) {
            if (found) return nullptr; // ambiguous — multiple matches
            found = child.get();
        }
    }
    return found;
}

void CommandNode::collect_completions(const std::string& prefix,
                                      std::vector<std::string>& out) const {
    for (auto& child : children) {
        if (child->is_parameter) continue;
        if (prefix.empty() || child->keyword.starts_with(prefix)) {
            out.push_back(child->keyword);
        }
    }
}

std::string CommandNode::suggest(const std::string& token) const {
    std::string best;
    int best_dist = 999;
    for (auto& child : children) {
        if (child->is_parameter) continue;
        int dist = levenshtein(token, child->keyword);
        if (dist <= 2 && dist < best_dist) {
            best_dist = dist;
            best = child->keyword;
        }
    }
    return best;
}

std::string CommandNode::help(int indent) const {
    std::string out;
    std::string pad(static_cast<size_t>(indent), ' ');
    for (auto& child : children) {
        out += pad;
        if (child->is_parameter) {
            out += "<" + child->keyword + ">";
        } else {
            out += child->keyword;
        }
        out += "  —  " + child->help_text + "\n";

        if (!child->children.empty()) {
            out += child->help(indent + 2);
        }
    }
    return out;
}

}  // namespace cli
}  // namespace hpactor
