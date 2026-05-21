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

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace hpactor {

/// \brief Shared failure carrier pushed to observability paths.
///
/// Built at the point where a failure is first detected (mailbox admission,
/// registry lookup, transport send). The envelope is forwarded to the metrics
/// ring buffer, structured log, trace span attributes, and — in a follow-on
/// phase — the dead-letter queue.
///
/// All members have default initializers; a default-constructed envelope is
/// safe and represents an unknown failure with zeroed metadata.
///
/// \note Thread safety: The struct is trivially copyable and safe to pass
///       between threads by value. Timestamp capture uses
///       \c std::chrono::steady_clock and is not synchronized.
struct FailureEnvelope {
    /// Canonical failure reason.
    FailureReason reason{FailureReason::Unknown};

    /// Target actor the failure relates to.
    ActorId actor_id{};

    /// Original sender address.
    ActorAddress sender{};

    /// Intended receiver address.
    ActorAddress receiver{};

    /// Message that failed (zeroed for non-message failures like spawn).
    MessageId message_id{};

    /// Distributed trace context at the failure point.
    TraceContext trace{};

    /// True if the caller can retry with a reasonable chance of success.
    bool retryable{false};

    /// Monotonic timestamp (nanoseconds) when the failure was recorded.
    uint64_t timestamp_ns{0};

    /// Which subsystem produced this failure.
    FailureSource source{FailureSource::ActorRuntime};

    /// Human-readable detail. Bounded to keep the envelope stack-friendly.
    std::array<char, 256> detail{};
    uint8_t detail_len{0};

    /// \brief Set the detail string, truncating to the array capacity.
    ///
    /// Maximum stored length is 255 bytes (array size minus 1). The detail
    /// buffer is always null-terminated at position \c detail_len, so
    /// \c detail.data() is safe to use as a C string.
    ///
    /// \param[in] s The detail string to store. May be empty.
    void set_detail(std::string_view s) noexcept {
        detail_len = static_cast<uint8_t>(std::min(s.size(), detail.size() - 1));
        std::memcpy(detail.data(), s.data(), detail_len);
        detail[detail_len] = '\0';
    }

    /// \brief View of the detail string.
    ///
    /// \return A \c std::string_view over the stored detail, bounded by
    ///         \c detail_len. Empty if no detail was set.
    [[nodiscard]] std::string_view detail_view() const noexcept {
        return {detail.data(), detail_len};
    }
};

/// \brief Build a fully populated FailureEnvelope with a monotonic timestamp.
///
/// This is the canonical factory for building failure envelopes at the
/// delivery boundary. The \c retryable flag is derived from \c reason via
/// \c retryable(reason). \c timestamp_ns is sampled from
/// \c std::chrono::steady_clock at call time.
///
/// \param[in] reason Canonical failure reason.
/// \param[in] actor_id Target actor the failure relates to.
/// \param[in] sender Original sender address.
/// \param[in] receiver Intended receiver address.
/// \param[in] message_id Message that failed (zeroed for non-message
///                      failures like spawn).
/// \param[in] trace Distributed trace context at the failure point.
/// \param[in] source Which subsystem produced this failure.
/// \param[in] detail Human-readable detail. Bounded to 255 bytes; may be
///                   empty.
/// \return A stack-constructed FailureEnvelope with all fields populated
///         and a monotonic timestamp.
/// \note Thread safety: Callable from any thread. Timestamp capture is
///       monotonic but not synchronized across threads.
FailureEnvelope
make_failure_envelope(FailureReason reason, ActorId actor_id,
                      const ActorAddress& sender, const ActorAddress& receiver,
                      MessageId message_id, const TraceContext& trace,
                      FailureSource source, std::string_view detail = {}) noexcept;

} // namespace hpactor
