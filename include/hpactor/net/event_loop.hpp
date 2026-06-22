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

#include <hpactor/net/reactor_backend.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace hpactor {

class ActorSystem;

namespace net {

class ProactorDispatcher;

/// \brief Unified async I/O event loop over platform-specific backends.
///
/// Provides a common interface over these platform-specific async I/O
/// backends:
/// - io_uring on Linux (preferred), epoll fallback
/// - libdispatch (GCD) on macOS (preferred), kqueue fallback
///
/// Manages file descriptor registration, read/write handlers, timer
/// scheduling, and completion dispatch. Must be explicitly started via
/// \c run() after construction.
///
/// Usage:
/// \code{.cpp}
///   EventLoop loop;  // Creates backend but does not start
///   loop.run();       // Explicitly start processing
///   // ... event loop runs ...
///   loop.stop();      // Stop when done
/// \endcode
///
/// \note Thread safety: All public methods must be called from the event
///       loop thread. The backend pointer is safe to access from any
///       thread for async I/O submission only.
class EventLoop {
  public:
    EventLoop();
    ~EventLoop();

    /// \name Non-copyable, non-movable
    /// @{
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;
    /// @}

    /// \brief Start the backend and begin processing events.
    ///
    /// Must be called explicitly after construction.
    /// \return \c true on success.
    bool run();

    /// \brief Stop the backend and halt event processing.
    void stop();

    /// \brief Check whether the backend is running.
    ///
    /// \return \c true if \c run() has been called and \c stop() has not.
    bool is_running() const {
        return running_.load();
    }

    /// \brief Return the backend name for diagnostics.
    ///
    /// \return Platform-specific backend name (e.g., \c "iouring",
    ///         \c "epoll", \c "kqueue", \c "gcd").
    const char* backend_name() const;

    // ── File descriptor registration ───────────────────────────────────

    /// \brief I/O event interest flags for file descriptor registration.
    enum class Event : uint32_t {
        Read = 1,          ///< Interested in readability.
        Write = 2,         ///< Interested in writability.
        EdgeTriggered = 4, ///< Use edge-triggered notification.
    };

    /// \brief Register a file descriptor with the given interest set.
    ///
    /// \param[in] fd File descriptor to monitor.
    /// \param[in] events Bitmask of \c Event flags.
    /// \return \c true on success.
    bool add_fd(int fd, Event events);

    /// \brief Update the interest set for a previously registered fd.
    ///
    /// \param[in] fd File descriptor to update.
    /// \param[in] events New bitmask of \c Event flags.
    /// \return \c true on success.
    bool update_fd(int fd, Event events);

    /// \brief Remove a file descriptor from the interest set.
    ///
    /// \param[in] fd File descriptor to deregister.
    /// \return \c true on success.
    bool remove_fd(int fd);

    // ── Read/write handler management ──────────────────────────────────

    /// \brief Read handler callback type (re-exported from
    /// \c async_io_fwd.hpp).
    using read_callback = net::read_callback;

    /// \brief Register a read handler for a file descriptor.
    ///
    /// When data arrives, it is delivered to this callback. The
    /// \c EventLoop will automatically issue \c async_recv for the fd
    /// when it becomes readable.
    /// \param[in] fd File descriptor to watch.
    /// \param[in] handler Callback invoked on readability.
    void set_read_handler(int fd, read_callback handler);

    /// \brief Remove the read handler for a file descriptor.
    ///
    /// \param[in] fd File descriptor whose handler should be removed.
    void clear_read_handler(int fd);

    /// \brief Query whether the backend supports read handler dispatch
    /// from \c wait().
    ///
    /// \return \c true for reactor backends (epoll, kqueue),
    ///         \c false for proactor backends (io_uring, GCD).
    bool supports_read_handler() const;

    /// \brief Register a write handler for a file descriptor.
    ///
    /// When the fd becomes writable, the callback is invoked. Used for
    /// non-blocking connect completion.
    /// \param[in] fd File descriptor to watch for writability.
    /// \param[in] handler Callback invoked on writability.
    void set_write_handler(int fd, write_callback handler);

    /// \brief Remove the write handler for a file descriptor.
    ///
    /// \param[in] fd File descriptor whose handler should be removed.
    void clear_write_handler(int fd);

    /// \brief Query whether the backend supports write handler dispatch.
    ///
    /// \return \c true for reactor backends, \c false for proactor
    ///         backends.
    bool supports_write_handler() const;

    // ── Event waiting ──────────────────────────────────────────────────

    /// \brief Wait for events (blocking with timeout).
    ///
    /// \param[in] timeout_ms Maximum wait time in milliseconds.
    /// \return Number of events triggered, 0 on timeout, -1 on error.
    int wait(int timeout_ms);

    /// \brief Check whether a specific event was triggered for an fd.
    ///
    /// \param[in] fd File descriptor to check.
    /// \param[in] event Event flag to test.
    /// \return \c true if the event occurred.
    bool has_event(int fd, Event event) const;

    // ── Timer management ───────────────────────────────────────────────

    /// \brief Timer expiry callback.
    using timer_callback = std::function<void()>;

    /// \brief Schedule a one-shot timer.
    ///
    /// \param[in] callback Callback invoked on expiry.
    /// \param[in] delay_ms Delay in milliseconds.
    /// \return Timer handle for cancellation.
    uint64_t run_after(timer_callback callback, int delay_ms);

    /// \brief Schedule a repeating timer.
    ///
    /// \param[in] callback Callback invoked on each expiry.
    /// \param[in] interval_ms Interval in milliseconds.
    /// \return Timer handle for cancellation.
    uint64_t run_every(timer_callback callback, int interval_ms);

    /// \brief Cancel a scheduled timer.
    ///
    /// \param[in] timer_handle Timer handle from \c run_after() or
    ///            \c run_every().
    void cancel_timer(uint64_t timer_handle);

    // ── Completion dispatch ────────────────────────────────────────────

    /// \brief Process completions from the backend.
    ///
    /// Called by the wait loop after \c wait() returns a positive count.
    void process_completions();

    /// \brief Enqueue a completion for delivery to an actor.
    ///
    /// Called by proactor backends via \c deliver_completion.
    /// \param[in] completion The completion record to deliver.
    void enqueue_completion(OpCompletion completion);

    /// \brief Set the ActorSystem for delivering completions to actors.
    ///
    /// \param[in] actor_system The owning \c ActorSystem.
    void set_actor_system(ActorSystem* actor_system);

    // ── Test support ───────────────────────────────────────────────────

    /// \brief Completion capture callback (test-only API).
    using completion_callback = std::function<void(OpCompletion)>;

    /// \brief Set a callback to capture completions for test verification.
    ///
    /// When set, completions go to this callback instead of the
    /// \c ActorSystem.
    /// \param[in] cb Callback for test verification.
    void set_completion_callback(completion_callback cb) {
        completion_callback_ = std::move(cb);
    }

    /// \brief Access the underlying backend for direct async operations.
    ///
    /// \return Pointer to the platform-specific \c IReactorBackend.
    IReactorBackend* backend() {
        return backend_.get();
    }

  private:
    void deliver_timer_completion(OpCompletion completion);

    std::unique_ptr<IReactorBackend> backend_;
    std::unique_ptr<ProactorDispatcher> proactor_dispatcher_;
    std::atomic<bool> running_{false};
    const char* backend_name_ = "unknown";

    std::unordered_map<uint64_t, timer_callback> timer_callbacks_;
    std::atomic<uint64_t> next_timer_handle_{1};
    std::unordered_map<uint64_t, uint64_t> backend_handle_to_handle_;
    std::unordered_set<uint64_t> repeating_timers_;
    std::unordered_map<int, Event> fd_events_;
    completion_callback completion_callback_;
    ActorSystem* actor_system_ = nullptr;
};

} // namespace net
} // namespace hpactor
