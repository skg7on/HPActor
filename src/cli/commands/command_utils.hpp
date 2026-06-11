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

#include <hpactor/types/types.hpp>

#include <charconv>
#include <string>

namespace hpactor {
namespace cli {

/// \brief Format a byte count as a human-readable string (e.g. "1.2 MB").
std::string format_bytes(uint64_t bytes);

inline ActorId parse_actor_id(const std::string& s) {
    uint64_t raw = 0;
    int base = 10;
    const char* start = s.data();
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        start = s.data() + 2;
    }
    auto [ptr, ec] = std::from_chars(start, s.data() + s.size(), raw, base);
    if (ec != std::errc{})
        return ActorId{0};
    return ActorId{raw};
}

} // namespace cli
} // namespace hpactor