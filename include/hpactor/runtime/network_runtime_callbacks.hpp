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
#include <hpactor/runtime/observability_dispatch_targets.hpp>

#include <cstdint>

namespace hpactor {

/// \brief Fixed-size non-owning sink for node membership events.
///
/// Callbacks fire on the network loop thread. The target must outlive the
/// discovery subscription and must not be destroyed until discovery callbacks
/// are quiesced (subscription removed + last callback returned).
struct NodeEventSink {
    NodeEventTarget* target{nullptr};

    /// \brief Call when a node joins or leaves the cluster.
    void on_member_changed(const net::Member& member, bool joined) const noexcept {
        if (target) {
            target->on_member_changed(member, joined);
        }
    }
};

/// \brief Fixed-size non-owning handler for reliable-retry processing.
///
/// Called from the network loop thread at a fixed poll interval.
/// The target (MessagingRuntime) must outlive NetworkRuntime.
struct OutboundRetryHandler {
    OutboundRetryTarget* target{nullptr};

    /// \brief Process due retries.
    void process_due(uint64_t now_ns) const noexcept {
        if (target) {
            target->process_due(now_ns);
        }
    }
};

/// \brief Fixed-size non-owning handler for remote-spawn receiver
///        lifecycle.
///
/// Called from the network component during startup and stop.
/// The target (ActorRuntime) owns the spawn receiver actor and its
/// directory entry; NetworkRuntime owns only the protocol registration.
struct RemoteSpawnHandler {
    RemoteSpawnTarget* target{nullptr};

    /// \brief Install the remote-spawn receiver actor into the directory
    ///        and register spawn protocol handlers with the transport.
    /// \return The actor address on success, or an error code.
    result<ActorAddress> install_receiver(net::Transport& transport) const noexcept {
        if (target) {
            return target->install_receiver(transport);
        }
        return result<ActorAddress>::make(
            error(errors::actor_not_found, "remote spawn target unavailable"));
    }

    /// \brief Remove the spawn receiver's protocol registration.
    void remove_receiver() const noexcept {
        if (target) {
            target->remove_receiver();
        }
    }
};

/// \brief Fixed-size non-owning telemetry sink for network events.
///
/// May be null when metrics are disabled. Callers check for null
/// before each call.
struct NetworkTelemetrySink {
    MetricsTarget* target{nullptr};

    /// \brief Emit a network metric event.
    void emit_event(uint32_t event_type, uint64_t val) const noexcept {
        if (target) {
            target->on_metric_event(event_type, val);
        }
    }
};

} // namespace hpactor
