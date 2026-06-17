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

#include <hpactor/adt/json_helpers.hpp>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hpactor {
namespace adt {

// ---------------------------------------------------------------------------
// String escaping
// ---------------------------------------------------------------------------

std::string json_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '"':
                    out += '"';
                    ++i;
                    break;
                case '\\':
                    out += '\\';
                    ++i;
                    break;
                case '/':
                    out += '/';
                    ++i;
                    break;
                case 'n':
                    out += '\n';
                    ++i;
                    break;
                case 't':
                    out += '\t';
                    ++i;
                    break;
                case 'r':
                    out += '\r';
                    ++i;
                    break;
                case 'b':
                    out += '\b';
                    ++i;
                    break;
                case 'f':
                    out += '\f';
                    ++i;
                    break;
                default:
                    out += s[i];
                    break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
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
            case '\r':
                out += "\\r";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Whitespace
// ---------------------------------------------------------------------------

size_t skip_json_ws(const std::string& json, size_t pos) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) {
        ++pos;
    }
    return pos;
}

// ---------------------------------------------------------------------------
// Primitive extractors
// ---------------------------------------------------------------------------

std::string extract_json_string(const std::string& json, size_t& pos) {
    ++pos; // skip opening quote
    size_t start = pos;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size())
            ++pos; // skip escaped char
        ++pos;
    }
    std::string raw = json.substr(start, pos - start);
    if (pos < json.size())
        ++pos; // skip closing quote
    return json_unescape(raw);
}

std::string extract_json_object_raw(const std::string& json, size_t& pos) {
    if (pos >= json.size() || json[pos] != '{')
        return {};
    size_t start = pos;
    ++pos;
    int depth = 1;
    bool in_string = false;
    while (pos < json.size() && depth > 0) {
        char c = json[pos];
        if (in_string) {
            if (c == '\\' && pos + 1 < json.size())
                ++pos; // skip escaped
            else if (c == '"')
                in_string = false;
        } else {
            if (c == '"')
                in_string = true;
            else if (c == '{')
                ++depth;
            else if (c == '}')
                --depth;
        }
        ++pos;
    }
    return json.substr(start, pos - start);
}

std::string extract_json_array_raw(const std::string& json, size_t& pos) {
    if (pos >= json.size() || json[pos] != '[')
        return {};
    size_t start = pos;
    ++pos;
    int depth = 1;
    bool in_string = false;
    while (pos < json.size() && depth > 0) {
        char c = json[pos];
        if (in_string) {
            if (c == '\\' && pos + 1 < json.size())
                ++pos;
            else if (c == '"')
                in_string = false;
        } else {
            if (c == '"')
                in_string = true;
            else if (c == '[')
                ++depth;
            else if (c == ']')
                --depth;
        }
        ++pos;
    }
    return json.substr(start, pos - start);
}

// ---------------------------------------------------------------------------
// Structured parsers
// ---------------------------------------------------------------------------

std::vector<std::string> parse_json_string_array(const std::string& arr) {
    std::vector<std::string> result;
    size_t pos = 0;
    pos = skip_json_ws(arr, pos);
    if (pos >= arr.size() || arr[pos] != '[')
        return result;
    ++pos;
    while (pos < arr.size()) {
        pos = skip_json_ws(arr, pos);
        if (pos >= arr.size())
            break;
        if (arr[pos] == ']') {
            ++pos;
            break;
        }
        if (arr[pos] == ',') {
            ++pos;
            continue;
        }
        if (arr[pos] == '"') {
            result.push_back(extract_json_string(arr, pos));
            continue;
        }
        ++pos;
    }
    return result;
}

std::vector<std::pair<std::string, std::string>>
parse_json_string_map(const std::string& obj) {
    std::vector<std::pair<std::string, std::string>> result;
    size_t pos = 0;
    pos = skip_json_ws(obj, pos);
    if (pos >= obj.size() || obj[pos] != '{')
        return result;
    ++pos;
    while (pos < obj.size()) {
        pos = skip_json_ws(obj, pos);
        if (pos >= obj.size())
            break;
        if (obj[pos] == '}') {
            ++pos;
            break;
        }
        if (obj[pos] == ',') {
            ++pos;
            continue;
        }
        if (obj[pos] == '"') {
            std::string key = extract_json_string(obj, pos);
            pos = skip_json_ws(obj, pos);
            if (pos < obj.size() && obj[pos] == ':')
                ++pos;
            pos = skip_json_ws(obj, pos);
            std::string value;
            if (pos < obj.size() && obj[pos] == '"') {
                value = extract_json_string(obj, pos);
            } else {
                size_t tok_start = pos;
                while (pos < obj.size() && obj[pos] != ',' && obj[pos] != '}' &&
                       obj[pos] != ' ' && obj[pos] != '\t' &&
                       obj[pos] != '\n' && obj[pos] != '\r') {
                    ++pos;
                }
                value = obj.substr(tok_start, pos - tok_start);
            }
            result.emplace_back(std::move(key), std::move(value));
            continue;
        }
        ++pos;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Declarative JSON Builder
// ---------------------------------------------------------------------------

JsonBuilder JsonBuilder::root_object() {
    JsonBuilder b;
    b.buf_ += '{';
    b.stack_.push_back({StackFrame::kObject, false});
    return b;
}

JsonBuilder& JsonBuilder::object(const char* key) {
    pre_value();
    emit_key(key);
    buf_ += '{';
    stack_.push_back({StackFrame::kObject, false});
    return *this;
}

JsonBuilder& JsonBuilder::array(const char* key) {
    pre_value();
    emit_key(key);
    buf_ += '[';
    stack_.push_back({StackFrame::kArray, false});
    return *this;
}

JsonBuilder& JsonBuilder::object() {
    pre_value();
    buf_ += '{';
    stack_.push_back({StackFrame::kObject, false});
    return *this;
}

JsonBuilder& JsonBuilder::array() {
    pre_value();
    buf_ += '[';
    stack_.push_back({StackFrame::kArray, false});
    return *this;
}

JsonBuilder& JsonBuilder::end_object() {
    buf_ += '}';
    stack_.pop_back();
    if (!stack_.empty()) {
        stack_.back().needs_comma = true;
    }
    return *this;
}

JsonBuilder& JsonBuilder::end_array() {
    buf_ += ']';
    stack_.pop_back();
    if (!stack_.empty()) {
        stack_.back().needs_comma = true;
    }
    return *this;
}

// ── Leaf fields (keyed) ──────────────────────────────────────────────

JsonBuilder& JsonBuilder::field(const char* key, const std::string& v) {
    pre_value();
    emit_key(key);
    emit_string(v);
    return *this;
}

JsonBuilder& JsonBuilder::field(const char* key, double v) {
    pre_value();
    emit_key(key);
    std::ostringstream oss;
    oss << v;
    buf_ += oss.str();
    return *this;
}

JsonBuilder& JsonBuilder::field(const char* key, bool v) {
    pre_value();
    emit_key(key);
    buf_ += v ? "true" : "false";
    return *this;
}

JsonBuilder& JsonBuilder::null_field(const char* key) {
    pre_value();
    emit_key(key);
    buf_ += "null";
    return *this;
}

// ── Array elements (unkeyed) ─────────────────────────────────────────

JsonBuilder& JsonBuilder::element(const std::string& v) {
    pre_value();
    emit_string(v);
    return *this;
}

JsonBuilder& JsonBuilder::element(double v) {
    pre_value();
    std::ostringstream oss;
    oss << v;
    buf_ += oss.str();
    return *this;
}

JsonBuilder& JsonBuilder::element(bool v) {
    pre_value();
    buf_ += v ? "true" : "false";
    return *this;
}

// ── Finalize ─────────────────────────────────────────────────────────

std::string JsonBuilder::build() {
    // Auto-close all remaining open structures.
    while (!stack_.empty()) {
        if (stack_.back().kind == StackFrame::kObject) {
            buf_ += '}';
        } else {
            buf_ += ']';
        }
        stack_.pop_back();
    }
    return buf_;
}

void JsonBuilder::reset() {
    buf_.clear();
    stack_.clear();
}

// ── Private helpers ──────────────────────────────────────────────────

void JsonBuilder::pre_value() {
    if (!stack_.empty()) {
        if (stack_.back().needs_comma) {
            buf_ += ',';
            stack_.back().needs_comma = false;
        }
        stack_.back().needs_comma = true;
    }
}

void JsonBuilder::emit_key(const char* key) {
    buf_ += '"';
    buf_ += key;
    buf_ += "\":";
}

void JsonBuilder::emit_string(const std::string& v) {
    buf_ += '"';
    buf_ += json_escape(v);
    buf_ += '"';
}

} // namespace adt
} // namespace hpactor
