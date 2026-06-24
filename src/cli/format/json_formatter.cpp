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

namespace hpactor {
namespace cli {

std::string JsonFormatter::escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
    out += '"';
    return out;
}

void JsonFormatter::header(const std::string& /*title*/) {} // no-op

void JsonFormatter::table(const std::vector<std::string>& columns,
                          const std::vector<std::vector<std::string>>& rows) {
    buf_ += "[";
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r > 0)
            buf_ += ",";
        buf_ += "{";
        for (size_t c = 0; c < columns.size() && c < rows[r].size(); ++c) {
            if (c > 0)
                buf_ += ",";
            buf_ += escape(columns[c]) + ":" + escape(rows[r][c]);
        }
        buf_ += "}";
    }
    buf_ += "]";
}

void JsonFormatter::key_value(const std::map<std::string, std::string>& pairs) {
    buf_ += "{";
    bool first = true;
    for (const auto& kv : pairs) {
        if (!first)
            buf_ += ",";
        first = false;
        buf_ += escape(kv.first) + ":" + escape(kv.second);
    }
    buf_ += "}";
}

void JsonFormatter::tree(const TreeNode& root) {
    buf_ += "{" + escape("name") + ":" + escape(root.name) + "," +
            escape("description") + ":" + escape(root.description) + "," +
            escape("children") + ":";
    json_tree(root);
    buf_ += "}";
}

void JsonFormatter::json_tree(const TreeNode& node) {
    buf_ += "[";
    for (size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0)
            buf_ += ",";
        tree(node.children[i]);
    }
    buf_ += "]";
}

void JsonFormatter::raw(const std::string& text) {
    buf_ += escape(text);
}

void JsonFormatter::error(const std::string& message) {
    buf_ += "{\"error\":" + escape(message) + "}";
}

std::string JsonFormatter::finalize() {
    return std::move(buf_);
}

} // namespace cli
} // namespace hpactor