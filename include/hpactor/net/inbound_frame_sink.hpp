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

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/runtime/network_dispatch_targets.hpp>

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

// ── Typed-target sink ────────────────────────────────────────────────────────

/// \brief Allocation-free inbound frame delivery sink.
///
/// Installed into \c ConnectionPool before ingress begins. When non-null,
/// every valid frame and decode failure is routed exclusively through this
/// sink; legacy per-category handlers receive zero calls.
///
/// The sink stores a typed \c InboundFrameTarget pointer — one virtual
/// dispatch per event, no allocation, no facade capture.  The target must
/// outlive all callback invocations (typically the router object itself).
struct InboundFrameSink {
    /// Typed target — typically the InboundFrameRouter itself.
    InboundFrameTarget* target{nullptr};

    /// \brief Route a successfully decoded frame.
    FrameDispatchResult
    route(const InboundFrameContext& ictx, const WireFrame& frame) const noexcept {
        if (target) {
            return target->on_frame(ictx, frame);
        }
        return FrameDispatchResult{};
    }

    /// \brief Report a decode failure.
    FrameDispatchResult on_decode_failure(const InboundFrameContext& ictx,
                                          FrameDecodeError error) const noexcept {
        if (target) {
            return target->on_decode_failure(ictx, error);
        }
        return FrameDispatchResult{};
    }

    /// \brief True when a non-trivial sink is installed.
    [[nodiscard]] bool active() const noexcept {
        return target != nullptr;
    }
};

} // namespace hpactor::net
