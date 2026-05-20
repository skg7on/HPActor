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

#include <array>
#include <cstdint>
#include <cstring>

namespace hpactor {

/// Shared failure carrier. Built at the point where a failure is first
/// detected and pushed to observability paths (log, trace, metric, DLQ).
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

    /// Set the detail string, truncating to the array capacity.
    /// Safe: max stored length is 255 (array size minus 1).
    void set_detail(std::string_view s) noexcept {
        detail_len = static_cast<uint8_t>(std::min(s.size(), detail.size() - 1));
        std::memcpy(detail.data(), s.data(), detail_len);
    }

    /// View of the detail string.
    [[nodiscard]] std::string_view detail_view() const noexcept {
        return {detail.data(), detail_len};
    }
};

/// Factory: fill an envelope with the fields available at the delivery
/// boundary. Timestamp is sampled from the caller's clock.
FailureEnvelope
make_failure_envelope(FailureReason reason, ActorId actor_id,
                      const ActorAddress& sender, const ActorAddress& receiver,
                      MessageId message_id, const TraceContext& trace,
                      FailureSource source, std::string_view detail = {}) noexcept;

} // namespace hpactor
