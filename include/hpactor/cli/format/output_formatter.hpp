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

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {

/// \brief A node in an output tree for hierarchical rendering.
struct TreeNode {
    /// \brief Display name for this node.
    std::string name;
    /// \brief Optional description text.
    std::string description;
    /// \brief Child nodes, in display order.
    std::vector<TreeNode> children;
};

/// \brief Abstract interface for CLI output formatting.
///
/// Concrete formatters (PrettyFormatter, JsonFormatter, TabularFormatter)
/// accumulate content through method calls and produce the final string
/// via \c finalize().
///
/// \note Thread affinity: called on the CLI daemon thread.
class OutputFormatter {
  public:
    virtual ~OutputFormatter() = default;

    /// \brief Render a titled section header.
    ///
    /// \param[in] title Header text.
    virtual void header(const std::string& title) = 0;

    /// \brief Render a table with column headers and rows.
    ///
    /// \param[in] columns Column header labels.
    /// \param[in] rows Row data, each inner vector having the same size
    ///                 as \p columns.
    virtual void table(const std::vector<std::string>& columns,
                       const std::vector<std::vector<std::string>>& rows) = 0;

    /// \brief Render key-value pairs.
    ///
    /// \param[in] pairs Key-value map to render.
    virtual void key_value(const std::map<std::string, std::string>& pairs) = 0;

    /// \brief Render a hierarchical tree.
    ///
    /// \param[in] root Root node of the tree.
    virtual void tree(const TreeNode& root) = 0;

    /// \brief Append raw text without formatting.
    ///
    /// \param[in] text Arbitrary text string.
    virtual void raw(const std::string& text) = 0;

    /// \brief Render an error message.
    ///
    /// \param[in] message Error text.
    virtual void error(const std::string& message) = 0;

    /// \brief Produce the final formatted output string.
    ///
    /// \return The accumulated formatted content.
    virtual std::string finalize() = 0;

    /// \brief Factory for creating formatters by name.
    ///
    /// \param[in] format Format name: "pretty", "json", or "tabular".
    /// \return A heap-allocated formatter instance.
    static std::unique_ptr<OutputFormatter> create(const std::string& format);
};

} // namespace cli
} // namespace hpactor
