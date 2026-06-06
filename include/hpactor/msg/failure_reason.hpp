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

/// Canonical failure reason shared across actor send, ask, RPC, spawn,
/// DLQ, and tracing. Every failed delivery or runtime control failure
/// maps to one of these codes.
///
/// Numeric ranges partition the failure space: route (0-9), lifecycle
/// (10-19), resource (20-29), time (30-39), policy (40-49), transport
/// (50-59), dedup (60-69), shutdown (70-79), reliable messaging (80-89),
/// spawn (90-99), passivation (100-109). \c Unknown = 255 is the sentinel.
enum class FailureReason : uint8_t {
    // ── Route / addressing (0-9) ────────────────────────────────
    NoRoute = 0,           ///< Actor or node not found for the target address.
    NodeUnavailable = 1,   ///< Remote node unreachable (network, partition,
                           ///< down).
    RemoteUnavailable = 2, ///< Remote endpoint or service is unavailable
                           ///< (circuit breaker open, refused connection).

    // ── Actor lifecycle (10-19) ─────────────────────────────────
    ActorDead = 10,     ///< Target actor has terminated.
    ActorNotReady = 11, ///< Actor exists but not accepting messages (Starting,
                        ///< Draining, Recovering).
    Quarantined = 12,   ///< Actor is quarantined — rejecting all user messages.
    CircuitOpen = 13,   ///< Circuit breaker is open — calls blocked until
                        ///< cooldown.

    // ── Resource limits (20-29) ─────────────────────────────────
    MailboxFull = 20,       ///< Target actor's mailbox is at capacity.
    OutboundQueueFull = 21, ///< Remote endpoint's outbound queue is at
                            ///< capacity.
    MemoryPressure = 22,    ///< Node-level memory pressure prevents admission.
    ResourceExhausted = 23, ///< General resource exhaustion (backpressure,
                            ///< outbound queue, rate limit).

    // ── Time (30-39) ────────────────────────────────────────────
    Expired = 30, ///< Message deadline expired before delivery.
    Timeout = 31, ///< Operation timed out (ask/request, RPC, spawn).

    // ── Policy (40-49) ──────────────────────────────────────────
    RejectedByPolicy = 40, ///< Rejected by admission or overflow policy.
    Dropped = 41,          ///< Message explicitly dropped
                           ///< (DropNewest/DropOldest/DropLowestPriority).
    MailboxClosed = 42,    ///< Mailbox is closed (actor draining, stopping, or
                           ///< stopped).

    // ── Transport / serialization (50-59) ───────────────────────
    SerializationError = 50, ///< Message encode or decode failure.
    TransportError = 51, ///< Network-level transport failure (connection lost,
                         ///< reset).
    FrameRejected = 52,  ///< Frame validation failure (size, malformed,
                         ///< corrupt).

    // ── Deduplication (60-69) ───────────────────────────────────
    Duplicate = 60, ///< Duplicate message suppressed by receiver dedup cache.

    // ── Graceful shutdown (70-79) ───────────────────────────────
    Draining = 70,     ///< Node or actor is draining — new ingress rejected.
    ShuttingDown = 71, ///< Node is shutting down — all user ingress rejected.

    // ── Reliable messaging (80-89) ──────────────────────────────
    RetryExhausted = 80, ///< All retry attempts exhausted without ACK.

    // ── Spawn (90-99) ───────────────────────────────────────────
    SpawnFailed = 90, ///< Remote spawn failed (codec, permission, node, type).

    // ── Passivation (100-109) ───────────────────────────────────
    PassivationDrainTimeout = 100, ///< Drain did not complete within deadline.
    PassivationSnapshotFailed = 101, ///< Durable store write failed.
    ReactivationFailed = 102,        ///< Restore from durable store failed.
    PassivationQueueFull = 103,      ///< Reactivation buffer exhausted.
    SchemaVersionMismatch = 104,     ///< Stored schema has no migration path.

    // ── Sentinel ────────────────────────────────────────────────
    Unknown = 255, ///< Unclassified failure. Must not be used for new
                   ///< production paths.
};

/// Which subsystem produced a failure. Combined with FailureReason to
/// disambiguate context (e.g. Timeout from ActorRuntime vs Rpc).
enum class FailureSource : uint8_t {
    ActorRuntime, ///< Actor send/reply/spawn paths.
    Mailbox,      ///< Mailbox admission.
    Rpc,          ///< RPC channel.
    Transport,    ///< Network transport (TCP, TLS, frame).
    Discovery,    ///< Service discovery (registrar, gossip).
    Scheduler,    ///< Scheduling / timer infrastructure.
    Config,       ///< Config validation / bootstrap.
    Security,     ///< Authentication / authorization.
    DurableStore, ///< Durable state / event store.
    Supervision,  ///< Supervision / restart.
    Cluster,      ///< Cluster membership / sharding.
    Unknown,      ///< Unspecified source.
};

/// \brief Whether the caller can retry with a reasonable chance of success.
///
/// \param[in] reason The failure reason to check.
/// \return true if the reason represents a transient condition likely to
///         resolve without application-level intervention.
constexpr bool retryable(FailureReason reason) noexcept {
    switch (reason) {
        case FailureReason::NoRoute:
        case FailureReason::NodeUnavailable:
        case FailureReason::ActorNotReady:
        case FailureReason::CircuitOpen:
        case FailureReason::MailboxFull:
        case FailureReason::OutboundQueueFull:
        case FailureReason::MemoryPressure:
        case FailureReason::Timeout:
        case FailureReason::TransportError:
        case FailureReason::Draining:
        case FailureReason::ShuttingDown:
        case FailureReason::ResourceExhausted:
        case FailureReason::RemoteUnavailable:
            return true;
        default:
            return false;
    }
}

/// \brief Human-readable snake_case string for metrics labels, log keys,
///        and CLI.
///
/// \param[in] reason The failure reason.
/// \return A null-terminated snake_case string literal (e.g. "no_route",
///         "mailbox_full"). Never returns nullptr.
const char* to_string(FailureReason reason) noexcept;

/// \brief Human-readable snake_case string for the subsystem source.
///
/// \param[in] source The failure source.
/// \return A null-terminated snake_case string literal (e.g. "actor_runtime",
///         "mailbox"). Never returns nullptr.
const char* to_string(FailureSource source) noexcept;

} // namespace hpactor
