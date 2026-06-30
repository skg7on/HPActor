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

#include <hpactor/types/types.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

namespace hpactor {

// ── Lifecycle state ─────────────────────────────────────────────────────────

/// \brief States of the runtime lifecycle state machine.
enum class RuntimeLifecycleState : uint8_t {
    Built,        ///< Components constructed, nothing started.
    Preflighting, ///< Process preflight in progress (daemonization etc.).
    Starting,     ///< Component startup stages in progress.
    Running,      ///< All stages complete, accepting work.
    RollingBack,  ///< Startup failure, reversing completed stages.
    Failed,       ///< Startup failed, all rollbacks complete.
    Draining,     ///< Shutdown initiated, draining in-flight work.
    Stopping,     ///< Components stopping in reverse order.
    Stopped,      ///< All components stopped.
};

// ── Lifecycle action ────────────────────────────────────────────────────────

/// \brief A function-pointer action (start or rollback) bound to a context.
/// Returns true on success, false on failure (triggers rollback of prior
/// stages).
struct LifecycleAction {
    void* context{nullptr};
    bool (*action)(void* context) noexcept {nullptr};
};

// ── Lifecycle stage ─────────────────────────────────────────────────────────

/// \brief One startup stage with a name, start action, and rollback action.
struct RuntimeLifecycleStage {
    const char* name{nullptr};
    LifecycleAction start;
    LifecycleAction rollback;
    /// \brief Optional cleanup action called after coordinator stop/destroy.
    /// Used to free heap-allocated stage context.
    void (*destroy_context)(void* context) noexcept {nullptr};
};

// ── Lifecycle snapshot ──────────────────────────────────────────────────────

/// \brief Bounded snapshot of coordinator state for CLI/admin visibility.
struct RuntimeLifecycleSnapshot {
    RuntimeLifecycleState state{RuntimeLifecycleState::Built};
    bool ready{false};
    uint64_t transition_epoch{0};
    const char* last_stage{nullptr};
    uint32_t primary_error_code{0};
    uint32_t rollback_error_bits{0};
};

// ── RuntimeCoordinator ──────────────────────────────────────────────────────

/// \brief Non-owning lifecycle mediator for the actor runtime.
///
/// Owns the startup, rollback, readiness, and stop state machine.
/// Stages are registered in dependency order and executed sequentially.
/// On any stage failure, completed stages are rolled back in reverse.
/// Stop is idempotent and safe from any state.
class RuntimeCoordinator final {
  public:
    RuntimeCoordinator() = default;
    ~RuntimeCoordinator();

    /// \brief Add a startup stage in dependency order.
    void add_stage(RuntimeLifecycleStage stage);

    /// \brief Execute all registered stages in order.
    result<void> start() noexcept;

    /// \brief Stop all components, draining if running.
    result<void> stop() noexcept;

    /// \brief Current lifecycle state.
    RuntimeLifecycleState state() const noexcept;

    /// \brief True only in Running state after all stages complete.
    bool is_ready() const noexcept;

    /// \brief Bounded snapshot for observability.
    RuntimeLifecycleSnapshot snapshot() const noexcept;

  private:
    void rollback(size_t completed_count) noexcept;
    void transition_to(RuntimeLifecycleState new_state) noexcept;

    std::vector<RuntimeLifecycleStage> stages_;
    std::atomic<RuntimeLifecycleState> state_{RuntimeLifecycleState::Built};
    std::atomic<uint64_t> epoch_{0};
    std::atomic<bool> ready_{false};
    std::atomic<uint32_t> primary_error_{0};
    std::atomic<uint32_t> rollback_bits_{0};
    const char* last_stage_{nullptr};
};

} // namespace hpactor
