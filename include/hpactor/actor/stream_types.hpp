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
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <string>

namespace hpactor::stream {

/// \brief Stream data chunk delivered to the receiver actor.
///
/// The payload carries the original chunk data. The message's \c sender_address
/// identifies the stream sender. The stream's trace context is attached.
inline constexpr TypeTag StreamChunkTag = make_subsystem_tag(0x80);

/// \brief Stream session has been opened.
///
/// Delivered before the first chunk. Payload: \c StreamOpenedPayload
/// (stream_id, sender address, config summary).
inline constexpr TypeTag StreamOpenedTag = make_subsystem_tag(0x81);

/// \brief Stream has been gracefully closed by the sender.
///
/// Payload: \c StreamClosedPayload (stream_id, close reason, total bytes).
inline constexpr TypeTag StreamClosedTag = make_subsystem_tag(0x82);

/// \brief Stream has errored.
///
/// Payload: \c StreamErrorPayload (stream_id, error code, description).
inline constexpr TypeTag StreamErrorTag = make_subsystem_tag(0x83);

/// \brief Payload delivered to receiver when a stream session is established.
struct StreamOpenedPayload {
    uint64_t stream_id;
    ActorAddress sender;
    uint32_t initial_window_bytes;
};

/// \brief Payload delivered to receiver when a stream is gracefully closed.
struct StreamClosedPayload {
    uint64_t stream_id;
    uint32_t reason; ///< 0=COMPLETE, 1=CANCELLED, 2=TIMEOUT
    uint64_t total_bytes;
};

/// \brief Payload delivered to receiver when a stream errors.
struct StreamErrorPayload {
    uint64_t stream_id;
    uint32_t error_code;
    std::string description;
};

// ── Wire-protocol TypeTags (internal, used between StreamSender/Receiver) ────

/// \brief Wire frame: StreamDataFrame (sender → receiver data chunk).
inline constexpr TypeTag StreamDataTag = make_subsystem_tag(0x84);

/// \brief Wire frame: StreamAckFrame (receiver → sender credit ack).
inline constexpr TypeTag StreamAckTag = make_subsystem_tag(0x85);

/// \brief Wire frame: StreamCloseFrame (graceful close).
inline constexpr TypeTag StreamCloseTag = make_subsystem_tag(0x86);

/// \brief Wire frame: StreamErrorFrame (stream error).
/// \note Distinct from the user-facing \c StreamErrorTag (0x83),
///       which delivers a \c StreamErrorPayload to the target actor.
inline constexpr TypeTag StreamWireErrorTag = make_subsystem_tag(0x87);

// ── Internal control tags (StreamHandle → StreamSenderActor) ────────────

/// \brief Internal: StreamHandle requests graceful stream close.
/// Payload: empty. StreamSenderActor sends StreamCloseFrame to the receiver.
inline constexpr TypeTag InternalCloseTag = make_subsystem_tag(0x88);

/// \brief Internal: StreamHandle requests stream abort with error.
/// Payload: 4-byte LE error_code + optional description string.
/// StreamSenderActor sends StreamErrorFrame to the receiver.
inline constexpr TypeTag InternalErrorTag = make_subsystem_tag(0x89);

/// \brief Internal: idle timeout fired.
/// Payload: empty. StreamSenderActor sends StreamErrorFrame to the receiver.
inline constexpr TypeTag InternalTimeoutTag = make_subsystem_tag(0x8A);

} // namespace hpactor::stream
