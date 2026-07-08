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

#include <hpactor/msg/completion_port.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace hpactor {

/// \brief Handle to a pending request with optional timeout.
///
/// Move-only. The caller can block on get(), check ready() without blocking,
/// or cancel the pending request.
///
/// \tparam T The result type (e.g., StreamBuffer, ActorRef).
///
/// \note Thread safety: get() blocks the calling thread. ready() and cancel()
///       are safe from any thread.
///
/// \note The caller must not move the handle while a concurrent resolve()
///       or cancel() is in-flight on a different thread. The move constructor
///       does not acquire the internal mutex.
///
/// \note Multiple RequestHandle instances can share the same underlying State
///       via the shared-state constructor. This enables the AskManager pattern
///       where one handle is stored for timeout/response handling while another
///       is returned to the caller.
template <typename T> class RequestHandle {
  public:
    /// \brief Shared mutable state for a request handle.
    ///
    /// Multiple RequestHandle instances can reference the same State,
    /// enabling the AskManager pattern where one handle is stored for
    /// timeout/response handling while another is returned to the caller.
    struct State {
        std::optional<result<T>> inner;
        std::atomic<bool> ready{false};
        bool cancelled = false;
        std::chrono::steady_clock::time_point deadline{
            std::chrono::steady_clock::time_point::max()};
        MessageId msg_id{};
        CompletionPort<result<T>> fixed_completion;
        bool completion_consumed{false};
        std::mutex mutex;
        std::condition_variable cv;
    };

    RequestHandle() : state_(std::make_shared<State>()) {}

    RequestHandle(std::chrono::steady_clock::time_point deadline, MessageId msg_id)
        : state_(std::make_shared<State>()) {
        state_->deadline = deadline;
        state_->msg_id = msg_id;
    }

    /// \brief Construct a handle that shares state with other handles.
    ///
    /// Used by AskManager to create multiple RequestHandle instances
    /// that share the same underlying result, mutex, and condition variable.
    ///
    /// \param[in] s Shared state to reference.
    explicit RequestHandle(std::shared_ptr<State> s) : state_(std::move(s)) {}

    /// \brief Move constructor — transfers shared-state ownership.
    ///
    /// The moved-from handle holds a null state pointer and must not be
    /// used except for destruction or move-assignment.
    RequestHandle(RequestHandle&& other) noexcept = default;

    /// \brief Move assignment — transfers shared-state ownership.
    ///
    /// The moved-from handle holds a null state pointer and must not be
    /// used except for destruction or move-assignment.
    RequestHandle& operator=(RequestHandle&& other) noexcept = default;

    RequestHandle(const RequestHandle&) = delete;
    RequestHandle& operator=(const RequestHandle&) = delete;

    ~RequestHandle() = default;

    /// \brief Block until response arrives or the request is resolved.
    ///
    /// \return The response value or an error.
    result<T> get() {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (state_->cancelled) {
            return result<T>::make(error(errors::cancelled, "request "
                                                            "cancelled"));
        }
        state_->cv.wait(lock, [this] {
            return state_->ready.load(std::memory_order_acquire);
        });
        if (state_->cancelled) {
            return result<T>::make(error(errors::cancelled, "request "
                                                            "cancelled"));
        }
        return std::move(*state_->inner);
    }

    /// \brief Non-blocking readiness check.
    ///
    /// \return true if the response has arrived or the request has been
    ///         resolved.
    bool ready() const noexcept {
        return state_->ready.load(std::memory_order_acquire);
    }

    /// \brief Cancel the pending request.
    ///
    /// Any blocked get() returns errors::cancelled. If the handle has already
    /// been resolved (via resolve() or resolve_error()), cancel() is a no-op
    /// so that the original result is preserved.
    void cancel() {
        if (state_->ready.load(std::memory_order_acquire)) {
            return; // Already resolved — no-op
        }
        CompletionPort<result<T>> port;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->cancelled) {
                return;
            }
            state_->cancelled = true;
            port = std::move(state_->fixed_completion);
        }
        state_->ready.store(true, std::memory_order_release);
        state_->cv.notify_all();
        if (port) {
            port(result<T>::make(error(errors::cancelled, "request cancelled")));
        }
    }

    /// \brief The message_id used for this request (for tracing correlation).
    ///
    /// \return The correlation message ID.
    MessageId message_id() const noexcept {
        return state_->msg_id;
    }

    /// \brief The deadline after which this request is considered expired.
    ///
    /// \return Absolute deadline in steady_clock time.
    std::chrono::steady_clock::time_point deadline() const noexcept {
        return state_->deadline;
    }

    /// \brief Register a fixed function-pointer completion port.
    ///
    /// If the handle is already resolved, the port is invoked immediately
    /// on the calling thread and the method returns \c true. Otherwise the
    /// port is stored and invoked exactly once when the handle is resolved
    /// or cancelled. Returns \c false if a callback was already installed
    /// or the result was already consumed.
    ///
    /// \param[in] port The completion port to register.
    /// \return true if the port was installed or immediately invoked.
    [[nodiscard]] bool on_complete(CompletionPort<result<T>> port) {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (state_->completion_consumed) {
            return false;
        }
        if (state_->inner.has_value() || state_->cancelled) {
            // Already resolved — invoke immediately outside the mutex.
            auto inner = std::move(*state_->inner);
            bool was_cancelled = state_->cancelled;
            state_->completion_consumed = true;
            lock.unlock();
            result<T> r =
                was_cancelled
                    ? result<T>::make(error(errors::cancelled, "request cancelled"))
                    : std::move(inner);
            port(std::move(r));
            return true;
        }
        // Store for later invocation.
        state_->fixed_completion = std::move(port);
        state_->completion_consumed = true;
        return true;
    }

    // ── Internal (called by AskManager / RpcChannel) ────────────────────

    /// \brief Resolve the handle with a successful result.
    void resolve(result<T> value) {
        CompletionPort<result<T>> port;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->inner.has_value() || state_->cancelled) {
                return; // already resolved
            }
            state_->inner = std::move(value);
            port = std::move(state_->fixed_completion);
        }
        state_->ready.store(true, std::memory_order_release);
        state_->cv.notify_all();
        if (port) {
            port(std::move(*state_->inner));
        }
    }

    /// \brief Resolve the handle with an error.
    void resolve_error(error err) {
        resolve(result<T>::make(std::move(err)));
    }

  private:
    std::shared_ptr<State> state_;
};

} // namespace hpactor
