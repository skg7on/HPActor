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

#include <atomic>
#include <cstdint>

#include <hpactor/actor/lifecycle/drain_config.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/actor/lifecycle/quarantine_reason.hpp>
#include <hpactor/types/types.hpp> // complete error type needed for value members

namespace hpactor {

class LifecycleActor {
  public:
    LifecycleActor()
        : state_(static_cast<uint8_t>(LifecycleState::kStarting)),
          incarnation_(0) {}
    virtual ~LifecycleActor() = default;

    // ── State queries ──────────────────────────────────
    LifecycleState state() const noexcept {
        return static_cast<LifecycleState>(state_.load(std::memory_order_acquire));
    }
    bool accepts_user_msgs() const noexcept {
        return kStateMachine[static_cast<int>(state())].accepts_user_msgs;
    }
    bool accepts_system_msgs() const noexcept {
        return kStateMachine[static_cast<int>(state())].accepts_system_msgs;
    }
    const char* state_string() const noexcept {
        return kStateMachine[static_cast<int>(state())].name;
    }
    uint64_t incarnation() const noexcept {
        return incarnation_.load(std::memory_order_acquire);
    }
    void bump_incarnation() {
        incarnation_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Failure reason ─────────────────────────────────
    // Set before transition(kFailed) so on_fail() receives the real error.
    void set_failure_reason(error err) {
        failure_reason_ = err;
    }
    error failure_reason() const {
        return failure_reason_;
    }

    // ── Drain config accessors ──────────────────────────
    DrainConfig drain_config() const noexcept {
        return drain_config_;
    }
    void set_drain_config(DrainConfig cfg) noexcept {
        drain_config_ = cfg;
    }

    // ── Transition ─────────────────────────────────────
    // Validates that `to` is a legal transition from the current state,
    // performs a CAS, and invokes the corresponding virtual hook.
    // Returns false if the transition is illegal or CAS fails.
    bool transition(LifecycleState to);

    // ── Quarantine ─────────────────────────────────────

    /// \brief Transition the actor into \c kQuarantined.
    ///
    /// Stores the reason and a monotonic timestamp for observability.
    /// The \c on_quarantined() hook is invoked after the CAS succeeds.
    ///
    /// \param[in] reason Why the actor is being quarantined.
    /// \return \c true if the transition succeeded (legal + CAS won).
    ///         \c false if the state does not permit this transition
    ///         or a concurrent writer changed the state.
    /// \note Thread safety: callable from the owning scheduler thread
    ///       or from the supervisor thread that owns this child.
    bool transition_to_quarantined(QuarantineReason reason);

    /// \brief Operator override: release from quarantine.
    ///
    /// Transitions \c kQuarantined → \c kStopped. The supervisor or
    /// operator must restart the actor through \c kStarting → \c kActive.
    ///
    /// \return \c true if the transition succeeded. \c false if the
    ///         actor is not currently quarantined or the CAS fails.
    /// \note Thread safety: callable from CLI or admin threads.
    ///       The \c state_ CAS provides synchronization.
    bool transition_from_quarantined();

    /// \brief Whether the actor is currently in \c kQuarantined state.
    [[nodiscard]] bool is_quarantined() const noexcept {
        return state() == LifecycleState::kQuarantined;
    }

    /// \brief Reason the actor was quarantined.
    ///
    /// \return The \c QuarantineReason stored by the most recent call
    ///         to \c transition_to_quarantined(). The value is
    ///         meaningless if \c is_quarantined() is \c false.
    [[nodiscard]] QuarantineReason quarantine_reason() const noexcept {
        return quarantine_reason_;
    }

    /// \brief Monotonic timestamp of the quarantine transition.
    ///
    /// \return The \c steady_clock::time_point captured when
    ///         \c transition_to_quarantined() succeeded. The value
    ///         is meaningless if \c is_quarantined() is \c false.
    [[nodiscard]] std::chrono::steady_clock::time_point
    quarantined_at() const noexcept {
        return quarantined_at_;
    }

    // ── Virtual hooks (default = no-op) ────────────────
    virtual void on_start() {}
    virtual void on_drain() {}
    virtual void on_drain_timeout() {}
    virtual void on_stop() {}
    virtual void on_deactivate() {}
    virtual void on_fail(error err);
    virtual void on_recover() {}
    virtual void on_restart() {}

    /// \brief Hook invoked after the actor transitions to \c kQuarantined.
    ///
    /// Override to perform quarantine-specific cleanup (e.g., flush
    /// pending work, notify dependents). The default implementation
    /// is a no-op.
    ///
    /// \param[in] reason The \c QuarantineReason that triggered the
    ///                   quarantine. Available via \c quarantine_reason().
    /// \note Called from the CAS path in \c transition(), under the
    ///       owning scheduler thread or supervisor thread.
    virtual void on_quarantined(QuarantineReason /*reason*/) {}

    /// \brief Hook invoked after transition to \c kPassivating.
    ///
    /// Default starts draining the actor's mailbox. Override to add
    /// custom pre-passivation logic.
    virtual void on_passivating() {}

    /// \brief Hook invoked after transition to \c kPassivated.
    ///
    /// Default releases actor memory and installs the route stub.
    /// Override to add custom post-passivation logic.
    virtual void on_passivated() {}

    // NOTE: LifecycleActor does NOT override as_lifecycle().
    // Each lifecycle actor class must explicitly override
    // AbstractActor::as_lifecycle() to return this. Example:
    //   LifecycleActor* as_lifecycle() override { return this; }
    // This is required because LifecycleActor does not inherit from
    // AbstractActor, so it cannot override its virtual method directly.

  protected:
    std::atomic<uint8_t> state_;
    std::atomic<uint64_t> incarnation_;
    error failure_reason_{0};
    DrainConfig drain_config_{};
    QuarantineReason quarantine_reason_{QuarantineReason::SupervisionEscalation};
    std::chrono::steady_clock::time_point quarantined_at_{};
};

} // namespace hpactor