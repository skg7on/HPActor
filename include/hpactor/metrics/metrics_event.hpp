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
#include <hpactor/types/types.hpp>

namespace hpactor::metrics {

/// \brief Event types emitted into the metrics ring buffer.
enum class MetricEventType : uint8_t {
    kMailboxEnqueue = 0,     ///< Message enqueued into an actor mailbox.
    kMailboxDequeue = 1,     ///< Message dequeued from an actor mailbox.
    kMessageProcessed = 2,   ///< Actor finished processing a message.
    kActorSpawned = 3,       ///< A new actor was spawned.
    kActorTerminated = 4,    ///< An actor terminated.
    kSchedulerDispatch = 5,  ///< A worker thread dispatched an actor.
    kSchedulerSteal = 6,     ///< A worker thread stole work from another queue.
    kSupervisorRestart = 7,  ///< A supervisor restarted a child actor.
    kMemoryAlloc = 8,        ///< Memory allocated through the allocator.
    kMemoryFree = 9,         ///< Memory freed through the allocator.
    kMailboxRejected = 10,   ///< Message rejected by mailbox admission.
    kMailboxDropped = 11,    ///< Message dropped by overflow policy.
    kMailboxDeadLetter = 12, ///< Message routed to the dead-letter queue.
    kBackpressureSignal = 13,   ///< Backpressure signal emitted.
    kDeadLetterLost = 14,       ///< Dead-letter record evicted.
    kLifecycleTransition = 15,  ///< Actor lifecycle state changed.
    kMessageRejected = 16,      ///< Message rejected at the receiver.
    kActorDrainStart = 17,      ///< Actor drain phase started.
    kActorDrainComplete = 18,   ///< Actor drain phase completed.
    kActorDrainTimeout = 19,    ///< Actor drain phase timed out.
    kDeliveryFailure = 20,      ///< Delivery failure — \c code carries
                                ///< FailureReason.
    kDeliveryDuplicate = 21,    ///< Duplicate message suppressed at receiver.
    kDeliveryExpired = 22,      ///< Message expired before handler execution.
    kActorQuarantined = 23,     ///< Actor transitioned to kQuarantined.
    kActorUnquarantined = 24,   ///< Actor released from quarantine.
    kCircuitStateChange = 25,   ///< Circuit breaker state changed.
    kFaultInjected = 26,        ///< Fault injection fired.
    kEndpointSendAccepted = 27, ///< Message accepted into endpoint outbound
                                ///< queue.
    kEndpointSendRejected = 28, ///< Message rejected by endpoint outbound
                                ///< queue.
    kEndpointOutboundMessages = 29, ///< Endpoint outbound queue message count.
    kEndpointOutboundBytes = 30,    ///< Endpoint outbound queue byte count.
    kEndpointPressureState = 31,    ///< Endpoint outbound queue pressure state.
    kEndpointCircuitState = 32,     ///< Endpoint circuit breaker state.
    kEndpointBackpressureSignal = 33, ///< Endpoint backpressure signal sent.
    kEndpointCircuitTransition = 34,  ///< Endpoint circuit breaker transition.
    kRateLimitBlocked = 35,   ///< Message deferred by actor rate limiter.
    kAdmissionRejected = 36,  ///< Message rejected by admission policy.
    kAdmissionDLQRouted = 37, ///< Message rerouted to DLQ by admission policy.
    kPerSenderBucketCount = 38, ///< Number of active per-sender buckets.
};

/// \brief A single metric event in the lock-free ring buffer.
///
/// 32-byte aligned struct for cache-line-friendly atomic operations.
/// Written by producers throughout the actor system and drained by
/// the MetricsActor for aggregation.
struct alignas(32) MetricEvent {
    /// \brief Monotonic timestamp in nanoseconds.
    uint64_t timestamp_ns;
    /// \brief Actor this event pertains to.
    ActorId actor_id;
    /// \brief Event classification.
    MetricEventType event_type;
    /// \brief Event-specific code (rejection reason, drop reason, policy code).
    uint8_t code;
    /// \brief Auxiliary data (event-specific semantics).
    uint8_t aux;
    uint8_t _pad[1];
    /// \brief High 32 bits of an event-specific 64-bit value (e.g. byte count).
    uint32_t value_hi;
};

static_assert(sizeof(MetricEvent) == 32, "MetricEvent must be 32 bytes");

} // namespace hpactor::metrics
