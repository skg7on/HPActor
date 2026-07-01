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

#include <hpactor/mailbox/fixed_message_envelope.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <memory>

namespace hpactor {

class EventBasedActor;
class TypedMessage;

namespace cli {
struct MboxSnapshot;
}

namespace sched {
struct ActorExecutionContext;
}

namespace mailbox {

// ── Delivery interface ────────────────────────────────────────────────────

struct FixedDeliveryObservation {
    ActorId actor;
    uint64_t message_id;
    uint64_t enqueue_sequence;
    uint32_t payload_bytes;
    uint64_t deadline_ns;
};

struct FixedDeliveryFailure {
    ActorId actor;
    ActorAddress sender;
    FailureReason reason;
    uint64_t message_id;
    uint64_t deadline_ns;
    uint32_t payload_bytes;
};

struct DeliveryPreflightResult {
    bool accepted{true};
    FailureReason reason{FailureReason::Unknown};
};

/// Non-owning function-pointer interface for fixed-message delivery
/// preflight checks and outcome recording.  No \c std::function on
/// production paths.
struct FixedMailboxDelivery {
    void* context{nullptr};

    DeliveryPreflightResult (*preflight)(void*, const ActorAddress&,
                                         const FixedEnvelopeMeta&) noexcept{
        nullptr};

    void (*record_accepted)(void*, ActorId,
                            const FixedDeliveryObservation&) noexcept{nullptr};

    void (*record_rejected)(void*, ActorId,
                            const FixedDeliveryFailure&) noexcept{nullptr};
};

// ── Control ingress ───────────────────────────────────────────────────────

/// Interface for routing system-level TypedMessage traffic into a
/// fixed actor's protected system lane.
struct FixedMailboxControl {
    void* context{nullptr};

    EnqueueResult (*try_push)(void*, TypedMessage&&) noexcept{nullptr};
};

// ── Execution interface ───────────────────────────────────────────────────

/// Interface consumed by the scheduler's ActorExecutionEngine to
/// activate a fixed-mailbox actor.
struct FixedMailboxExecution {
    void* context{nullptr};

    bool (*consume_one)(void*, EventBasedActor&,
                        const sched::ActorExecutionContext&) noexcept{nullptr};

    bool (*empty)(const void*) noexcept{nullptr};

    cli::MboxSnapshot (*snapshot_fn)(const void*) noexcept{nullptr};
};

// ── Lifecycle interface ───────────────────────────────────────────────────

/// Interface for actor lifecycle transitions (drain, close).
struct FixedMailboxLifecycle {
    void* context{nullptr};

    void (*begin_drain)(void*) noexcept{nullptr};

    void (*drain_immediate)(void*, EventBasedActor&) noexcept{nullptr};

    void (*close)(void*) noexcept{nullptr};

    bool (*publishers_quiescent)(const void*) noexcept{nullptr};
};

// ── Handle ────────────────────────────────────────────────────────────────

/// Complete handle to a fixed-mailbox actor's external interfaces.
/// Stored in \c ActorDirectoryEntry alongside the variable mailbox.
struct FixedMailboxHandle {
    /// Retains the shared \c FixedActorMailboxCore so it outlives
    /// the actor and can reject surviving-reference sends.
    std::shared_ptr<void> lifetime;

    FixedMailboxDelivery delivery;
    FixedMailboxControl control;
    FixedMailboxExecution execution;
    FixedMailboxLifecycle lifecycle;

    /// True when all required interfaces are non-null.
    [[nodiscard]] bool valid() const noexcept {
        return lifetime != nullptr && control.try_push != nullptr &&
               execution.consume_one != nullptr && lifecycle.close != nullptr;
    }
};

} // namespace mailbox
} // namespace hpactor
