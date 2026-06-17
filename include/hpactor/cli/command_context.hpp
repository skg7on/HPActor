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

#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/types/types.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

class CliActor;
class CliLegacyServerActor;
class CliProtoServerActor;
class CliSession;
class OutputFormatter;

/// \brief Execution context passed to command handlers.
///
/// Populated by CliActor after tokenizing and walking the command tree.
/// Contains parsed arguments, captured parameters, output configuration,
/// and references to the actor system and CLI actor for request-response
/// round-trips.
struct CommandContext {
    /// \brief Positional arguments in order (beyond the matched command path).
    std::vector<std::string> args;
    /// \brief Captured parameter values (e.g. \c <id>) and flag values
    ///        (e.g. \c --format).
    std::map<std::string, std::string> params;
    /// \brief Host interface pointers — the preferred way for commands to
    /// access
    ///        actor/system/lifecycle operations. Populated by CliSession from
    ///        the owning actor (CliActor, CliLegacyServerActor, or
    ///        CliClientActor).
    ICliCommandHost* command_host = nullptr;
    ISystemCliHost* system_host = nullptr;
    ILifecycleCliHost* lifecycle_host = nullptr;
    /// \brief The actor system, for sending inspect/kill/list requests.
    ActorSystem* system = nullptr;
    /// \brief The CLI actor (stdin-based), for request-response helpers.
    CliActor* cli_actor = nullptr;
    /// \brief The CLI server actor (socket-based), for request-response
    ///        helpers when \c cli_actor is null.
    CliLegacyServerActor* cli_server_actor = nullptr;
    /// \brief The protobuf CLI server, for client-management commands.
    CliProtoServerActor* cli_proto_server = nullptr;
    /// \brief The owning session, for shutdown requests.
    CliSession* cli_session = nullptr;
    /// \brief Output formatter for rendering results.
    OutputFormatter* output = nullptr;
    /// \brief Whether output should be paged.
    bool paged = false;
    /// \brief Page size for paged output.
    uint32_t page_size = 50;
    /// \brief Output format name ("pretty", "json", "tabular").
    std::string format = "pretty";

    /// \brief Check whether a boolean flag is present.
    ///
    /// \param[in] name Flag name (without \c -- prefix).
    /// \retval true The flag is present and its value is "true" or empty.
    /// \retval false The flag is absent or explicitly set to "false".
    bool has_flag(const std::string& name) const {
        auto it = params.find(name);
        return it != params.end() && (it->second == "true" || it->second.empty());
    }

    /// \brief Get the value of a named parameter or flag.
    ///
    /// \param[in] name Parameter name (without angle brackets or \c -- prefix).
    /// \return The value if present, otherwise \c std::nullopt.
    std::optional<std::string> get_param(const std::string& name) const {
        auto it = params.find(name);
        if (it != params.end())
            return it->second;
        return std::nullopt;
    }
};

} // namespace cli
} // namespace hpactor
