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

#include <hpactor/cli/format/tabular_formatter.hpp>

#include <algorithm>

namespace hpactor {
namespace cli {

void TabularFormatter::header(const std::string& title) {
    buffer_ += "# " + title + "\n";
}

void TabularFormatter::table(const std::vector<std::string>& cols,
                             const std::vector<std::vector<std::string>>& rows) {
    if (cols.empty())
        return;
    std::vector<size_t> widths(cols.size());
    for (size_t i = 0; i < cols.size(); ++i)
        widths[i] = cols[i].size();
    for (const auto& row : rows)
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i)
            widths[i] = std::max(widths[i], row[i].size());

    for (size_t i = 0; i < cols.size(); ++i)
        buffer_ += cols[i] + std::string(widths[i] - cols[i].size(), ' ') + "  ";
    buffer_ += "\n";

    for (const auto& row : rows) {
        for (size_t i = 0; i < cols.size(); ++i) {
            std::string val = i < row.size() ? row[i] : "-";
            buffer_ +=
                val +
                std::string(widths[i] > val.size() ? widths[i] - val.size() : 0,
                            ' ') +
                "  ";
        }
        buffer_ += "\n";
    }
}

void TabularFormatter::key_value(const std::map<std::string, std::string>& pairs) {
    for (const auto& kv : pairs)
        buffer_ += kv.first + ": " + kv.second + "\n";
}

void TabularFormatter::tree(const TreeNode& root) {
    auto print = [this](auto& self, const TreeNode& node, int depth) -> void {
        std::string indent(static_cast<size_t>(depth) * 2, ' ');
        buffer_ += indent + node.name;
        if (!node.description.empty())
            buffer_ += "  # " + node.description;
        buffer_ += "\n";
        for (const auto& child : node.children)
            self(self, child, depth + 1);
    };
    print(print, root, 0);
}

void TabularFormatter::raw(const std::string& text) {
    buffer_ += text;
    if (!text.empty() && text.back() != '\n')
        buffer_ += '\n';
}

void TabularFormatter::error(const std::string& message) {
    buffer_ += "ERROR: " + message + "\n";
}

std::string TabularFormatter::finalize() {
    return std::move(buffer_);
}

} // namespace cli
} // namespace hpactor