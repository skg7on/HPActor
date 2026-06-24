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

#include <hpactor/cli/format/output_formatter.hpp>

namespace hpactor {
namespace cli {

/// \brief Tabular (plain-text columns) output formatter.
///
/// Produces grep-friendly plain-text output with aligned columns.
/// No ANSI escape sequences or JSON formatting.
class TabularFormatter : public OutputFormatter {
  public:
    void header(const std::string& title) override;
    void table(const std::vector<std::string>& columns,
               const std::vector<std::vector<std::string>>& rows) override;
    void key_value(const std::map<std::string, std::string>& pairs) override;
    void tree(const TreeNode& root) override;
    void raw(const std::string& text) override;
    void error(const std::string& message) override;

    /// \brief Produce the final tabular output string.
    ///
    /// \return Plain-text accumulated output.
    std::string finalize() override;

  private:
    std::string buffer_;
};

} // namespace cli
} // namespace hpactor
