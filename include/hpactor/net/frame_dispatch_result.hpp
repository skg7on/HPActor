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

#include <hpactor/msg/frame.hpp>

namespace hpactor::net {

// ── Inbound frame dispatch result ────────────────────────────────────────────

/// \brief Outcome produced by \c InboundFrameRouter for each envelope.
///
/// Fixed-size so the router never allocates on the classification path.
enum class FrameDispatchCode : uint8_t {
    ActorDelivered = 0,
    ActorRejected,
    BatchDelivered,
    BatchPartiallyDelivered,
    RpcResponseHandled,
    ReliableAckHandled,
    ReliableNackHandled,
    BackpressureHandled,
    StreamHandled,
    DecodeFailed,
    UnsupportedPayload,
    InvalidFlags,
    InvalidAddress,
    InvalidControlPayload,
    InvalidTraceContext,
    UnknownStream,
    DuplicateStream,
    StreamCapacityExceeded,
    HandlerUnavailable,
    RuntimeStopping,
    HandshakeRejected, ///< Connection handshake was rejected (version mismatch,
                       ///< incompatible flags, or auth failure).
};

/// \brief Fixed-size diagnostic returned by the inbound frame router.
///
/// Contains no payload, dynamic string, actor pointer, or unbounded
/// error detail so it is safe for allocation-free hot-path use.
struct FrameDispatchResult {
    FrameDispatchCode code{FrameDispatchCode::ActorDelivered};
    WireFrame::PayloadType payload_type{WireFrame::PayloadType::Unknown};
    uint32_t detail_code{0};
    uint32_t accepted_count{0};
    uint32_t rejected_count{0};
    uint32_t invalid_count{0};
};

} // namespace hpactor::net
