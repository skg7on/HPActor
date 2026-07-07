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

#include <hpactor/adt/mpsc_ring_buffer.hpp>
#include <hpactor/python/python_bridge_types.hpp>
#include <hpactor/python/python_runtime_config.hpp>
#include <hpactor/python/python_runtime_snapshot.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace hpactor::python {

/// \brief Three-queue bridge between the native actor runtime and the Python
///        interpreter thread.
///
/// Each queue is a lock-free MPSC ring buffer carrying shared_ptr to const
/// value types. The dispatch queue is consumed by the Python bridge actor;
/// the command queue is consumed by the native gateway; the completion queue
/// is consumed by the Python interpreter thread.
///
/// Rejection counters are atomic so the producer (any thread) can increment
/// them on a full push without synchronizing with the consumer.
class PythonRuntimeQueues final {
  public:
    /// \brief Construct all three queues from a validated configuration.
    ///
    /// \param[in] cfg A validated PythonRuntimeConfig. Behavior is undefined if
    ///                cfg.validate() != PythonConfigError::None.
    explicit PythonRuntimeQueues(const PythonRuntimeConfig& cfg);

    // ── Dispatch queue (native → Python handler) ────────────────────────

    /// \brief Try to enqueue a dispatch envelope for a Python-bound actor.
    ///
    /// \param[in] envelope The dispatch envelope to enqueue.
    /// \return true if enqueued, false if the dispatch queue is full.
    bool try_push_dispatch(PythonDispatchPtr envelope) noexcept;

    /// \brief Drain up to \p max_items dispatch envelopes.
    ///
    /// \tparam Fn Callable invoked as fn(const PythonDispatchEnvelope&).
    /// \param[in] max_items Maximum number of envelopes to drain.
    /// \param[in] callback Invoked for each drained envelope.
    /// \return The number of envelopes actually drained.
    template <typename Fn>
    size_t drain_dispatch(size_t max_items, Fn&& callback) {
        return dispatch_queue_.drain_up_to(
            max_items, [&callback](const PythonDispatchPtr& ptr) {
                if (ptr) {
                    callback(*ptr);
                }
            });
    }

    // ── Command queue (Python interpreter → native gateway) ─────────────

    /// \brief Try to enqueue a command from the Python interpreter.
    ///
    /// \param[in] cmd The command to enqueue.
    /// \return true if enqueued, false if the command queue is full.
    bool try_push_command(PythonCommandPtr cmd) noexcept;

    /// \brief Drain up to \p max_items commands.
    ///
    /// \tparam Fn Callable invoked as fn(const PythonCommand&).
    /// \param[in] max_items Maximum number of commands to drain.
    /// \param[in] callback Invoked for each drained command.
    /// \return The number of commands actually drained.
    template <typename Fn>
    size_t drain_commands(size_t max_items, Fn&& callback) {
        return command_queue_.drain_up_to(
            max_items, [&callback](const PythonCommandPtr& ptr) {
                if (ptr) {
                    callback(*ptr);
                }
            });
    }

    // ── Completion queue (native gateway → Python interpreter) ──────────

    /// \brief Try to enqueue a completion for the Python interpreter.
    ///
    /// \param[in] completion The completion to enqueue.
    /// \return true if enqueued, false if the completion queue is full.
    bool try_push_completion(PythonCompletionPtr completion) noexcept;

    /// \brief Drain up to \p max_items completions.
    ///
    /// \tparam Fn Callable invoked as fn(const PythonCompletion&).
    /// \param[in] max_items Maximum number of completions to drain.
    /// \param[in] callback Invoked for each drained completion.
    /// \return The number of completions actually drained.
    template <typename Fn>
    size_t drain_completions(size_t max_items, Fn&& callback) {
        return completion_queue_.drain_up_to(
            max_items, [&callback](const PythonCompletionPtr& ptr) {
                if (ptr) {
                    callback(*ptr);
                }
            });
    }

    /// \brief Return a point-in-time snapshot of queue depths and rejection
    ///        counters.
    ///
    /// \return A PythonQueueSnapshot with current values.
    PythonQueueSnapshot snapshot() const noexcept;

  private:
    using DispatchQueue = adt::DynamicMpscRingBuffer<PythonDispatchPtr>;
    using CommandQueue = adt::DynamicMpscRingBuffer<PythonCommandPtr>;
    using CompletionQueue = adt::DynamicMpscRingBuffer<PythonCompletionPtr>;

    DispatchQueue dispatch_queue_;
    CommandQueue command_queue_;
    CompletionQueue completion_queue_;

    std::atomic<uint64_t> dispatch_rejected_{0};
    std::atomic<uint64_t> command_rejected_{0};
    std::atomic<uint64_t> completion_rejected_{0};
};

} // namespace hpactor::python
