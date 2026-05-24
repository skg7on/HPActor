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
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <hpactor/actor/abstract_actor.hpp>
#include <memory>
#include <mutex>
#include <queue>

namespace hpactor {

// Forward declaration - full definition in mutex_mailbox.hpp
template <typename T> class MutexMailbox;

/// \brief Type-erased mailbox interface for actor message delivery.
///
/// Implementations provide the hot-path \c push and consumer-side \c pop
/// operations. The interface is templated on the message type to avoid
/// virtual dispatch overhead on the message payload.
///
/// \tparam T Message type stored in the mailbox.
template <typename T> class IMailbox {
  public:
    virtual ~IMailbox() = default;

    /// \brief Push a message onto the mailbox.
    ///
    /// Hot path — marked \c noexcept for real-time scheduling guarantees.
    /// \param[in,out] msg Message to enqueue, moved into the mailbox.
    virtual void push(T&& msg) noexcept = 0;

    /// \brief Non-blocking attempt to pop a message.
    ///
    /// \param[out] out Set to the dequeued message on success.
    /// \return \c true if a message was available, \c false if the mailbox
    ///         was empty.
    /// \note Thread safety: Safe to call from any thread.
    virtual bool try_pop(T& out) noexcept = 0;

    /// \brief Blocking pop — waits until a message is available.
    ///
    /// Used for the actor message processing loop.
    /// \param[out] out Set to the dequeued message.
    /// \return \c true once a message is retrieved.
    /// \note Thread safety: Blocks the calling thread. Call only from
    ///       dedicated-thread actors or worker threads that allow blocking.
    virtual bool pop(T& out) = 0;

    /// \brief Approximate count of messages currently in the mailbox.
    virtual size_t size() const = 0;

    /// \brief Returns \c true if the mailbox contains no messages.
    virtual bool empty() const = 0;
};

/// \brief Backend selector for mailbox factory functions.
enum class MailboxType {
    Mutex,    ///< Thread-safe, slower — default implementation.
    LockFree, ///< Lock-free MPSC queue (reserved for future use).
};

/// \brief Factory function for creating mailbox instances.
///
/// \tparam T Message type.
/// \tparam Type Backend selection (defaults to \c Mutex).
/// \return A heap-allocated mailbox implementing \c IMailbox<T>.
template <typename T, MailboxType Type = MailboxType::Mutex>
std::unique_ptr<IMailbox<T>> create_mailbox() {
    if constexpr (Type == MailboxType::Mutex) {
        return std::make_unique<MutexMailbox<T>>();
    }
}

/// \brief Alias for \c AbstractActor — base class for all actors.
using ActorBase = AbstractActor;

/// \brief Mutex-based mailbox with owner association.
///
/// Wraps a \c std::queue with mutex + condition variable synchronization.
/// Supports blocking \c pop_with_timeout for bounded wait.
///
/// \tparam T Message type stored in the mailbox.
/// \note Thread safety: All public methods acquire the internal mutex.
///       Suitable for dedicated-thread and blocking actors.
template <typename T> class ActorMailbox : public IMailbox<T> {
  public:
    ActorMailbox() = default;

    void push(T&& msg) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(msg));
        cv_.notify_one();
    }

    bool try_pop(T& out) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool pop(T& out) override {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty(); });
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /// \brief Blocking pop with a deadline.
    ///
    /// \param[out] out Set to the dequeued message on success.
    /// \param[in] timeout Maximum time to wait.
    /// \return \c true if a message was retrieved before the timeout,
    ///         \c false if the deadline expired.
    /// \note Thread safety: Blocks the calling thread for up to \p timeout.
    bool pop_with_timeout(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        bool result =
            cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); });
        if (result) {
            out = std::move(queue_.front());
            queue_.pop();
        }
        return result;
    }

    /// \brief Associate this mailbox with an owning actor.
    ///
    /// Called by \c ActorSystem during spawn.
    /// \param[in] owner Pointer to the owning \c AbstractActor.
    void set_owner(ActorBase* owner) {
        owner_ = owner;
    }

  private:
    mutable std::mutex mutex_;
    std::queue<T> queue_;
    std::condition_variable cv_;
    ActorBase* owner_ = nullptr;
};

} // namespace hpactor
