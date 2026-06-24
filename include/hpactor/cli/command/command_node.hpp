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

#include <functional>
#include <hpactor/types/types.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {

struct CommandContext;

/// \brief A node in the CLI command trie.
///
/// Terminal nodes carry an \c execute callback. Non-terminal (intermediate)
/// nodes exist purely for routing. Parameter nodes (\c is_parameter=true)
/// match any runtime token that is not itself a child keyword and capture
/// the matched value into the command context.
struct CommandNode {
    /// \brief Keyword or parameter name for this node.
    std::string keyword;
    /// \brief One-line description shown in help output.
    std::string help_text;
    /// \brief When true, this node matches any token not claimed by a sibling
    ///        keyword and stores the captured value.
    bool is_parameter = false;

    /// \brief Action to invoke when this node is reached.
    ///
    /// Set only on terminal nodes. Called on the CLI daemon thread.
    std::function<result<void>(CommandContext&)> execute{};

    /// \brief Ordered children for deterministic traversal and help output.
    std::vector<std::unique_ptr<CommandNode>> children{};

    /// \brief Add a child node.
    ///
    /// \param[in] kw Keyword for the child node.
    /// \param[in] help Help text for the child.
    /// \param[in] is_param Whether the child is a parameter placeholder.
    /// \return Non-owning pointer to the newly added child.
    CommandNode* add_child(std::string kw, std::string help, bool is_param = false);

    /// \brief Find a child matching the given token.
    ///
    /// First tries exact keyword match. If no keyword matches and a
    /// parameter child exists, that child captures the token value.
    ///
    /// \param[in] token The token to match.
    /// \param[out] param_value Set to the captured value if a parameter
    ///             child matched.
    /// \return Pointer to the matching child, or nullptr.
    CommandNode*
    find_child(const std::string& token, std::string& param_value) const;

    /// \brief Generate help text for this node's children.
    ///
    /// \param[in] indent Number of spaces to indent.
    /// \return Formatted help string listing each child keyword and its
    ///         help text.
    std::string help(int indent = 0) const;

    /// \brief Find a non-parameter child by unique prefix match.
    ///
    /// \param[in] prefix Prefix to match against child keywords.
    /// \return Pointer to the matching child if exactly one child's keyword
    ///         starts with \p prefix; nullptr if zero or multiple match.
    CommandNode* find_child_prefix(const std::string& prefix) const;

    /// \brief Collect tab-completion candidates for a prefix.
    ///
    /// Gathers all non-parameter child keywords that start with \p prefix.
    ///
    /// \param[in] prefix Prefix to match.
    /// \param[out] out Appended with matching keywords.
    void collect_completions(const std::string& prefix,
                             std::vector<std::string>& out) const;

    /// \brief Suggest the closest keyword for a typo.
    ///
    /// Computes Levenshtein distance between \p token and every child keyword.
    ///
    /// \param[in] token The potentially-misspelled token.
    /// \return The closest keyword if within edit distance 2, otherwise an
    ///         empty string.
    std::string suggest(const std::string& token) const;
};

} // namespace cli
} // namespace hpactor
