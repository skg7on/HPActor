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

#include <hpactor/cli/format/json_formatter.hpp>
#include <hpactor/cli/format/pretty_formatter.hpp>
#include <hpactor/cli/format/tabular_formatter.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace hpactor {
namespace cli {

// UTF-8 box-drawing characters
static constexpr const char* kHDash = "\xe2\x94\x80";              // ─
static constexpr const char* kBranch = "\xe2\x94\x9c\xe2\x94\x80"; // ├─
static constexpr const char* kLast = "\xe2\x94\x94\xe2\x94\x80";   // └─

std::string PrettyFormatter::pad_right(const std::string& s, size_t width) {
    if (s.size() >= width)
        return s.substr(0, width);
    return s + std::string(width - s.size(), ' ');
}

std::string PrettyFormatter::horizontal_rule(size_t width) {
    std::string result;
    result.reserve(width * 3);
    for (size_t i = 0; i < width; ++i)
        result += kHDash;
    return result;
}

void PrettyFormatter::header(const std::string& title) {
    auto* cols_env = std::getenv("COLUMNS");
    if (cols_env)
        columns_ = std::atoi(cols_env);
    if (columns_ < 40)
        columns_ = 80;
    buffer_ += "\n" + bold(title) + "\n";
    buffer_ += dim(horizontal_rule(static_cast<size_t>(columns_))) + "\n";
}

void PrettyFormatter::table(const std::vector<std::string>& cols,
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
        buffer_ += bold(pad_right(cols[i], widths[i] + 2));
    buffer_ += "\n";

    for (size_t i = 0; i < cols.size(); ++i)
        buffer_ += dim(pad_right(horizontal_rule(widths[i]), widths[i] + 2));
    buffer_ += "\n";

    for (const auto& row : rows) {
        for (size_t i = 0; i < cols.size(); ++i)
            buffer_ += pad_right(i < row.size() ? row[i] : "-", widths[i] + 2);
        buffer_ += "\n";
    }
}

void PrettyFormatter::key_value(const std::map<std::string, std::string>& pairs) {
    size_t max_key = 0;
    for (const auto& kv : pairs)
        max_key = std::max(max_key, kv.first.size());
    for (const auto& kv : pairs)
        buffer_ += "  " + cyan(pad_right(kv.first, max_key + 2)) +
                   green(kv.second) + "\n";
}

void PrettyFormatter::tree(const TreeNode& root) {
    auto print_node = [this](auto& self, const TreeNode& node, int depth,
                             bool last) -> void {
        std::string indent(static_cast<size_t>(depth) * 2, ' ');
        if (depth > 0)
            buffer_ += indent + std::string(last ? kLast : kBranch) + " ";
        buffer_ += bold(node.name);
        if (!node.description.empty())
            buffer_ += "  " + dim(node.description);
        buffer_ += "\n";
        for (size_t i = 0; i < node.children.size(); ++i)
            self(self, node.children[i], depth + 1, i == node.children.size() - 1);
    };
    print_node(print_node, root, 0, true);
}

void PrettyFormatter::raw(const std::string& text) {
    buffer_ += text;
    if (!text.empty() && text.back() != '\n')
        buffer_ += '\n';
}

void PrettyFormatter::error(const std::string& message) {
    buffer_ += red("Error: ") + message + "\n";
}

std::string PrettyFormatter::finalize() {
    return std::move(buffer_);
}

std::unique_ptr<OutputFormatter>
OutputFormatter::create(const std::string& format) {
    if (format == "json")
        return std::make_unique<JsonFormatter>();
    if (format == "tabular")
        return std::make_unique<TabularFormatter>();
    return std::make_unique<PrettyFormatter>();
}

} // namespace cli
} // namespace hpactor