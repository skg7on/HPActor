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
#include <string>

namespace hpactor {
namespace cli {

/// \brief Configuration for the interactive CLI subsystem.
///
/// All fields map to TOML keys under \c [system.cli]. The CLI is opt-in:
/// \c enabled must be set to \c true via TOML or programmatic config.
struct CliConfig {
    /// \brief Enable the CLI subsystem. Defaults to false (opt-in).
    bool enabled = false;
    /// \brief Unix domain socket path for CLI connections.
    ///        Empty means stdin/stdout.
    std::string listen_path;
    /// \brief TCP listen port. 0 means disabled.
    uint16_t tcp_port = 0;
    /// \brief Default output format: "pretty", "json", or "tabular".
    std::string default_format = "pretty";
    /// \brief Number of items per page for paged output.
    uint32_t page_size = 50;
    /// \brief History file path. Empty means \c ~/.hpactor_history, falling
    ///        back to \c /tmp/.hpactor_history if the home directory is
    ///        unavailable.
    std::string history_path;
    /// \brief Maximum number of in-memory history entries.
    uint32_t history_max = 1000;
};

} // namespace cli
} // namespace hpactor
