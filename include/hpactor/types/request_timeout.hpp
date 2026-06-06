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

/// \brief Per-request timeout specification.
///
/// Represents either a relative duration or an absolute deadline.
/// A zero-value duration means "use the system default."
///
/// \note Thread safety: Immutable value type, safe to copy across threads.
struct RequestTimeout {
    enum class Kind : uint8_t { Duration, Deadline };

    Kind kind = Kind::Duration;
    std::chrono::milliseconds value{0};

    /// \brief Create a relative-duration timeout.
    ///
    /// \param[in] ms Timeout in milliseconds. 0 means "use default."
    /// \return RequestTimeout with kind=Duration.
    static RequestTimeout from_ms(uint64_t ms) noexcept {
        return {Kind::Duration, std::chrono::milliseconds{ms}};
    }

    /// \brief Create an absolute-deadline timeout.
    ///
    /// \param[in] tp Absolute deadline in steady_clock time.
    /// \return RequestTimeout with kind=Deadline.
    static RequestTimeout
    from_deadline(std::chrono::steady_clock::time_point tp) noexcept {
        auto d = std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch());
        return {Kind::Deadline, d};
    }

    /// \brief Sentinel for "use system default."
    ///
    /// \return RequestTimeout with kind=Duration and value=0ms.
    static RequestTimeout use_default() noexcept {
        return {};
    }

    /// \brief Compute the point in time when this request expires.
    ///
    /// For Duration kind, returns now + value. For Deadline kind,
    /// returns the stored absolute point. For default (zero-duration),
    /// returns time_point::max().
    ///
    /// \return Absolute deadline in steady_clock time.
    std::chrono::steady_clock::time_point deadline() const noexcept {
        if (is_default()) {
            return std::chrono::steady_clock::time_point::max();
        }
        if (kind == Kind::Deadline) {
            return std::chrono::steady_clock::time_point(value);
        }
        return std::chrono::steady_clock::now() + value;
    }

    /// \brief Whether this is using the system default (value == 0ms).
    bool is_default() const noexcept {
        return kind == Kind::Duration && value.count() == 0;
    }
};

} // namespace hpactor
