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

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace hpactor::receptionist {

/// Runtime service key — name + expected message TypeTag.
/// Equality and hashing are by name only; the TypeTag is advisory.
struct ServiceKey {
    std::string name;
    uint32_t type_tag{0};

    bool operator==(const ServiceKey& o) const {
        return name == o.name;
    }
    bool operator!=(const ServiceKey& o) const {
        return name != o.name;
    }
};

/// Typed convenience factory — captures the expected message TypeTag
/// at compile time for documentation.
template <typename T> ServiceKey service_key(std::string_view name) {
    return ServiceKey{std::string(name), T::kTypeTag};
}

} // namespace hpactor::receptionist

namespace std {
template <> struct hash<hpactor::receptionist::ServiceKey> {
    size_t operator()(const hpactor::receptionist::ServiceKey& k) const {
        return hash<string>{}(k.name);
    }
};
} // namespace std
