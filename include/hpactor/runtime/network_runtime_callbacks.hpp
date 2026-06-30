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

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>

namespace hpactor {

namespace net {
class Transport;
struct WireFrame;
struct Member;
} // namespace net

/// \brief Fixed-size non-owning sink for node membership events.
///
/// Callbacks fire on the network loop thread. The target must outlive the
/// discovery subscription and must not be destroyed until discovery callbacks
/// are quiesced (subscription removed + last callback returned).
struct NodeEventSink {
    void* context{nullptr};
    void (*member_changed)(void* ctx, const net::Member& member,
                           bool joined) noexcept {nullptr};
};

/// \brief Fixed-size non-owning port for reliable-retry processing.
///
/// Called from the network loop thread at a fixed poll interval.
/// The target (MessagingRuntime) must outlive NetworkRuntime.
struct OutboundRetryPort {
    void* context{nullptr};
    void (*process_due)(void* ctx, uint64_t now_ns) noexcept {nullptr};
};

/// \brief Fixed-size non-owning port for remote-spawn receiver lifecycle.
///
/// Called from the network component during startup and stop.
/// The target (ActorRuntime) owns the spawn receiver actor and its
/// directory entry; NetworkRuntime owns only the protocol registration.
struct RemoteSpawnPort {
    void* context{nullptr};

    /// \brief Install the remote-spawn receiver actor into the directory
    ///        and register spawn protocol handlers with the transport.
    /// \return The actor address on success, or an error code.
    result<ActorAddress> (*install_receiver)(void* ctx,
                                             net::Transport& transport) noexcept {
        nullptr};

    /// \brief Remove the spawn receiver's protocol registration.
    void (*remove_receiver)(void* ctx) noexcept {nullptr};
};

/// \brief Fixed-size non-owning telemetry sink for network events.
///
/// May be null when metrics are disabled. Callers check for null
/// before each call.
struct NetworkTelemetryPort {
    void* context{nullptr};
    void (*emit_event)(void* ctx, uint32_t event_type,
                       uint64_t val) noexcept {nullptr};
};

/// \brief Fixed-size non-owning inbound frame sink (Phase 4 contract).
///
/// Installed before listening. The target (InboundFrameRouter) must
/// outlive NetworkRuntime.
struct InboundFrameSinkPort {
    void* context{nullptr};
    void (*sink)(void* ctx, const net::WireFrame& frame) noexcept {nullptr};
};

} // namespace hpactor
