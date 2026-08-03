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

#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/net/reliable_ack.hpp>

namespace hpactor::net {

/// Result of computing whether an ACK or NACK should be emitted after
/// an inbound frame with AckRequested flag is processed.
struct AckEmitDecision {
    /// Whether an ACK/NACK should be emitted.
    bool should_emit = false;

    /// The ACK/NACK status to emit.
    /// AckStatus::Accepted (0) = ACK.
    /// AckStatus::Rejected (1) = NACK (all NACK reasons).
    /// AckStatus::Duplicate (2) = duplicate suppressed (treated as ACK).
    AckStatus status = AckStatus::Accepted;

    /// Suggested retry delay in milliseconds (NACK only, 0 = sender decides).
    uint32_t retry_after_ms = 0;
};

/// Compute what ACK/NACK (if any) to emit after processing an inbound frame.
///
/// \param[in] result        The enqueue result from DeliveryPipeline.
/// \param[in] ack_requested Whether the sender set the AckRequested flag.
/// \return An \c AckEmitDecision describing what to emit.
AckEmitDecision compute_ack_emission(const mailbox::EnqueueResult& result,
                                     bool ack_requested) noexcept;

} // namespace hpactor::net
