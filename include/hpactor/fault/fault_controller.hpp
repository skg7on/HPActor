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

#include <hpactor/fault/fault_schedule.hpp>
#include <hpactor/fault/fault_types.hpp>
#include <hpactor/log/log_manager.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor::fault {

/// \brief Point-in-time snapshot of \c FaultController state for observability.
///
/// Returned by \c snapshot() and \c aggregate_snapshot() for CLI inspection.
struct FaultControllerSnapshot {
    bool enabled;             ///< Whether fault injection is currently active.
    std::string active_scope; ///< Active scope pattern (e.g. \c
                              ///< "hpactor.mailbox.*").
    uint64_t replay_seed;     ///< Seed used for \c expand_random replay.
    size_t schedule_entry_count; ///< Number of entries in the loaded schedule.
    uint64_t domain_ticks[14];   ///< Current tick counter per \c FaultDomain.
    uint64_t faults_fired;       ///< Total faults fired since last \c load().
};

/// \brief Per-thread deterministic fault injection controller.
///
/// Each worker thread owns one \c FaultController registered via \c install().
/// The controller maintains per-domain tick counters advanced by the owning
/// subsystem, a sorted schedule consumed sequentially by \c check(), and a
/// scope pattern that restricts which injection sites are live.
///
/// \note Thread safety: the owning thread calls \c check(), \c advance_tick(),
///       and \c stall() without external synchronization. The \c load(),
///       \c clear(), \c enable(), and \c disable() methods broadcast to all
///       registered instances under an internal mutex. The \c schedule_ pointer
///       is swapped under mutex but read lock-free via \c std::shared_ptr.
class FaultController {
  public:
    /// \brief Construct a disabled controller with scope \c "*".
    FaultController();

    /// \brief Broadcast a new sorted schedule to all registered instances.
    ///
    /// The schedule is copied, sorted, and shared via \c std::shared_ptr so
    /// that concurrent \c check() calls hold a stable snapshot.
    ///
    /// \param[in] schedule The schedule to load (need not be pre-sorted).
    void load(const FaultSchedule& schedule);

    /// \brief Clear the schedule on all registered instances.
    void clear();

    /// \brief Enable fault injection with the given scope pattern on all
    /// instances.
    ///
    /// \param[in] scope_pattern A dot-separated prefix or \c "*" for all sites.
    void enable(std::string_view scope_pattern);

    /// \brief Disable fault injection on all instances.
    void disable();

    /// \brief Return whether fault injection is currently enabled.
    bool is_enabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

    /// \brief Check whether a fault should fire at \p path for the calling
    /// thread.
    ///
    /// Called by the \c FAULT_INJECT(path) macro. Advances the domain tick
    /// counter for the matching \c FaultPoint, then scans the schedule for an
    /// entry at the current (domain, tick, path). On match, increments
    /// \c faults_fired_, logs a structured fault event, and returns \c true.
    /// If the matched entry has action \c kPanic, calls \c std::abort().
    ///
    /// \param[in] path Exact dot-separated injection site path.
    /// \param[in] target Optional actor filter matched against the schedule
    /// entry.
    /// \return \c true if a fault should be injected, \c false otherwise.
    /// \note This is a fast-path function. When disabled, the \c enabled_
    /// atomic
    ///       load ensures the cold branch is predicted not-taken via
    ///       \c HPACTOR_UNLIKELY in the \c FAULT_INJECT macro.
    bool check(std::string_view path, std::optional<ActorId> target = std::nullopt);

    /// \brief Advance the tick counter for \p domain by one.
    ///
    /// Called by the owning subsystem at well-defined progress points.
    ///
    /// \param[in] domain The subsystem domain to advance.
    void advance_tick(FaultDomain domain);

    /// \brief Artificially advance \p domain by \p delay_ticks.
    ///
    /// Used to implement \c FaultAction::kDelay in injection sites that
    /// support it.
    ///
    /// \param[in] domain The subsystem domain to stall.
    /// \param[in] delay_ticks Number of ticks to advance.
    void stall(FaultDomain domain, uint64_t delay_ticks);

    /// \brief Set the replay seed for \c expand_random reproducibility.
    void set_replay_seed(uint64_t seed) {
        replay_seed_ = seed;
    }
    /// \brief Return the current replay seed.
    uint64_t replay_seed() const noexcept {
        return replay_seed_;
    }

    /// \brief Set the log manager for structured fault-event logging.
    ///
    /// When set, each fired fault emits a structured log entry at
    /// \c LogLevel::kInfo with category \c LogCategory::kFault.
    void set_log_manager(log::LogManager* lm) {
        log_manager_ = lm;
    }

    /// \brief Capture a point-in-time snapshot of this instance's state.
    FaultControllerSnapshot snapshot() const;

    /// \brief Aggregate snapshots across all registered instances.
    ///
    /// Domain ticks and faults fired are summed; the active scope is taken
    /// from the first non-\c "*" scope seen.
    static FaultControllerSnapshot aggregate_snapshot();

    /// \brief Return the total faults fired by this instance since last \c
    /// load().
    uint64_t faults_fired() const noexcept {
        return faults_fired_.load(std::memory_order_acquire);
    }

    /// \brief Register this controller on the calling thread.
    ///
    /// Sets the thread-local instance pointer and adds this controller to the
    /// global instance list. Safe to call multiple times (double-registration
    /// is guarded).
    ///
    /// \pre Must be called from the thread that will own this controller.
    void install();

    /// \brief Unregister this controller from the calling thread.
    ///
    /// Removes this controller from the global instance list and clears the
    /// thread-local pointer.
    void remove();

    /// \brief Return the \c FaultController installed on the calling thread.
    ///
    /// \return The thread-local controller pointer, or \c nullptr if none
    ///         is installed.
    static FaultController* instance();

  private:
    static thread_local FaultController* tls_instance_;

    struct InstanceList {
        std::mutex mutex;
        std::vector<FaultController*> instances;
    };
    static InstanceList& instance_list();

    void load_impl(const FaultSchedule& schedule);
    void clear_impl();
    void enable_impl(std::string_view scope_pattern);
    void disable_impl();

    std::atomic<bool> enabled_{false};
    std::string active_scope_;
    // Shared pointer to allow check() to hold a stable snapshot while
    // load()/clear() swap in a new schedule from the broadcast path.
    std::shared_ptr<const FaultSchedule> schedule_;
    // Per-instance cursor, only accessed by the owning thread's check().
    size_t schedule_cursor_{0};
    uint64_t domain_ticks_[14]{};
    uint64_t replay_seed_{0};
    std::atomic<uint64_t> faults_fired_{0};
    log::LogManager* log_manager_ = nullptr;
};

} // namespace hpactor::fault
