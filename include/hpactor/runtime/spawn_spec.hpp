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

#include <hpactor/actor/lifecycle/quarantine_policy.hpp>
#include <hpactor/core/actor_id.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/sched/dispatch_policy.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace hpactor {

/// \brief Origin of a spawn request — affects observability, never behavior.
enum class SpawnOrigin : uint8_t {
    Programmatic,  ///< spawn<T>() or equivalent direct call.
    Configured,    ///< spawn_configured() / TOML topology.
    System,        ///< Reserved system actor (SpawnReceiver, etc.).
    RemoteFactory, ///< ActorTypeRegistry remote factory invocation.
};

/// \brief Resolved spawn policy for one actor adoption.
///
/// Built by variant adapters (template, configured, system, remote) and
/// consumed by \c ActorSpawner::adopt().  All fields carry effective values;
/// the spawner does not consult global config or actor-declared defaults.
struct SpawnSpec final {
    std::string_view type_name; ///< Copied into the actor during adoption.
    std::optional<std::string_view> registered_name; ///< Optional published
                                                     ///< name.
    mailbox::MailboxConfig mailbox;             ///< Complete mailbox config.
    sched::DispatchPolicy dispatch_policy;      ///< Effective dispatch policy.
    sched::DispatchHints dispatch_hints;        ///< Effective dispatch hints.
    std::optional<QuarantinePolicy> quarantine; ///< If enabled, per-actor
                                                ///< policy.
    std::optional<ActorId> reserved_id;         ///< Absent = auto-allocate.
    std::optional<ActorType> actor_type_override; ///< For system actor types.
    SpawnOrigin origin{SpawnOrigin::Programmatic};
};

} // namespace hpactor
