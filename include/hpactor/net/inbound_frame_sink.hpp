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

#include <hpactor/net/frame_dispatch_result.hpp>
#include <hpactor/ref/actor_address.hpp>

namespace hpactor::net {

struct WireFrame; // forward decl — full def in <hpactor/msg/frame.hpp>

// ── Inbound frame context ────────────────────────────────────────────────────

/// \brief Metadata for an inbound frame delivered to the router.
///
/// Carries peer identity and encoded byte count so the router can make
/// security, capacity, and peer-identification decisions without reading
/// the connection object.
struct InboundFrameContext {
    /// Remote endpoint that sent this frame.
    EndPoint peer;
    /// Total encoded bytes received (header + payload) for this frame.
    uint32_t encoded_bytes{0};
};

// ── Fixed-function sink ──────────────────────────────────────────────────────

/// \brief Allocation-free inbound frame delivery sink.
///
/// Installed into \c ConnectionPool before ingress begins. When non-null,
/// every valid frame and decode failure is routed exclusively through this
/// sink; legacy per-category handlers receive zero calls.
///
/// The sink stores a raw context pointer and two function pointers — no
/// virtual dispatch, no allocation, no facade capture. The context must
/// outlive all callback invocations (typically the router object itself).
struct InboundFrameSink {
    /// Opaque context pointer — typically the router itself.
    void* context{nullptr};

    /// \brief Route a successfully decoded frame.
    ///
    /// \param ctx \c context pointer.
    /// \param ictx Frame metadata (peer, encoded size).
    /// \param frame Valid decoded wire frame.
    /// \return Fixed-size diagnostic result.
    FrameDispatchResult (*route)(void* ctx, const InboundFrameContext& ictx,
                                 const WireFrame& frame) noexcept {nullptr};

    /// \brief Report a decode failure.
    ///
    /// \param ctx \c context pointer.
    /// \param ictx Frame metadata (peer, observed bytes).
    /// \param error Decode error reason.
    /// \return Fixed-size diagnostic result.
    FrameDispatchResult (*decode_failed)(void* ctx, const InboundFrameContext& ictx,
                                         FrameDecodeError error) noexcept {nullptr};

    /// \brief True when a non-trivial sink is installed.
    [[nodiscard]] bool active() const noexcept {
        return route != nullptr || decode_failed != nullptr;
    }
};

} // namespace hpactor::net
