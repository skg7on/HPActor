#pragma once
#include <hpactor/mailbox.hpp>
#include <hpactor/message.hpp>
#include <memory>
#include <mutex>
#include <queue>

namespace hpactor {

template <typename T> class MutexMailbox : public IMailbox<T> {
  public:
    MutexMailbox() = default;

    // Hot path - marked noexcept for real-time guarantees
    void push(Message<T>&& msg) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(msg));
    }

    bool pop(Message<T>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // Hot path - marked noexcept for real-time guarantees
    bool try_pop(Message<T>& out) noexcept override {
        return pop(out);
    }

    size_t size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

  private:
    mutable std::mutex mutex_;
    std::queue<Message<T>> queue_;
};

} // namespace hpactor