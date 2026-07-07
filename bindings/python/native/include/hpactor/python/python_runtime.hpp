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

#include <hpactor/python/native_notifier.hpp>
#include <hpactor/python/python_bridge_types.hpp>
#include <hpactor/python/python_ports.hpp>
#include <hpactor/python/python_runtime_config.hpp>
#include <hpactor/python/python_runtime_queues.hpp>
#include <hpactor/python/python_runtime_snapshot.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace hpactor::python {

class PythonRuntime;

/// \brief Lifecycle states for the Python bridge runtime.
enum class PythonRuntimeState : uint8_t {
    Created,  ///< Initial state after construction, before start().
    Starting, ///< Transitional state during start() before notifiers are ready.
    Running,  ///< Fully operational; queues accept submissions.
    Draining, ///< Draining in-progress deliveries; no new submissions accepted.
    Stopping, ///< Shutting down notifiers and clearing wake port.
    Stopped,  ///< Terminal state; all resources released.
    Failed,   ///< Terminal state entered after an unrecoverable error.
};

/// \brief Point-in-time snapshot of the Python runtime state for metrics and
///        introspection.
struct PythonRuntimeSnapshot final {
    /// Current lifecycle state.
    PythonRuntimeState state{PythonRuntimeState::Created};

    /// Queue depth and rejection counters.
    PythonQueueSnapshot queues;

    /// Current number of bound actor IDs.
    size_t actor_bindings{0};

    /// Cumulative count of completions rejected due to stale generation.
    uint64_t stale_completion_rejected{0};

    /// Read-end file descriptor of the dispatch notifier, or -1 if closed.
    int dispatch_notifier_fd{-1};

    /// Read-end file descriptor of the completion notifier, or -1 if closed.
    int completion_notifier_fd{-1};
};

/// \brief Move-only lease that binds an ActorId to a monotonic generation.
///
/// Reserved from a PythonRuntime. Once bound via bind(), the lease holds the
/// exclusive right to identify that actor by its current generation.
/// Destruction or reset() releases the binding, but only erases the
/// id/generation pair if it has not been superseded by a newer reservation.
class PythonActorLease final {
  public:
    PythonActorLease(PythonActorLease&& other) noexcept;
    PythonActorLease& operator=(PythonActorLease&& other) noexcept;
    ~PythonActorLease();

    PythonActorLease(const PythonActorLease&) = delete;
    PythonActorLease& operator=(const PythonActorLease&) = delete;

    /// \brief Bind this lease to a specific ActorId.
    ///
    /// Inserts the id/generation pair into the runtime's active binding map.
    /// Only valid when the lease is not already bound and is not empty (i.e.
    /// was obtained from a successful reserve_actor call).
    ///
    /// \param[in] actor_id The actor ID to bind.
    /// \return true if the binding was successfully inserted.
    [[nodiscard]] bool bind(ActorId actor_id) noexcept;

    /// \brief Release this lease, unbinding the actor ID if still bound.
    void reset() noexcept;

    /// \brief The monotonic generation assigned at reservation time.
    ///
    /// \return The generation value, or 0 if the lease is empty.
    [[nodiscard]] uint64_t generation() const noexcept;

    /// \brief The actor ID bound to this lease, or 0 if not bound.
    ///
    /// \return The bound ActorId.
    [[nodiscard]] ActorId actor_id() const noexcept;

  private:
    friend class PythonRuntime;

    /// \brief Construct a lease reserved from a runtime with a specific
    ///        generation.
    ///
    /// \param[in] runtime The owning PythonRuntime.
    /// \param[in] gen The monotonic generation assigned to this reservation.
    PythonActorLease(PythonRuntime* runtime, uint64_t generation) noexcept;

    PythonRuntime* runtime_{nullptr};
    ActorId actor_id_{};
    uint64_t generation_{0};
    bool bound_{false};
};

/// \brief Central coordinator for the Python bridge subsystem.
///
/// Owns the three lock-free queues (dispatch, command, completion), two
/// NativeNotifier instances for waking the Python event loop, and the actor
/// lease registry with monotonic generation tracking.
///
/// Lifecycle: Created → Starting → Running → Draining → Stopping → Stopped.
/// Failed is entered on unrecoverable errors (e.g. notifier creation failure
/// during start).
class PythonRuntime final {
  public:
    /// \brief Create a PythonRuntime with the given configuration.
    ///
    /// Validates the config and constructs the three queues. Notifiers are
    /// created lazily during start().
    ///
    /// \param[in] config Runtime configuration.
    /// \return A result containing a unique_ptr to the runtime, or an error.
    static result<std::unique_ptr<PythonRuntime>>
    create(PythonRuntimeConfig config) noexcept;

    ~PythonRuntime();

    PythonRuntime(const PythonRuntime&) = delete;
    PythonRuntime& operator=(const PythonRuntime&) = delete;

    /// \brief Start the runtime with a wake port for command notification.
    ///
    /// Creates the dispatch and completion notifiers. Transitions
    /// Created → Starting → Running. On notifier creation failure, transitions
    /// to Failed. Only valid when the wake port is non-empty.
    ///
    /// \param[in] wake_port Valid GatewayWakePort for command wakeups.
    /// \return ok() on success, or an error.
    [[nodiscard]] result<void> start(GatewayWakePort wake_port) noexcept;

    /// \brief Begin draining the runtime.
    ///
    /// Transitions Running → Draining. After this call, queue submissions
    /// are rejected but existing in-flight deliveries may still complete.
    void begin_draining() noexcept;

    /// \brief Stop the runtime.
    ///
    /// Idempotent. Closes admission, closes both notifiers, clears the
    /// wake port, and transitions to Stopped. Safe to call from any state.
    ///
    /// \return ok() on success.
    [[nodiscard]] result<void> stop() noexcept;

    /// \brief Reserve an actor lease slot.
    ///
    /// Increments the global monotonic generation counter and the reservation
    /// count. Returns std::nullopt when the maximum number of bindings has
    /// been reached.
    ///
    /// \return A PythonActorLease if a slot is available, std::nullopt
    ///         otherwise.
    [[nodiscard]] std::optional<PythonActorLease> reserve_actor() noexcept;

    /// \brief Check whether an ActorId's current generation matches the given
    ///        value.
    ///
    /// \param[in] actor_id The actor ID to look up.
    /// \param[in] generation The generation to check.
    /// \return true if the actor is bound with exactly this generation.
    [[nodiscard]] bool
    generation_matches(ActorId actor_id, uint64_t generation) const noexcept;

    // ── Queue submission ───────────────────────────────────────────────────

    /// \brief Try to enqueue a dispatch envelope.
    ///
    /// Only succeeds when state == Running. On success, signals the dispatch
    /// notifier.
    ///
    /// \param[in] envelope The dispatch envelope to enqueue.
    /// \return true if enqueued.
    [[nodiscard]] bool try_push_dispatch(const PythonDispatchPtr& envelope) noexcept;

    /// \brief Try to enqueue a command from the Python interpreter.
    ///
    /// Only succeeds when state == Running. On success, invokes the wake port.
    ///
    /// \param[in] command The command to enqueue.
    /// \return true if enqueued.
    [[nodiscard]] bool try_push_command(const PythonCommandPtr& command) noexcept;

    /// \brief Try to enqueue a completion for the Python interpreter.
    ///
    /// Only succeeds when state == Running. For completions with a non-zero
    /// actor ID and generation, calls generation_matches() before signalling
    /// the completion notifier. A mismatch increments stale_completion_rejected
    /// and returns false without signalling.
    ///
    /// \param[in] completion The completion to enqueue.
    /// \return true if enqueued and (if applicable) generation-valid.
    [[nodiscard]] bool
    try_push_completion(const PythonCompletionPtr& completion) noexcept;

    // ── Notifier file descriptors ──────────────────────────────────────────

    /// \brief Read-end file descriptor for the dispatch notifier.
    ///
    /// \return A non-negative fd, or -1 if the notifier is closed.
    [[nodiscard]] int dispatch_read_fd() const noexcept;

    /// \brief Read-end file descriptor for the completion notifier.
    ///
    /// \return A non-negative fd, or -1 if the notifier is closed.
    [[nodiscard]] int completion_read_fd() const noexcept;

    /// \brief Drain pending dispatch wakeup tokens.
    ///
    /// \return The number of wakeup signals drained.
    [[nodiscard]] uint64_t drain_dispatch_notification() noexcept;

    /// \brief Drain pending completion wakeup tokens.
    ///
    /// \return The number of wakeup signals drained.
    [[nodiscard]] uint64_t drain_completion_notification() noexcept;

    // ── Queue draining (delegates to PythonRuntimeQueues) ───────────────────

    /// \brief Drain up to \p max_items dispatch envelopes.
    ///
    /// \tparam Fn Callable invoked as fn(const PythonDispatchEnvelope&).
    /// \param[in] max_items Maximum number of envelopes to drain.
    /// \param[in] callback Invoked for each drained envelope.
    /// \return The number of envelopes actually drained.
    template <typename Fn>
    size_t drain_dispatch(size_t max_items, Fn&& callback) {
        return queues_->drain_dispatch(max_items, std::forward<Fn>(callback));
    }

    /// \brief Drain up to \p max_items commands.
    ///
    /// \tparam Fn Callable invoked as fn(const PythonCommand&).
    /// \param[in] max_items Maximum number of commands to drain.
    /// \param[in] callback Invoked for each drained command.
    /// \return The number of commands actually drained.
    template <typename Fn>
    size_t drain_commands(size_t max_items, Fn&& callback) {
        return queues_->drain_commands(max_items, std::forward<Fn>(callback));
    }

    /// \brief Drain up to \p max_items completions.
    ///
    /// \tparam Fn Callable invoked as fn(const PythonCompletion&).
    /// \param[in] max_items Maximum number of completions to drain.
    /// \param[in] callback Invoked for each drained completion.
    /// \return The number of completions actually drained.
    template <typename Fn>
    size_t drain_completions(size_t max_items, Fn&& callback) {
        return queues_->drain_completions(max_items, std::forward<Fn>(callback));
    }

    // ── Observers ──────────────────────────────────────────────────────────

    /// \brief Return the runtime configuration.
    ///
    /// \return A const reference to the configuration.
    [[nodiscard]] const PythonRuntimeConfig& config() const noexcept;

    /// \brief Return a point-in-time snapshot of runtime state.
    ///
    /// \return A PythonRuntimeSnapshot with current values.
    [[nodiscard]] PythonRuntimeSnapshot snapshot() const noexcept;

  private:
    friend class PythonActorLease;

    explicit PythonRuntime(PythonRuntimeConfig config) noexcept;

    /// \brief Bind an ActorId to a generation. Called by PythonActorLease.
    [[nodiscard]] bool bind_actor(ActorId actor_id, uint64_t generation) noexcept;

    /// \brief Release an actor binding. Called by PythonActorLease.
    void
    release_actor(ActorId actor_id, uint64_t generation, bool was_bound) noexcept;

    /// \brief Transition to a new state, returning the previous state.
    PythonRuntimeState transition_state(PythonRuntimeState target) noexcept;

    PythonRuntimeConfig config_;

    std::unique_ptr<PythonRuntimeQueues> queues_;

    std::unique_ptr<NativeNotifier> dispatch_notifier_;
    std::unique_ptr<NativeNotifier> completion_notifier_;

    GatewayWakePort wake_port_;

    mutable std::mutex actor_mutex_;
    std::unordered_map<ActorId, uint64_t> actor_generations_;
    size_t actor_reservation_count_{0};
    std::atomic<uint64_t> actor_generation_counter_{0};
    std::atomic<uint64_t> stale_completion_rejected_{0};

    std::atomic<PythonRuntimeState> state_{PythonRuntimeState::Created};
    std::atomic<bool> admission_open_{true};
};

} // namespace hpactor::python
