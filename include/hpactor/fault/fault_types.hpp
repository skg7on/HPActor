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
#include <string_view>

namespace hpactor::fault {

/// \brief Per-subsystem domain for deterministic fault injection tick counters.
///
/// Each value represents an independent tick counter advanced by the owning
/// subsystem. Fault schedules match against a specific domain so that a
/// schedule targeting the transport does not interfere with mailbox or
/// scheduler injection sites.
enum class FaultDomain : uint8_t {
    kMailbox = 0,      ///< Mailbox enqueue/dequeue and DLQ paths.
    kTransport = 1,    ///< TCP transport, connection pool, wireframe, and
                       ///< acceptor.
    kScheduler = 2,    ///< Work-stealing, dispatch, and worker notification.
    kAllocator = 3,    ///< Slab allocator, segment provider, and freelist.
    kStorage = 4,      ///< Durable storage and snapshot I/O (backlog).
    kTimer = 5,        ///< Timing wheel schedule, advance, and cancel.
    kGossip = 6,       ///< SWIM gossip protocol rounds and state transitions.
    kConfig = 7,       ///< TOML parsing and topology bootstrap.
    kActor = 8,        ///< Actor lifecycle, handler dispatch, become, and CLI.
    kRpc = 9,          ///< RPC send, response, timeout, and retry.
    kSupervision = 10, ///< Supervision restart, child tracking, and directives.
    kDiscovery = 11,   ///< Service discovery heartbeat, register, and location
                       ///< cache.
    kTracing = 12,     ///< Distributed tracing span start, finish, and drain.
    kMetrics = 13,     ///< Metrics aggregation and event processing.
    kPassivation = 14, ///< Actor passivation, reactivation, and snapshot I/O.
};

/// \brief Convert a \c FaultDomain value to its enumerator name.
///
/// \param[in] d The domain value to convert.
/// \return A \c std::string_view of the enumerator name (e.g. \c "kMailbox"),
///         or \c "kUnknown" for values outside the defined range.
constexpr std::string_view to_string(FaultDomain d) noexcept {
    switch (d) {
        case FaultDomain::kMailbox:
            return "kMailbox";
        case FaultDomain::kTransport:
            return "kTransport";
        case FaultDomain::kScheduler:
            return "kScheduler";
        case FaultDomain::kAllocator:
            return "kAllocator";
        case FaultDomain::kStorage:
            return "kStorage";
        case FaultDomain::kTimer:
            return "kTimer";
        case FaultDomain::kGossip:
            return "kGossip";
        case FaultDomain::kConfig:
            return "kConfig";
        case FaultDomain::kActor:
            return "kActor";
        case FaultDomain::kRpc:
            return "kRpc";
        case FaultDomain::kSupervision:
            return "kSupervision";
        case FaultDomain::kDiscovery:
            return "kDiscovery";
        case FaultDomain::kTracing:
            return "kTracing";
        case FaultDomain::kMetrics:
            return "kMetrics";
        case FaultDomain::kPassivation:
            return "kPassivation";
    }
    return "kUnknown";
}

/// \brief Action taken when a fault injection site fires.
///
/// Each action describes the observable effect on the enclosing code path.
/// The injection site controls how it interprets the action; not every site
/// supports every action.
enum class FaultAction : uint8_t {
    kFail = 0,  ///< Return a synthetic error code to the caller.
    kDrop = 1,  ///< Silently discard the operation (message, packet, or event).
    kDelay = 2, ///< Stall the domain tick counter by a configurable number of
                ///< ticks.
    kCorrupt = 3, ///< Flip bits at a byte offset in the payload or state.
    kPanic = 4,   ///< Call \c std::abort() immediately after logging.
};

/// \brief Convert a \c FaultAction value to its short name.
///
/// \param[in] a The action value to convert.
/// \return A \c std::string_view of the action name (e.g. \c "Fail"),
///         or \c "Unknown" for values outside the defined range.
constexpr std::string_view to_string(FaultAction a) noexcept {
    switch (a) {
        case FaultAction::kFail:
            return "Fail";
        case FaultAction::kDrop:
            return "Drop";
        case FaultAction::kDelay:
            return "Delay";
        case FaultAction::kCorrupt:
            return "Corrupt";
        case FaultAction::kPanic:
            return "Panic";
    }
    return "Unknown";
}

} // namespace hpactor::fault
