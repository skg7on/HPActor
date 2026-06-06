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
#include <hpactor/msg/enqueue_result.hpp>

#include <optional>

namespace hpactor::mailbox {

/// \brief Decoded result of deserializing a backpressure signal from the wire.
struct DecodedBackpressureSignal {
    /// The backpressure signal carrying target, depth, capacity, and pressure
    /// ratio.
    BackpressureSignal signal;
    /// Pressure state at the time of serialization.
    MailboxPressureState state = MailboxPressureState::Normal;
};

/// \brief Serialize a backpressure signal for remote transmission.
///
/// Encodes the signal and pressure state into a compact binary buffer suitable
/// for attachment to transport frames.
///
/// \param[in] signal The backpressure signal to serialize.
/// \param[in] state The pressure state at serialization time.
/// \return A \c StreamBuffer owning the serialized payload.
/// \note Thread safety: safe to call from any thread. Does not access shared
///       state.
[[nodiscard]] StreamBuffer
serialize_backpressure_signal(const BackpressureSignal& signal,
                              MailboxPressureState state);

/// \brief Deserialize a backpressure signal received from a remote node.
///
/// Decodes the wire format produced by \c serialize_backpressure_signal().
///
/// \param[in] payload The serialized payload received over the transport.
/// \return A \c DecodedBackpressureSignal on success, or \c std::nullopt if
///         the payload is malformed or truncated.
/// \note Thread safety: safe to call from any thread. Does not access shared
///       state.
[[nodiscard]] std::optional<DecodedBackpressureSignal>
deserialize_backpressure_signal(const StreamBuffer& payload);

} // namespace hpactor::mailbox
