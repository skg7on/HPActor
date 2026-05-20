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
enum class FailureReason : uint8_t {
    // ── Route / addressing (0-9) ────────────────────────────────
    NoRoute = 0,
    NodeUnavailable = 1,

    // ── Actor lifecycle (10-19) ─────────────────────────────────
    ActorDead = 10,
    ActorNotReady = 11,
    Quarantined = 12,
    CircuitOpen = 13,

    // ── Resource limits (20-29) ─────────────────────────────────
    MailboxFull = 20,
    OutboundQueueFull = 21,
    MemoryPressure = 22,

    // ── Time (30-39) ────────────────────────────────────────────
    Expired = 30,
    Timeout = 31,

    // ── Policy (40-49) ──────────────────────────────────────────
    RejectedByPolicy = 40,
    Dropped = 41,
    MailboxClosed = 42,

    // ── Transport / serialization (50-59) ───────────────────────
    SerializationError = 50,
    TransportError = 51,
    FrameRejected = 52,

    // ── Deduplication (60-69) ───────────────────────────────────
    Duplicate = 60,

    // ── Graceful shutdown (70-79) ───────────────────────────────
    Draining = 70,
    ShuttingDown = 71,

    // ── Reliable messaging (80-89) ──────────────────────────────
    RetryExhausted = 80,

    // ── Spawn (90-99) ───────────────────────────────────────────
    SpawnFailed = 90,

    // ── Sentinel ────────────────────────────────────────────────
    Unknown = 255,
};

/// Which subsystem produced a failure. Combined with FailureReason to
/// disambiguate context (e.g. Timeout from ActorRuntime vs Rpc).
enum class FailureSource : uint8_t {
    ActorRuntime,
    Mailbox,
    Rpc,
    Transport,
    Discovery,
    Scheduler,
    Config,
    Security,
    DurableStore,
    Supervision,
    Cluster,
    Unknown,
};

/// Whether the caller can retry with a reasonable chance of success.
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
            return true;
        default:
            return false;
    }
}

/// Human-readable snake_case string for metrics labels, log keys, and CLI.
const char* to_string(FailureReason reason) noexcept;

/// Human-readable snake_case string for the subsystem source.
const char* to_string(FailureSource source) noexcept;

} // namespace hpactor
