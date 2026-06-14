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

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor::process {

enum class ProcessMode : uint8_t {
    Foreground, ///< Attached to terminal (default).
    Systemd,    ///< systemd Type=notify, no fork.
    Daemon,     ///< Traditional double-fork daemon.
};

struct ProcessConfig {
    ProcessMode mode = ProcessMode::Foreground;
    std::string pidfile_path;    ///< e.g., "/var/run/hpactor/hpactor.pid"
    bool redirect_stdio = false; ///< Redirect stdin/out/err to /dev/null
                                 ///< (daemon)
    std::string log_file;        ///< Optional log file path for daemon mode
    std::string working_directory = "/"; ///< chdir target for daemon mode

    // systemd watchdog
    std::chrono::milliseconds watchdog_interval{0}; ///< 0 = disabled

    // systemd notify socket override (for testing; empty = use $NOTIFY_SOCKET)
    std::string notify_socket;

    /// Parse mode from a string ("foreground", "systemd", "daemon").
    /// Case-insensitive. Returns Foreground for unknown values.
    static ProcessMode parse_mode(const std::string& s);
};

} // namespace hpactor::process
