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

enum class MetricEventType : uint8_t {
    kMailboxEnqueue = 0,
    kMailboxDequeue = 1,
    kMessageProcessed = 2,
    kActorSpawned = 3,
    kActorTerminated = 4,
    kSchedulerDispatch = 5,
    kSchedulerSteal = 6,
    kSupervisorRestart = 7,
    kMemoryAlloc = 8,
    kMemoryFree = 9,
    kMailboxRejected = 10,
    kMailboxDropped = 11,
    kMailboxDeadLetter = 12,
    kBackpressureSignal = 13,
    kDeadLetterLost = 14,
    kLifecycleTransition = 15,
    kMessageRejected = 16,
    kActorDrainStart = 17,
    kActorDrainComplete = 18,
    kActorDrainTimeout = 19,
    kDeliveryFailure = 20,    ///< Delivery failure event — \c code carries
                              ///< FailureReason.
    kDeliveryDuplicate = 21,  ///< Duplicate message suppressed at receiver.
    kDeliveryExpired = 22,    ///< Message expired before handler execution.
    kActorQuarantined = 23,   ///< Actor transitioned to kQuarantined.
    kActorUnquarantined = 24, ///< Actor released from quarantine.
    kCircuitStateChange = 25, ///< Circuit breaker state changed.
};

struct alignas(32) MetricEvent {
    uint64_t timestamp_ns;
    ActorId actor_id;
    MetricEventType event_type;
    uint8_t code; // rejection reason, drop reason, policy code
    uint8_t aux;  // auxiliary data
    uint8_t _pad[1];
    uint32_t value_hi;
};

static_assert(sizeof(MetricEvent) == 32, "MetricEvent must be 32 bytes");

} // namespace hpactor::metrics
