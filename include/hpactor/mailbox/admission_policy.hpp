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

#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/failure_reason.hpp>

#include <cstdint>

namespace hpactor::mailbox {

/// \brief Decision returned by an admission policy evaluation.
///
/// Governs whether the message is accepted into the mailbox, rejected outright,
/// or rerouted to the dead-letter queue.
enum class AdmissionDecision : uint8_t {
    Accept, ///< Message is admitted — proceed to lane routing.
    Reject, ///< Message is rejected — returned to producer with failure info.
    RerouteToDLQ, ///< Message is rerouted to the dead-letter queue.
};

/// \brief Result of a single admission policy evaluation.
///
/// Returned by \c IAdmissionPolicy::evaluate(). Carries the decision plus
/// diagnostic metadata for metrics and CLI observability.
struct AdmissionPolicyResult {
    /// The admission decision for this message.
    AdmissionDecision decision{AdmissionDecision::Accept};
    /// Canonical failure reason when rejected or rerouted.
    FailureReason reason{FailureReason::RejectedByPolicy};
    /// Human-readable policy name for diagnostics (not owned).
    const char* policy_name{nullptr};
    /// Policy-specific match code for detailed rejection classification.
    uint32_t match_code{0};
};

/// \brief Interface for admission policies evaluated during \c try_push().
///
/// An admission policy is evaluated before lane routing in the producer path.
/// The first non-\c Accept decision short-circuits the chain and the message
/// is rejected. Policies are evaluated in insertion order.
///
/// \note Thread safety: \c evaluate() is called from arbitrary producer
///       threads. Implementations must be safe to call concurrently or
///       provide their own internal synchronization.
class IAdmissionPolicy {
  public:
    virtual ~IAdmissionPolicy() = default;

    /// \brief Evaluate whether a message should be admitted.
    ///
    /// \param[in] msg The message being admitted.
    /// \param[in] meta Envelope metadata (sender, priority, deadline, etc.).
    /// \param[in] config Current mailbox configuration.
    /// \param[in] mailbox_depth Current total mailbox depth at evaluation time.
    /// \return An \c AdmissionPolicyResult with the admission decision.
    /// \note Thread safety: must be safe to call from any producer thread.
    virtual AdmissionPolicyResult
    evaluate(const TypedMessage& msg, const MailboxEnvelopeMeta& meta,
             const MailboxConfig& config, uint64_t mailbox_depth) noexcept = 0;

    /// \brief Human-readable policy name for diagnostics and CLI.
    ///
    /// \return A null-terminated string literal. The caller must not free it.
    virtual const char* name() const noexcept = 0;
};

} // namespace hpactor::mailbox
