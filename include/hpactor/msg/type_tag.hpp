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

namespace hpactor {

/// \brief Type identifier for message serialization and dispatch (replaces
///        RTTI).
///
/// Every \c TypedMessage carries a \c TypeTag. The runtime uses the tag to
/// dispatch to registered protobuf handlers and to classify system vs
/// user messages. Tags are organized in non-overlapping ranges.
///
/// Range layout:
/// - \c 0x00–0xFF: System messages (256 slots)
/// - \c 0x0100–0x0FFF: Reserved for future system expansion
/// - \c 0x1000–0xFFFFFF: Application-defined messages (~16M slots)
///
/// \note Values in the system range (0x00–0xFF) receive special treatment:
///       protected system-lane delivery, bypass of user-level overflow
///       policies, and intercept-only dispatch in \c EventBasedActor.
enum class TypeTag : uint32_t {
    Invalid = 0x00000000, ///< Sentinel; no valid message uses this tag.

    // ── Core system (0x00–0x0F) ──────────────────────────────────────────
    DownMsg = 0x01,      ///< Actor-down notification (link/monitor).
    ExitMsg = 0x02,      ///< Actor exit notification.
    LinkMsg = 0x03,      ///< Link-request system message.
    UnlinkMsg = 0x04,    ///< Unlink-request system message.
    MonitorMsg = 0x0A,   ///< Monitor-request system message.
    DemonitorMsg = 0x0B, ///< Demonitor-request system message.

    // ── Spawn protocol (0x10–0x1F) ───────────────────────────────────────
    SpawnRequestTag = 0x10,  ///< Remote spawn request.
    SpawnResponseTag = 0x11, ///< Remote spawn response.
    ErrorMsg = 0x12,         ///< Generic error response.

    // ── HTTP protocol (0x20–0x2F) ────────────────────────────────────────
    HttpRequestTag = 0x20,  ///< HTTP ingress request.
    HttpResponseTag = 0x21, ///< HTTP egress response.

    // ── TOML config bootstrapping (0x30–0x3F) ────────────────────────────
    SystemInitTag = 0x30, ///< Broadcast after full topology spawn.

    // ── Metrics subsystem (0x40–0x4F) ────────────────────────────────────
    MetricsRequestTag = 0x40,  ///< Prometheus /metrics scrape request.
    MetricsResponseTag = 0x41, ///< Prometheus /metrics scrape response.

    // ── CLI interactive subsystem (0x50–0x5F) ────────────────────────────
    InspectStateRequestTag = 0x50,     ///< CLI actor state inspection request.
    InspectStateResponseTag = 0x51,    ///< CLI actor state inspection response.
    KillRequestTag = 0x52,             ///< CLI kill-actor request.
    KillResponseTag = 0x53,            ///< CLI kill-actor response.
    ListActorsRequestTag = 0x54,       ///< CLI list-actors request.
    ListActorsResponseTag = 0x55,      ///< CLI list-actors response.
    SystemStatsRequestTag = 0x56,      ///< CLI system-stats request.
    SystemStatsResponseTag = 0x57,     ///< CLI system-stats response.
    MemoryStatsRequestTag = 0x58,      ///< CLI memory-stats request.
    MemoryStatsResponseTag = 0x59,     ///< CLI memory-stats response.
    TopologyShowRequestTag = 0x5A,     ///< CLI topology-show request.
    TopologyShowResponseTag = 0x5B,    ///< CLI topology-show response.
    TopologyRestartRequestTag = 0x5C,  ///< CLI topology-restart request.
    TopologyRestartResponseTag = 0x5D, ///< CLI topology-restart response.
    QuarantineRequestTag = 0x5E,       ///< CLI quarantine request.
    QuarantineResponseTag = 0x5F,      ///< CLI quarantine response.

    // ── Async I/O (0x60–0x6F) ────────────────────────────────────────────
    IoCompletionTag = 0x60, ///< Async I/O completion notification.

    // ── Backpressure control (0x70–0x7F) ─────────────────────────────────
    BackpressureSignalTag = 0x70, ///< Backpressure signal (local or remote).

    // ── Receptionist (0x71–0x75) ───────────────────────────────────────────
    ReceptionistRegisterTag = 0x71,    ///< Register actor under a ServiceKey.
    ReceptionistSubscribeTag = 0x72,   ///< Subscribe to ServiceKey changes.
    ReceptionistUnregisterTag = 0x73,  ///< Unregister actor from a ServiceKey.
    ReceptionistUnsubscribeTag = 0x74, ///< Unsubscribe from a ServiceKey.
    ReceptionistListingTag = 0x75,     ///< ServiceKey membership listing.

    // ── Subsystem extension range (0x80–0xFF) ────────────────────────────
    // 256 slots reserved for subsystem-defined TypeTags.
    //
    // Subsystems declare their tags as inline constexpr in their own headers:
    //   namespace hpactor::ai {
    //   inline constexpr TypeTag kAiLeaseRequestTag =
    //       make_subsystem_tag(0x80);
    //   }
    //
    // These are NOT added to the TypeTag enum. They are constexpr variables
    // that implicitly convert to TypeTag. This keeps the core enum closed
    // while subsystems own their tag definitions.

    // ── Application range ────────────────────────────────────────────────
    User = 0x00001000, ///< Start of application-defined message tags.
};

/// Construct a TypeTag from a subsystem-range value (0x80–0xFF).
/// Compile-time only via consteval; the value must be a constant expression.
consteval TypeTag make_subsystem_tag(uint32_t value) {
    return static_cast<TypeTag>(value);
}

} // namespace hpactor
