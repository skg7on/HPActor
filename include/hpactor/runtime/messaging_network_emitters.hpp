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

#include <hpactor/runtime/network_dispatch_targets.hpp>

#include <type_traits>

namespace hpactor {

/// \brief Non-owning, allocation-free emitter for reliable ACK/NACK
///        frames.
///
/// Uses a typed \c ReliableAckTarget pointer instead of \c void* context
/// plus function pointer.  Same single-indirection cost, compile-time
/// type safety.
///
/// A null target is a safe no-op (the emitter reports unavailability
/// and returns).
class ReliableAckEmitter final {
  public:
    /// \brief Invoke the emitter.  Safe no-op when unavailable.
    void operator()(const ActorAddress& target, const ActorAddress& acker,
                    uint64_t message_id, uint8_t status,
                    uint32_t retry_after_ms) const noexcept {
        if (target_) {
            target_->send_ack(target, acker, message_id, status, retry_after_ms);
        }
    }

    /// \brief Typed target, or \c nullptr when networking is unavailable.
    ReliableAckTarget* target_{nullptr};
};

static_assert(std::is_trivially_copyable_v<ReliableAckEmitter>,
              "ReliableAckEmitter must be allocation-free");

/// \brief Non-owning, allocation-free emitter for remote backpressure
///        signals.
///
/// Same design as \c ReliableAckEmitter: typed \c BackpressureSignalTarget
/// pointer.
class BackpressureSignalEmitter final {
  public:
    /// \brief Invoke the emitter.  Returns \c false when unavailable.
    bool operator()(const ActorAddress& target,
                    const StreamBuffer& encoded) const noexcept {
        if (target_) {
            return target_->send_signal(target, encoded);
        }
        return false;
    }

    /// \brief Typed target, or \c nullptr when networking is unavailable.
    BackpressureSignalTarget* target_{nullptr};
};

static_assert(std::is_trivially_copyable_v<BackpressureSignalEmitter>,
              "BackpressureSignalEmitter must be allocation-free");

/// \brief Aggregate of network-control emitters used by messaging.
///
/// These emitters cross the \c NetworkRuntime boundary without
/// capturing \c ActorSystem or creating a circular dependency.
/// Individual emitters may be null/unavailable when networking is
/// disabled.
struct MessagingNetworkEmitters final {
    /// \brief Reliable ACK/NACK emission emitter.
    ReliableAckEmitter reliable_ack;

    /// \brief Remote backpressure signal emitter.
    BackpressureSignalEmitter backpressure;
};

static_assert(std::is_trivially_copyable_v<MessagingNetworkEmitters>,
              "MessagingNetworkEmitters must be allocation-free");

} // namespace hpactor
