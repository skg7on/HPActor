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

#include <hpactor/net/reliable_ack_emission.hpp>

namespace hpactor::net {

AckEmitDecision compute_ack_emission(const mailbox::EnqueueResult& result,
                                     bool ack_requested) noexcept {
    AckEmitDecision decision;

    if (!ack_requested) {
        return decision; // should_emit remains false
    }

    decision.should_emit = true;

    if (result.accepted()) {
        decision.status = AckStatus::Accepted;
        return decision;
    }

    // Map EnqueueResultCode to AckStatus.
    // send_ack() uses status == 1 (AckStatus::Rejected) to distinguish
    // NACK from ACK on the wire. All NACK reasons map to AckStatus::Rejected.
    // The retry_after_ms field distinguishes retryable (MailboxFull, >0)
    // from non-retryable (ActorDead, RejectedByPolicy, 0).
    decision.status = AckStatus::Rejected;

    switch (result.code) {
        case mailbox::EnqueueResultCode::Rejected:
            // Retryable: mailbox full — sender should back off and retry.
            decision.retry_after_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(result.retry_after)
                    .count());
            break;
        default:
            // Non-retryable: actor dead, mailbox closed, policy rejection,
            // or otherwise undeliverable — sender should route to DLQ.
            decision.retry_after_ms = 0;
            break;
    }

    return decision;
}

} // namespace hpactor::net
