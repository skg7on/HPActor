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
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace process {

struct HealthCheckConfig;

/// \brief Overall system health classification.
///
/// Used by \c WatchdogActor to decide whether to notify systemd's watchdog
/// and by \c HealthHttpServer to select HTTP status codes.
enum class HealthStatus : uint8_t {
    /// All enabled checks pass.
    Healthy = 0,
    /// One or more non-critical checks indicate degraded operation.
    /// The system is still serving but operators should investigate.
    Degraded = 1,
    /// One or more critical checks indicate the system needs intervention.
    /// The watchdog stops notifying systemd so the process is restarted.
    Unhealthy = 2,
};

/// \brief Read-only context passed to each health check invocation.
struct CheckContext {
    /// Reference to the actor system under inspection.
    ActorSystem& system;
    /// Time elapsed since the previous health check cycle.
    /// Zero on the first invocation.
    std::chrono::milliseconds elapsed;
};

/// \brief Outcome of a single health check.
struct HealthCheckResult {
    /// Logical name of the check, e.g. "scheduler_liveness".
    std::string check_name;
    /// Classification for this check.
    HealthStatus status = HealthStatus::Healthy;
    /// Human-readable explanation when the status is not \c Healthy.
    /// Empty string when healthy.
    std::string reason;
};

/// \brief Thread-safe snapshot of the most recent health evaluation.
///
/// The \c WatchdogActor writes results on its timer tick and the
/// \c HealthHttpServer reads them on incoming HTTP requests.  The two
/// actors run on different threads, so all state is protected by a
/// mutex.
class HealthState {
  public:
    HealthState() = default;

    /// \brief Replace the cached health evaluation atomically.
    void update(HealthStatus overall, std::vector<HealthCheckResult> details,
                std::chrono::steady_clock::time_point timestamp) noexcept;

    /// \brief Current overall health classification.
    HealthStatus overall_status() const noexcept;

    /// \brief Copy of the per-check details from the last evaluation.
    std::vector<HealthCheckResult> details() const;

    /// \brief Timestamp of the last completed health check cycle.
    std::chrono::steady_clock::time_point last_check_time() const noexcept;

  private:
    mutable std::mutex mutex_;
    HealthStatus overall_ = HealthStatus::Healthy;
    std::vector<HealthCheckResult> details_;
    std::chrono::steady_clock::time_point last_check_{};
};

/// \brief Abstract interface for a single system health check.
///
/// Concrete checks are registered with \c HealthCheckEngine and evaluated
/// on every watchdog timer tick.  Each check receives a read-only
/// \c CheckContext and returns a \c HealthCheckResult.
class IHealthCheck {
  public:
    virtual ~IHealthCheck() = default;

    /// \brief Human-readable name for diagnostics and logging.
    virtual std::string_view name() const noexcept = 0;

    /// \brief Whether this check is critical.
    ///
    /// When a \e critical check returns \c Unhealthy the overall system
    /// status becomes \c Unhealthy and the watchdog stops notifying
    /// systemd.  When a \e non-critical check returns \c Unhealthy the
    /// overall status is capped at \c Degraded.
    virtual bool is_critical() const noexcept = 0;

    /// \brief Execute this check against the current system state.
    virtual HealthCheckResult check(const CheckContext& ctx) = 0;
};

/// \brief Owns a collection of \c IHealthCheck instances and runs them
///        as a batch, aggregating results into a \c HealthState.
class HealthCheckEngine {
  public:
    HealthCheckEngine() = default;

    /// \brief Register a check.  Checks are run in registration order.
    void add_check(std::unique_ptr<IHealthCheck> check);

    /// \brief Run every registered check and update \p state with the
    ///        aggregated outcome.
    ///
    /// The aggregation rule:
    /// - If any \e critical check returns \c Unhealthy → overall \c Unhealthy.
    /// - Else if any check returns \c Unhealthy or \c Degraded → overall
    ///   \c Degraded.
    /// - Else → overall \c Healthy.
    void run_all(const CheckContext& ctx, HealthState& state);

    /// \brief Number of registered checks.
    size_t size() const noexcept {
        return checks_.size();
    }

  private:
    std::vector<std::unique_ptr<IHealthCheck>> checks_;
};

// ===========================================================================
// Concrete health checks
// ===========================================================================

/// \brief Checks that scheduler workers are making forward progress.
///
/// Compares \c WorkerSnapshot::actors_executed across successive polls.
/// If no progress is detected for longer than \c progress_deadline_sec the
/// check returns \c Unhealthy.  Also returns \c Unhealthy if the scheduler
/// is not running or absent.
class SchedulerLivenessCheck : public IHealthCheck {
  public:
    /// \param progress_deadline_sec  Seconds without worker progress before
    ///                               the check is considered unhealthy.
    explicit SchedulerLivenessCheck(uint32_t progress_deadline_sec = 30);

    std::string_view name() const noexcept override;
    bool is_critical() const noexcept override;
    HealthCheckResult check(const CheckContext& ctx) override;

    /// \brief Reset internal progress tracking (for testing).
    void reset_baseline() noexcept;

  private:
    uint32_t progress_deadline_sec_;
    uint64_t prev_actors_executed_ = 0;
    std::chrono::steady_clock::time_point last_progress_time_{};
    bool has_baseline_ = false;
};

/// \brief Checks that critical system actors are not in failed or
///        quarantined state.
///
/// Iterates all actors marked as system actors via
/// \c AbstractActor::is_system_actor() and inspects their lifecycle state.
class SystemActorHealthCheck : public IHealthCheck {
  public:
    SystemActorHealthCheck() = default;

    std::string_view name() const noexcept override;
    bool is_critical() const noexcept override;
    HealthCheckResult check(const CheckContext& ctx) override;
};

/// \brief Checks that the dead-letter queue is not flooding.
///
/// Monitors DLQ depth as a percentage of capacity and the sustained loss
/// rate (records / minute).  This check is \e not critical because messages
/// can be lost without the system being non-functional.
class DLQGrowthCheck : public IHealthCheck {
  public:
    /// \param depth_warning_pct   Depth/capacity percentage for \c Degraded.
    /// \param depth_critical_pct  Depth/capacity percentage for \c Unhealthy.
    /// \param lost_rate_per_minute  Sustained loss rate for \c Degraded.
    DLQGrowthCheck(uint32_t depth_warning_pct = 80, uint32_t depth_critical_pct = 95,
                   uint32_t lost_rate_per_minute = 10);

    std::string_view name() const noexcept override;
    bool is_critical() const noexcept override;
    HealthCheckResult check(const CheckContext& ctx) override;

    /// \brief Reset internal loss-rate history (for testing).
    void reset_baseline() noexcept;

  private:
    void update_history(uint64_t total_lost);

    uint32_t depth_warning_pct_;
    uint32_t depth_critical_pct_;
    uint32_t lost_rate_per_minute_;
    uint64_t prev_total_lost_ = 0;
    bool has_baseline_ = false;
};

/// \brief Checks OS-reported memory utilisation.
///
/// The default pressure reader calls the platform's native API
/// (\c sysinfo(2) on Linux, \c host_statistics64 on macOS).  For
/// deterministic testing an injectable reader function can be provided.
class MemoryPressureCheck : public IHealthCheck {
  public:
    /// \brief Signature for an injectable OS memory pressure reader.
    /// \return Memory utilisation percentage (0–100).
    using PressureReader = uint8_t (*)();

    /// \param warning_pct   Percentage threshold for \c Degraded.
    /// \param critical_pct  Percentage threshold for \c Unhealthy.
    /// \param reader        Function that returns current memory pressure.
    ///                      The default reads from the OS.
    explicit MemoryPressureCheck(uint8_t warning_pct = 85, uint8_t critical_pct = 95,
                                 PressureReader reader = nullptr);

    std::string_view name() const noexcept override;
    bool is_critical() const noexcept override;
    HealthCheckResult check(const CheckContext& ctx) override;

  private:
    uint8_t warning_pct_;
    uint8_t critical_pct_;
    PressureReader reader_;
};

/// \brief Create an engine pre-populated with checks according to \p config.
///
/// Each check enabled in \p config is instantiated with the configured
/// thresholds.  When \c config.enabled is \c false the returned engine is
/// empty (all checks pass by default).
std::unique_ptr<HealthCheckEngine>
make_health_check_engine(const HealthCheckConfig& config);

} // namespace process
} // namespace hpactor
