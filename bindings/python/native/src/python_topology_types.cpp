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

#include <hpactor/python/python_topology_types.hpp>

#include <hpactor/config/topology_model.hpp>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace hpactor::python {
namespace {

/// \brief Check if a character is valid for the start of a Python identifier.
constexpr bool is_ident_start(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

/// \brief Check if a character is valid for the body of a Python identifier.
constexpr bool is_ident_body(char c) noexcept {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/// \brief Validate a dotted segment name (module or qualname segment).
/// Must match [A-Za-z_][A-Za-z0-9_]*
bool is_valid_segment(std::string_view segment) noexcept {
    if (segment.empty())
        return false;
    if (!is_ident_start(segment[0]))
        return false;
    for (size_t i = 1; i < segment.size(); ++i) {
        if (!is_ident_body(segment[i]))
            return false;
    }
    return true;
}

/// \brief Validate a dotted name (module or qualname).
/// Every segment separated by '.' must be valid.
bool is_valid_dotted_name(std::string_view name) noexcept {
    if (name.empty())
        return false;
    if (name.front() == '.' || name.back() == '.')
        return false;

    size_t pos = 0;
    while (pos < name.size()) {
        auto dot = name.find('.', pos);
        auto segment = name.substr(pos, dot - pos);
        if (!is_valid_segment(segment))
            return false;
        if (dot == std::string_view::npos)
            break;
        pos = dot + 1;
    }
    return true;
}

/// \brief FNV-1a 64-bit hash.
constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

uint64_t hash_bytes(const uint8_t* data, size_t len, uint64_t seed) noexcept {
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= kFnvPrime;
    }
    return h;
}

uint64_t hash_u64(uint64_t v, uint64_t seed) noexcept {
    return hash_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v), seed);
}

/// \brief Hash a string with its length prefix for domain separation.
uint64_t hash_length_prefixed(std::string_view s, uint64_t seed) noexcept {
    seed = hash_u64(s.size(), seed);
    return hash_bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size(), seed);
}

} // namespace

// ── Behavior reference parsing ──────────────────────────────────────────────

std::optional<PythonBehaviorRef>
parse_python_behavior_ref(std::string_view behavior) noexcept {
    constexpr std::string_view kPrefix = "python:";

    if (!behavior.starts_with(kPrefix))
        return std::nullopt;

    auto rest = behavior.substr(kPrefix.size());
    if (rest.empty())
        return std::nullopt;

    // Find the single colon separating module and qualname.
    auto colon_pos = rest.find(':');
    if (colon_pos == std::string_view::npos || colon_pos == 0 ||
        colon_pos == rest.size() - 1) {
        return std::nullopt;
    }

    auto module = rest.substr(0, colon_pos);
    auto qualname = rest.substr(colon_pos + 1);

    // Reject if there's more than one colon after the prefix.
    if (qualname.find(':') != std::string_view::npos)
        return std::nullopt;

    // Check total behavior length bound.
    if (behavior.size() > 518)
        return std::nullopt;

    // Module and qualname each max 255 bytes.
    if (module.size() > 255 || qualname.size() > 255)
        return std::nullopt;

    // Validate segment grammar.
    if (!is_valid_dotted_name(module))
        return std::nullopt;
    if (!is_valid_dotted_name(qualname))
        return std::nullopt;

    PythonBehaviorRef ref;
    ref.module = std::string(module);
    ref.qualname = std::string(qualname);
    return ref;
}

// ── Actor argument validation ───────────────────────────────────────────────

result<void>
validate_python_actor_args(const config::ActorDef& def) noexcept {
    if (def.args.size() > 128) {
        return result<void>::make(
            error(errors::invalid_argument, "too many actor arguments"));
    }

    size_t total_bytes = 0;
    for (const auto& [key, value] : def.args) {
        // Key validation
        if (key.empty()) {
            return result<void>::make(
                error(errors::invalid_argument, "empty argument key"));
        }
        if (key.size() > 128) {
            return result<void>::make(
                error(errors::invalid_argument, "argument key too long"));
        }
        if (!is_valid_segment(std::string_view(key))) {
            return result<void>::make(
                error(errors::invalid_argument, "invalid argument key"));
        }
        if (key.starts_with("__hpactor_")) {
            return result<void>::make(
                error(errors::invalid_argument, "reserved argument key prefix"));
        }

        // Value validation
        if (value.size() > 4096) {
            return result<void>::make(
                error(errors::invalid_argument, "argument value too long"));
        }

        total_bytes += key.size() + value.size();
    }

    if (total_bytes > 64 * 1024) {
        return result<void>::make(
            error(errors::invalid_argument, "combined argument size exceeds 64 KiB"));
    }

    return result<void>::make();
}

// ── Argument fingerprint ────────────────────────────────────────────────────

uint64_t fingerprint_python_actor_args(const config::ActorDef& def) noexcept {
    // Collect sorted keys for deterministic ordering.
    std::vector<std::string_view> keys;
    keys.reserve(def.args.size());
    for (const auto& [k, _] : def.args) {
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());

    uint64_t seed = kFnvOffsetBasis;
    // Hash the argument count first for domain separation.
    seed = hash_u64(def.args.size(), seed);
    for (const auto& key : keys) {
        auto it = def.args.find(std::string(key));
        const auto& value = it->second;
        seed = hash_length_prefixed(key, seed);
        seed = hash_length_prefixed(value, seed);
    }
    return seed;
}

} // namespace hpactor::python
