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

#include <hpactor/hpactor_config.hpp>
#include <hpactor/coroutine/coroutine_task.hpp>
#include <hpactor/types/types.hpp>

#include <utility>

namespace hpactor::sched {

#if HPACTOR_SUPPORT_COROUTINES

// ActorCoroutine: owns a coroutine handle for an actor.
// Produced by EventBasedActor::act() and consumed by
// HybridScheduler::execute_actor().
class ActorCoroutine {
  public:
    ActorCoroutine() noexcept = default;

    explicit ActorCoroutine(CoroutineTask&& task, ActorId actor_id) noexcept
        : task_(std::move(task)), actor_id_(actor_id) {}

    // Move-only
    ActorCoroutine(ActorCoroutine&& other) noexcept
        : task_(std::move(other.task_)), actor_id_(other.actor_id_) {
        // other.task_ is now in moved-from state (handle_=nullptr)
        // CoroutineTask's moved-from state is safe to destroy
    }

    ActorCoroutine& operator=(ActorCoroutine&& other) noexcept {
        if (this != &other) {
            // Use swap to transfer ownership; old task destroyed via temp's
            // destructor
            CoroutineTask old_task(std::move(task_));
            task_ = std::move(other.task_);
            actor_id_ = other.actor_id_;
            // old_task goes out of scope and destroys the previous task if any
        }
        return *this;
    }

    ActorCoroutine(const ActorCoroutine&) = delete;
    ActorCoroutine& operator=(const ActorCoroutine&) = delete;

    ~ActorCoroutine() = default;

    explicit operator bool() const noexcept {
        return static_cast<bool>(task_);
    }

    CoroutineTask& task() {
        return task_;
    }
    const CoroutineTask& task() const {
        return task_;
    }

    ActorId actor_id() const {
        return actor_id_;
    }

    // Resume the coroutine. Must be called on the owning worker thread.
    void resume() {
        if (task_ && !task_.done()) {
            task_.resume();
        }
    }

    bool done() const {
        return !task_ || task_.done();
    }

    // Access the promise for state inspection
    CoroutinePromise& promise() {
        return task_.handle().promise();
    }
    const CoroutinePromise& promise() const {
        return task_.handle().promise();
    }

  private:
    CoroutineTask task_;
    ActorId actor_id_;
};

#endif // HPACTOR_SUPPORT_COROUTINES

} // namespace hpactor::sched
