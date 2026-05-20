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
#include <functional>

namespace hpactor {

template <typename Tag, typename T = uint64_t> class Id {
    T value_{};

  public:
    constexpr Id() = default;
    explicit constexpr Id(T v) : value_{v} {}

    [[nodiscard]] constexpr T value() const noexcept {
        return value_;
    }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return value_ != T{};
    }

    friend constexpr bool operator==(Id, Id) = default;
    friend constexpr bool operator!=(Id, Id) = default;
    friend constexpr auto operator<=>(Id, Id) = default;
};

} // namespace hpactor

template <typename Tag, typename T> struct std::hash<hpactor::Id<Tag, T>> {
    std::size_t operator()(hpactor::Id<Tag, T> id) const noexcept {
        return std::hash<T>{}(id.value());
    }
};
