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

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hpactor {
namespace adt {

// ── String escaping ─────────────────────────────────────────────────

/// \brief Unescape a JSON string value (handles \\", \\\\, \\n, \\t, \\r,
/// \\b, \\f).
std::string json_unescape(const std::string& s);

/// \brief Escape a string for JSON output (quotes, backslashes, control
/// characters).
std::string json_escape(const std::string& s);

// ── Whitespace ──────────────────────────────────────────────────────

/// \brief Skip JSON whitespace. Returns new position.
size_t skip_json_ws(const std::string& json, size_t pos);

// ── Primitive extractors ────────────────────────────────────────────

/// \brief Extract a JSON string value starting at @p pos (which must
/// point to '"'). Returns the unescaped string and advances @p pos
/// past the closing quote.
std::string extract_json_string(const std::string& json, size_t& pos);

/// \brief Extract a JSON object `{ ... }` as a raw substring.
/// @p pos must point to '{'.
std::string extract_json_object_raw(const std::string& json, size_t& pos);

/// \brief Extract a JSON array `[ ... ]` as a raw substring.
/// @p pos must point to '['.
std::string extract_json_array_raw(const std::string& json, size_t& pos);

// ── Structured parsers ──────────────────────────────────────────────

/// \brief Extract string values from a JSON array like `["a","b","c"]`.
std::vector<std::string> parse_json_string_array(const std::string& arr);

/// \brief Extract key-value pairs from a JSON object like
/// `{"k1":"v1","k2":"v2"}`.
std::vector<std::pair<std::string, std::string>>
parse_json_string_map(const std::string& obj);

// ── Declarative JSON Builder ────────────────────────────────────────

/// \brief Declarative, chainable JSON builder that produces valid JSON
/// without manual string concatenation.
///
/// Start building with JsonBuilder::root_object(), then chain field(),
/// object(), array(), element(), and null_field() calls. Close nested
/// structures with end_object() / end_array(). Call build() to retrieve
/// the final JSON string, or reset() to clear and reuse the builder.
///
/// String values are automatically escaped via json_escape().
class JsonBuilder {
  public:
    /// \brief Create a builder starting at the root object `{`.
    static JsonBuilder root_object();

    // ── Structure open (keyed) ──────────────────────────────────────

    /// \brief Open a nested object with the given key: `"key": {`.
    JsonBuilder& object(const char* key);

    /// \brief Open a nested array with the given key: `"key": [`.
    JsonBuilder& array(const char* key);

    // ── Structure open (unkeyed, for array elements) ─────────────────

    /// \brief Open a nested object (no key): `{`.
    JsonBuilder& object();

    /// \brief Open a nested array (no key): `[`.
    JsonBuilder& array();

    // ── Structure close ──────────────────────────────────────────────

    /// \brief Close the innermost object: `}`.
    JsonBuilder& end_object();

    /// \brief Close the innermost array: `]`.
    JsonBuilder& end_array();

    // ── Leaf fields (keyed) ──────────────────────────────────────────

    JsonBuilder& field(const char* key, const std::string& v);
    JsonBuilder& field(const char* key, uint64_t v);
    JsonBuilder& field(const char* key, int64_t v);
    JsonBuilder& field(const char* key, uint32_t v);
    JsonBuilder& field(const char* key, int32_t v);
    JsonBuilder& field(const char* key, double v);
    JsonBuilder& field(const char* key, bool v);

    /// \brief Emit `"key": null`.
    JsonBuilder& null_field(const char* key);

    // ── Array elements (unkeyed) ─────────────────────────────────────

    JsonBuilder& element(const std::string& v);
    JsonBuilder& element(uint64_t v);
    JsonBuilder& element(double v);
    JsonBuilder& element(bool v);

    // ── Finalize ─────────────────────────────────────────────────────

    /// \brief Return the built JSON string, auto-closing any open
    /// structures.
    std::string build();

    /// \brief Reset the builder to its initial empty state.
    void reset();

  private:
    struct StackFrame {
        enum Kind { kObject, kArray };
        Kind kind;
        bool needs_comma = false;
    };

    std::string buf_;
    std::vector<StackFrame> stack_;

    /// \brief Insert a comma if the current frame already has a value.
    void pre_value();

    /// \brief Emit `"key":` into the buffer.
    void emit_key(const char* key);

    /// \brief Emit a JSON-escaped string value: `"<escaped>"`.
    void emit_string(const std::string& v);
};

} // namespace adt
} // namespace hpactor
