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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <type_traits>

namespace hpactor {

/// \brief Non-owning, allocation-free emitter for reliable ACK/NACK
///        frames.
///
/// Uses a function pointer plus opaque \c void* context instead of
/// \c std::function to avoid heap allocation and facade capture.
///
/// A null context or function pointer is a safe no-op (the emitter
/// reports unavailability and returns).
class ReliableAckEmitter final {
  public:
    /// \brief Signature for a reliable ACK/NACK emitter.
    ///
    /// \param context       Opaque pointer to stable network state.
    /// \param target        Destination address for the ACK/NACK frame.
    /// \param acker         Local endpoint that generated the ACK.
    /// \param message_id    Message being acknowledged.
    /// \param status        ACK status: 0=Accepted, 1=Rejected, 2=Duplicate.
    /// \param retry_after_ms Suggested retry delay for NACK (0 if unused).
    using EmitFn = void (*)(void* context, const ActorAddress& target,
                            const ActorAddress& acker, uint64_t message_id,
                            uint8_t status, uint32_t retry_after_ms) noexcept;

    /// \brief Invoke the emitter.  Safe no-op when unavailable.
    ///
    /// \param target        Destination address.
    /// \param acker         ACK emitter identity.
    /// \param message_id    Message to acknowledge.
    /// \param status        ACK status code.
    /// \param retry_after_ms Suggested retry delay.
    void operator()(const ActorAddress& target, const ActorAddress& acker,
                    uint64_t message_id, uint8_t status,
                    uint32_t retry_after_ms) const noexcept {
        if (context && emit) {
            emit(context, target, acker, message_id, status, retry_after_ms);
        }
    }

    /// \brief Opaque context passed to \c emit (stable network state).
    void* context{nullptr};

    /// \brief Emit function, or \c nullptr when networking is unavailable.
    EmitFn emit{nullptr};
};

static_assert(std::is_trivially_copyable_v<ReliableAckEmitter>,
              "ReliableAckEmitter must be allocation-free");

/// \brief Non-owning, allocation-free emitter for remote backpressure
///        signals.
///
/// Same design as \c ReliableAckEmitter: function pointer + opaque context.
class BackpressureSignalEmitter final {
  public:
    /// \brief Signature for a remote backpressure signal sender.
    ///
    /// \param context Opaque pointer to stable network state.
    /// \param target  Destination address.
    /// \param encoded Pre-encoded backpressure frame bytes.
    /// \return \c true if the frame was sent (or queued), \c false if
    ///         unavailable.
    using SendFn = bool (*)(void* context, const ActorAddress& target,
                            const StreamBuffer& encoded) noexcept;

    /// \brief Invoke the emitter.  Returns \c false when unavailable.
    ///
    /// \param target  Destination address.
    /// \param encoded Pre-encoded frame bytes.
    /// \return \c true if sent, \c false if unavailable.
    bool operator()(const ActorAddress& target,
                    const StreamBuffer& encoded) const noexcept {
        if (context && send) {
            return send(context, target, encoded);
        }
        return false;
    }

    /// \brief Opaque context passed to \c send (stable network state).
    void* context{nullptr};

    /// \brief Send function, or \c nullptr when networking is unavailable.
    SendFn send{nullptr};
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
