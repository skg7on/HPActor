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

} // namespace adt
} // namespace hpactor
