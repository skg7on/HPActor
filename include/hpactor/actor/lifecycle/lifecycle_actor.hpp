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

/// \brief Opt-in mixin that adds a finite-state-machine lifecycle to an actor.
///
/// Manages a \c LifecycleState stored in an atomic \c uint8_t with
/// CAS-based transitions. Each legal transition invokes a virtual hook
/// that subclasses override for custom behavior (e.g., drain, stop,
/// fail, quarantine, passivation). Message gate queries —
/// \c accepts_user_msgs() and \c accepts_system_msgs() — are driven
/// from the compile-time \c kStateMachine transition table.
///
/// \note Thread safety: the \c state_ CAS provides the sole
///       synchronization point for transitions. State queries use
///       \c memory_order_acquire and are safe to call from any thread.
///       Transition methods are callable from the owning scheduler
///       thread or the supervisor thread that owns this child.
///       Virtual hooks are invoked on the CAS winner's thread.
///
/// \note This class does NOT inherit from \c AbstractActor. Each
///       concrete lifecycle actor must explicitly override
///       \c AbstractActor::as_lifecycle() to return \c this.
class LifecycleActor {
  public:
    /// \brief Constructs the lifecycle actor in \c kStarting state.
    ///
    /// The initial incarnation counter is zero. The failure reason
    /// is default-initialized to \c error{0}.
    LifecycleActor()
        : state_(static_cast<uint8_t>(LifecycleState::kStarting)),
          incarnation_(0) {}

    /// \brief Virtual destructor.
    ///
    /// Ownership: the destructor is trivial. Subclass destructors are
    /// responsible for releasing actor-specific resources before the
    /// lifecycle base is destroyed.
    virtual ~LifecycleActor() = default;

    // ── State queries ──────────────────────────────────

    /// \brief Current lifecycle state.
    ///
    /// \return The \c LifecycleState value last written by a successful
    ///         \c transition() call, or \c kStarting if no transition
    ///         has occurred.
    /// \note Thread safety: lock-free atomic load with acquire ordering.
    LifecycleState state() const noexcept {
        return static_cast<LifecycleState>(state_.load(std::memory_order_acquire));
    }

    /// \brief Whether the actor accepts user (data-plane) messages in
    ///        its current state.
    ///
    /// \return \c true when the state is \c kActive.
    /// \note Used by the mailbox admission path to reject messages
    ///       before enqueue.
    bool accepts_user_msgs() const noexcept {
        return kStateMachine[static_cast<int>(state())].accepts_user_msgs;
    }

    /// \brief Whether the actor accepts system messages in its current
    ///        state.
    ///
    /// \return \c true for every state except \c kStopped.
    /// \note System messages include link/monitor signals, CLI
    ///       introspection requests, and supervision commands.
    bool accepts_system_msgs() const noexcept {
        return kStateMachine[static_cast<int>(state())].accepts_system_msgs;
    }

    /// \brief Human-readable name of the current lifecycle state.
    ///
    /// \return A null-terminated string literal (e.g. \c "active",
    ///         \c "draining", \c "failed"). Never returns \c nullptr.
    const char* state_string() const noexcept {
        return kStateMachine[static_cast<int>(state())].name;
    }

    /// \brief Monotonic incarnation counter.
    ///
    /// Incremented on each restart to distinguish incarnations of the
    /// same \c ActorId (e.g., for mailbox deduplication and incarnation
    /// fencing).
    ///
    /// \return The current incarnation value.
    /// \note Thread safety: lock-free atomic load with acquire ordering.
    uint64_t incarnation() const noexcept {
        return incarnation_.load(std::memory_order_acquire);
    }

    /// \brief Increment the incarnation counter by one.
    ///
    /// \note Thread safety: lock-free atomic fetch-add with relaxed
    ///       ordering. Typically called from the owning scheduler thread
    ///       during restart.
    void bump_incarnation() {
        incarnation_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Failure reason ─────────────────────────────────

    /// \brief Set the failure reason before transitioning to \c kFailed.
    ///
    /// The stored reason is passed to \c on_fail() when the transition
    /// succeeds. Must be called before \c transition(LifecycleState::kFailed).
    ///
    /// \param[in] err The error that caused the failure.
    /// \note Thread safety: not atomic. Call from the thread that owns
    ///       the upcoming \c transition() call.
    // Set before transition(kFailed) so on_fail() receives the real error.
    void set_failure_reason(error err) {
        failure_reason_ = err;
    }

    /// \brief The error that caused the most recent failure.
    ///
    /// \return The \c error value set by the last call to
    ///         \c set_failure_reason(). Meaningful only when
    ///         \c state() is \c kFailed or during an \c on_fail() hook.
    error failure_reason() const {
        return failure_reason_;
    }

    // ── Drain config accessors ──────────────────────────

    /// \brief Current drain configuration.
    ///
    /// \return A copy of the \c DrainConfig (policy + timeout).
    DrainConfig drain_config() const noexcept {
        return drain_config_;
    }

    /// \brief Override the drain configuration.
    ///
    /// \param[in] cfg The new drain policy and timeout to use when
    ///                this actor transitions to \c kDraining.
    /// \note Thread safety: not atomic. Call before the drain
    ///       transition, typically during actor construction or
    ///       configuration.
    void set_drain_config(DrainConfig cfg) noexcept {
        drain_config_ = cfg;
    }

    // ── Transition ─────────────────────────────────────

    /// \brief Attempt a lifecycle state transition.
    ///
    /// Validates that \p to is a legal edge from the current state
    /// (checked against \c kStateMachine), performs a CAS on \c state_,
    /// and invokes the corresponding virtual hook on success.
    ///
    /// \param[in] to The target lifecycle state.
    /// \return \c true if the transition was legal and the CAS succeeded.
    ///         \c false if the transition is not in the state machine,
    ///         or a concurrent writer changed \c state_ before the CAS.
    /// \post On success, exactly one virtual hook is invoked:
    ///       <table>
    ///       <tr><th>Target</th><th>From</th><th>Hook</th></tr>
    ///       <tr><td>\c kActive</td><td>\c kStarting, \c kRecovering</td><td>\c
    ///       on_start()</td></tr> <tr><td>\c kDraining</td><td>any</td><td>\c
    ///       on_drain()</td></tr> <tr><td>\c kStopping</td><td>any</td><td>\c
    ///       on_stop()</td></tr> <tr><td>\c kStopped</td><td>any</td><td>\c
    ///       on_deactivate()</td></tr> <tr><td>\c
    ///       kFailed</td><td>any</td><td>\c on_fail(failure_reason_)</td></tr>
    ///       <tr><td>\c kStarting</td><td>\c kFailed, \c kStopped</td><td>\c
    ///       on_restart()</td></tr> <tr><td>\c
    ///       kRecovering</td><td>any</td><td>\c on_recover()</td></tr>
    ///       <tr><td>\c kQuarantined</td><td>any</td><td>\c
    ///       on_quarantined(reason)</td></tr> <tr><td>\c
    ///       kPassivating</td><td>any</td><td>\c on_passivating()</td></tr>
    ///       <tr><td>\c kPassivated</td><td>any</td><td>\c
    ///       on_passivated()</td></tr>
    ///       </table>
    /// \note Thread safety: the CAS provides the synchronization
    ///       barrier. The virtual hook runs on the calling thread.
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

    /// \brief Hook invoked when the actor enters \c kActive.
    ///
    /// Called after transition from \c kStarting or \c kRecovering.
    /// Override to perform actor-specific initialization (e.g.,
    /// subscribe to events, send bootstrap messages).
    /// \note Called from the CAS path in \c transition(), under the
    ///       owning scheduler thread.
    virtual void on_start() {}

    /// \brief Hook invoked when the actor enters \c kDraining.
    ///
    /// Override to stop accepting new work and begin flushing
    /// in-flight messages. The default implementation is a no-op.
    /// \note Called from the CAS path in \c transition(), under the
    ///       owning scheduler thread.
    virtual void on_drain() {}

    /// \brief Hook invoked when the drain timeout fires.
    ///
    /// Override to handle incomplete drain (e.g., force-drop pending
    /// work, log the remaining queue depth). The default implementation
    /// is a no-op.
    /// \note Not called from \c transition(). Invoked externally by
    ///       the drain timeout mechanism (typically the timing wheel).
    virtual void on_drain_timeout() {}

    /// \brief Hook invoked when the actor enters \c kStopping.
    ///
    /// Override to begin tearing down internal state while still
    /// accepting system messages. The default implementation is a no-op.
    /// \note Called from the CAS path in \c transition(), under the
    ///       owning scheduler thread.
    virtual void on_stop() {}

    /// \brief Hook invoked when the actor enters \c kStopped.
    ///
    /// Override to release resources, unregister from registries, and
    /// clean up. After this hook returns the actor accepts neither user
    /// nor system messages. The default implementation is a no-op.
    /// \note Called from the CAS path in \c transition(), under the
    ///       owning scheduler thread.
    virtual void on_deactivate() {}

    /// \brief Hook invoked when the actor enters \c kFailed.
    ///
    /// The default implementation is a no-op (defined in
    /// \c lifecycle_actor.cpp). Override to perform failure-specific
    /// cleanup, emit metrics, or log diagnostic state.
    ///
    /// \param[in] err The error set by \c set_failure_reason() before
    ///                the transition. Available via \c failure_reason().
    /// \note Called from the CAS path in \c transition(), under the
    ///       owning scheduler thread.
    virtual void on_fail(error err);

    /// \brief Hook invoked when the actor enters \c kRecovering.
    ///
    /// Override to perform recovery logic (e.g., reload state from a
    /// snapshot, validate invariants) before re-entering \c kActive.
    /// The default implementation is a no-op.
    /// \note Called from the CAS path in \c transition(), under the
    ///       owning scheduler thread.
    virtual void on_recover() {}

    /// \brief Hook invoked when the actor restarts from \c kFailed or
    ///        \c kStopped back to \c kStarting.
    ///
    /// Override to reset internal state before \c on_start() runs.
    /// The default implementation is a no-op.
    /// \note Called from the CAS path in \c transition(), under the
    ///       owning scheduler thread.
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
    /// \brief Atomic lifecycle state.
    ///
    /// Stored as \c uint8_t for CAS efficiency. All transitions go
    /// through \c compare_exchange_strong with \c acq_rel ordering.
    /// \note Thread safety: the CAS on this field is the sole
    ///       synchronization point for lifecycle transitions.
    std::atomic<uint8_t> state_;

    /// \brief Monotonic incarnation counter.
    ///
    /// Incremented on each restart via \c bump_incarnation().
    /// \note Thread safety: accessed with \c memory_order_relaxed
    ///       for increments, \c memory_order_acquire for reads.
    std::atomic<uint64_t> incarnation_;

    /// \brief Error that caused the most recent failure.
    ///
    /// Set by \c set_failure_reason() before transitioning to
    /// \c kFailed. Passed to \c on_fail().
    /// \note Thread safety: not atomic. Write before \c transition().
    error failure_reason_{0};

    /// \brief Drain policy and timeout for this actor.
    ///
    /// Default: \c DrainPolicy::Drain with a 30-second timeout.
    /// Override via \c set_drain_config() before the drain transition.
    DrainConfig drain_config_{};

    /// \brief Reason for the most recent quarantine.
    ///
    /// Default: \c QuarantineReason::SupervisionEscalation.
    /// Set by \c transition_to_quarantined().
    QuarantineReason quarantine_reason_{QuarantineReason::SupervisionEscalation};

    /// \brief Wall-clock timestamp of the quarantine transition.
    ///
    /// Captured via \c std::chrono::steady_clock::now() inside
    /// \c transition() when entering \c kQuarantined. Meaningless
    /// unless \c is_quarantined() returns \c true.
    std::chrono::steady_clock::time_point quarantined_at_{};
};

} // namespace hpactor
