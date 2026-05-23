// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

#include <hpactor/actor/drain_config.hpp>
#include <hpactor/actor/lifecycle_state.hpp>
#include <hpactor/actor/quarantine_reason.hpp>
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
    /// Transition the actor into kQuarantined with the given reason.
    /// Stores the reason and timestamp for observability.
    /// Returns false if the transition is illegal or CAS fails.
    bool transition_to_quarantined(QuarantineReason reason);

    /// Operator override: release from quarantine via
    /// kQuarantined → kStopped. The supervisor or operator is
    /// expected to restart the actor through kStarting → kActive.
    bool transition_from_quarantined();

    bool is_quarantined() const noexcept {
        return state() == LifecycleState::kQuarantined;
    }
    QuarantineReason quarantine_reason() const noexcept {
        return quarantine_reason_;
    }

    /// Timestamp when the actor was quarantined (monotonic clock).
    std::chrono::steady_clock::time_point quarantined_at() const noexcept {
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
    virtual void on_quarantined(QuarantineReason /*reason*/) {}

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
