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

/// \brief Configuration for system health checks run by \c WatchdogActor.
///
/// Each check can be individually enabled or disabled.  Thresholds control
/// when a check transitions from \c Healthy to \c Degraded or \c Unhealthy.
struct HealthCheckConfig {
    /// Master switch.  When \c false all health checks are skipped and the
    /// system is always reported as \c Healthy.
    bool enabled = true;

    // -- Scheduler liveness ------------------------------------------------

    /// Check that scheduler workers are making forward progress.
    bool scheduler_liveness_enabled = true;
    /// Seconds without any worker executing an actor before marking
    /// \c Degraded.
    uint32_t scheduler_progress_deadline_sec = 30;

    // -- System actor health -----------------------------------------------

    /// Check that critical system actors are not in Failed or Quarantined
    /// state.
    bool system_actor_health_enabled = true;

    // -- Dead-letter queue growth ------------------------------------------

    /// Check that the DLQ is not flooding.
    bool dlq_growth_enabled = true;
    /// DLQ depth as a percentage of capacity that triggers \c Degraded.
    uint32_t dlq_depth_warning_pct = 80;
    /// DLQ depth as a percentage of capacity that triggers \c Unhealthy.
    uint32_t dlq_depth_critical_pct = 95;
    /// Sustained loss rate (records / minute) that triggers \c Degraded.
    uint32_t dlq_lost_rate_per_minute = 10;

    // -- Memory pressure ---------------------------------------------------

    /// Check OS-reported memory pressure.
    bool memory_pressure_enabled = true;
    /// Memory utilisation percentage that triggers \c Degraded.
    uint8_t memory_warning_pct = 85;
    /// Memory utilisation percentage that triggers \c Unhealthy.
    uint8_t memory_critical_pct = 95;
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

    /// Health check configuration for the watchdog.
    HealthCheckConfig health_check;

    // systemd notify socket override (for testing; empty = use $NOTIFY_SOCKET)
    std::string notify_socket;

    /// Parse mode from a string ("foreground", "systemd", "daemon").
    /// Case-insensitive. Returns Foreground for unknown values.
    static ProcessMode parse_mode(const std::string& s);
};

} // namespace hpactor::process
