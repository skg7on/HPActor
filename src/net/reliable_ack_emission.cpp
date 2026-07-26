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

#include <hpactor/frame.pb.h>

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

    // Map EnqueueResultCode to NackReason via AckStatus.
    switch (result.code) {
        case mailbox::EnqueueResultCode::Rejected:
            decision.status = static_cast<AckStatus>(NackReason::NACK_MAILBOX_FULL);
            decision.retry_after_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(result.retry_after)
                    .count());
            break;
        case mailbox::EnqueueResultCode::ActorNotFound:
        case mailbox::EnqueueResultCode::MailboxClosed:
            decision.status = static_cast<AckStatus>(NackReason::NACK_ACTOR_DEAD);
            break;
        default:
            decision.status =
                static_cast<AckStatus>(NackReason::NACK_REJECTED_BY_POLICY);
            break;
    }

    return decision;
}

} // namespace hpactor::net
