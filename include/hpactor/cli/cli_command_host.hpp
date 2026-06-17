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

#include <hpactor/cli/cli_types.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

class OutputFormatter;

// Forward-declare protobuf types (defined in cli_messages.pb.h)
class InspectStateReply;
class InspectStateRequest;
class KillReply;
class KillRequest;
class QuarantineReply;
class QuarantineRequest;

/// \brief Core interface for actor-level CLI operations.
///
/// Every CLI host (local or remote) must implement actor inspection, kill,
/// quarantine, and enumeration. Commands use this interface without knowing
/// whether they run on a local daemon or a remote client.
class ICliCommandHost {
  public:
    virtual ~ICliCommandHost() = default;

    /// \brief Inspect an actor and return its full state.
    virtual std::optional<InspectStateReply>
    inspect(ActorId target, const InspectStateRequest& req,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) = 0;

    /// \brief Kill (force-stop) an actor.
    virtual std::optional<KillReply>
    kill(ActorId target, const KillRequest& req,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) = 0;

    /// \brief Quarantine or unquarantine an actor.
    virtual std::optional<QuarantineReply>
    quarantine(ActorId target, const QuarantineRequest& req,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) = 0;

    /// \brief Enumerate all known actors, optionally filtered by type
    /// substring.
    virtual std::vector<ActorMeta> enumerate(std::string_view filter = "") = 0;
};

/// \brief Interface for system-level CLI queries.
///
/// Methods take an OutputFormatter& so the host can either render directly
/// from local data (CliActor/CliLegacyServerActor) or forward the command over
/// the wire and write the response payload (CliClientActor). Commands never
/// branch on local vs. remote.
class ISystemCliHost {
  public:
    virtual ~ISystemCliHost() = default;

    virtual void render_system_stats(OutputFormatter& output) = 0;
    virtual void render_memory_stats(OutputFormatter& output) = 0;
    virtual void render_fault_status(OutputFormatter& output) = 0;
    virtual void render_scheduler_workers(OutputFormatter& output) = 0;
    virtual void render_metrics_show(OutputFormatter& output) = 0;
    virtual void
    render_dlq_list(OutputFormatter& output, std::string_view filter = "") = 0;

    /// \brief Replay a dead-letter record to its original or an alternate
    /// target.
    /// \param[in] index 0-based index into the DLQ ring buffer.
    /// \param[in] target Actor to receive the replayed message.
    /// \return Success or an error code (e.g. \c kNotFound if DLQ is not
    ///         configured, \c kInvalidArgument if the index is out of range).
    virtual result<void> dlq_replay(uint32_t index, ActorId target) = 0;
};

/// \brief Interface for lifecycle CLI operations (drain, shutdown).
class ILifecycleCliHost {
  public:
    virtual ~ILifecycleCliHost() = default;

    /// \brief Drain in-flight messages without accepting new work.
    ///
    /// Actors stop accepting new messages but continue processing already
    /// enqueued work. Typically Step 1 of graceful shutdown.
    virtual result<void> drain() = 0;

    /// \brief Shut down the actor system.
    ///
    /// Initiates full shutdown: drain first, then stop all actors, then
    /// tear down the scheduler and system resources. Irreversible.
    virtual result<void> shutdown() = 0;
};

} // namespace cli
} // namespace hpactor
