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
template <typename T> class RequestHandle {
  public:
    RequestHandle()
        : mutex_(std::make_unique<std::mutex>()),
          cv_(std::make_unique<std::condition_variable>()) {}

    RequestHandle(std::chrono::steady_clock::time_point deadline, MessageId msg_id)
        : deadline_(deadline), msg_id_(msg_id),
          mutex_(std::make_unique<std::mutex>()),
          cv_(std::make_unique<std::condition_variable>()) {}

    RequestHandle(RequestHandle&& other) noexcept
        : inner_(std::move(other.inner_)),
          ready_(other.ready_.load(std::memory_order_acquire)),
          cancelled_(other.cancelled_), deadline_(other.deadline_),
          msg_id_(other.msg_id_), mutex_(std::move(other.mutex_)),
          cv_(std::move(other.cv_)) {}

    RequestHandle& operator=(RequestHandle&& other) noexcept {
        if (this != &other) {
            inner_ = std::move(other.inner_);
            ready_.store(other.ready_.load(std::memory_order_acquire),
                         std::memory_order_release);
            cancelled_ = other.cancelled_;
            deadline_ = other.deadline_;
            msg_id_ = other.msg_id_;
            mutex_ = std::move(other.mutex_);
            cv_ = std::move(other.cv_);
        }
        return *this;
    }

    RequestHandle(const RequestHandle&) = delete;
    RequestHandle& operator=(const RequestHandle&) = delete;

    ~RequestHandle() = default;

    /// \brief Block until response arrives or the request is resolved.
    ///
    /// \return The response value or an error.
    result<T> get() {
        std::unique_lock<std::mutex> lock(*mutex_);
        if (cancelled_) {
            return result<T>::make(error(errors::cancelled, "request "
                                                            "cancelled"));
        }
        cv_->wait(lock, [this] { return ready_.load(std::memory_order_acquire); });
        if (cancelled_) {
            return result<T>::make(error(errors::cancelled, "request "
                                                            "cancelled"));
        }
        return std::move(*inner_);
    }

    /// \brief Non-blocking readiness check.
    ///
    /// \return true if the response has arrived or the request has been
    ///         resolved.
    bool ready() const noexcept {
        return ready_.load(std::memory_order_acquire);
    }

    /// \brief Cancel the pending request.
    ///
    /// Any blocked get() returns errors::cancelled.
    void cancel() {
        {
            std::lock_guard<std::mutex> lock(*mutex_);
            cancelled_ = true;
        }
        ready_.store(true, std::memory_order_release);
        cv_->notify_all();
    }

    /// \brief The message_id used for this request (for tracing correlation).
    ///
    /// \return The correlation message ID.
    MessageId message_id() const noexcept {
        return msg_id_;
    }

    /// \brief The deadline after which this request is considered expired.
    ///
    /// \return Absolute deadline in steady_clock time.
    std::chrono::steady_clock::time_point deadline() const noexcept {
        return deadline_;
    }

    // ── Internal (called by AskManager / RpcChannel) ────────────────────

    /// \brief Resolve the handle with a successful result.
    void resolve(result<T> value) {
        {
            std::lock_guard<std::mutex> lock(*mutex_);
            inner_ = std::move(value);
        }
        ready_.store(true, std::memory_order_release);
        cv_->notify_all();
    }

    /// \brief Resolve the handle with an error.
    void resolve_error(error err) {
        resolve(result<T>::make(std::move(err)));
    }

  private:
    std::optional<result<T>> inner_;
    std::atomic<bool> ready_{false};
    bool cancelled_ = false;
    std::chrono::steady_clock::time_point deadline_{
        std::chrono::steady_clock::time_point::max()};
    MessageId msg_id_{};
    std::unique_ptr<std::mutex> mutex_;
    std::unique_ptr<std::condition_variable> cv_;
};

} // namespace hpactor
