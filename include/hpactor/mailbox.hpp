#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/message.hpp>
#include <memory>
#include <mutex>
#include <queue>

namespace hpactor {

// Forward declaration - full definition in mutex_mailbox.hpp
template <typename T> class MutexMailbox;

template <typename T> class IMailbox {
  public:
    virtual ~IMailbox() = default;

    // Hot path - marked noexcept for real-time guarantees
    virtual void push(Message<T>&& msg) noexcept = 0;
    virtual bool try_pop(Message<T>& out) noexcept = 0;

    // Blocking pop - may block, used for actor message processing loop
    virtual bool pop(Message<T>& out) = 0;

    virtual size_t size() const = 0;
    virtual bool empty() const = 0;
};

// Default implementation selector
enum class MailboxType {
    Mutex,    // Thread-safe, slower
    LockFree, // TODO: Implementation TBD after stress tests pass
};

// Factory function
template <typename T, MailboxType Type = MailboxType::Mutex>
std::unique_ptr<IMailbox<T>> create_mailbox() {
    if constexpr (Type == MailboxType::Mutex) {
        return std::make_unique<MutexMailbox<T>>();
    }
    // LockFree would be added here later
}

// ActorBase - alias for AbstractActor (base class for all actors)
using ActorBase = AbstractActor;

// ActorMailbox - mailbox with owner association for actor-specific features
template <typename T> class ActorMailbox : public IMailbox<T> {
  public:
    ActorMailbox() = default;

    // Hot path - marked noexcept for real-time guarantees
    void push(Message<T>&& msg) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(msg));
        cv_.notify_one();
    }

    // Hot path - marked noexcept for real-time guarantees
    bool try_pop(Message<T>& out) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool pop(Message<T>& out) override {
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

    bool pop_with_timeout(Message<T>& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        bool result =
            cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); });
        if (result) {
            out = std::move(queue_.front());
            queue_.pop();
        }
        return result;
    }

    void set_owner(ActorBase* owner) {
        owner_ = owner;
    }

  private:
    mutable std::mutex mutex_;
    std::queue<Message<T>> queue_;
    std::condition_variable cv_;
    ActorBase* owner_ = nullptr;
};

} // namespace hpactor