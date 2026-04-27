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
#include <hpactor/core/mailbox.hpp>
#include <memory>
#include <mutex>
#include <queue>

namespace hpactor {

template <typename T> class MutexMailbox : public IMailbox<T> {
  public:
    MutexMailbox() = default;

    void push(T&& msg) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(msg));
    }

    bool pop(T& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool try_pop(T& out) noexcept override {
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
    std::queue<T> queue_;
};

} // namespace hpactor
