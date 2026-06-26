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

#include <chrono>
#include <cstdint>

namespace hpactor {

/// \brief Duration type for timeouts and scheduling.
///
/// Wraps std::chrono::milliseconds for use in actor framework APIs.
class Duration {
  public:
    constexpr Duration() = default;

    explicit constexpr Duration(std::chrono::milliseconds ms) : ms_(ms) {}

    /// \brief Create a Duration from a number of seconds.
    static constexpr Duration from_seconds(uint32_t seconds) {
        return Duration(std::chrono::seconds(seconds));
    }

    /// \brief Create a Duration from a number of milliseconds.
    static constexpr Duration from_milliseconds(uint32_t ms) {
        return Duration(std::chrono::milliseconds(ms));
    }

    /// \brief Underlying chrono duration.
    [[nodiscard]] constexpr std::chrono::milliseconds as_chrono() const noexcept {
        return ms_;
    }

    /// \brief Duration value in milliseconds.
    [[nodiscard]] constexpr int64_t count() const noexcept {
        return ms_.count();
    }

    friend constexpr bool operator==(Duration, Duration) = default;
    friend constexpr bool operator!=(Duration, Duration) = default;
    friend constexpr auto operator<=>(Duration, Duration) = default;

  private:
    std::chrono::milliseconds ms_{0};
};

} // namespace hpactor
