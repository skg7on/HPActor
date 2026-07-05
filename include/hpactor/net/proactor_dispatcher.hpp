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

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/net/async_io_fwd.hpp>
#include <hpactor/net/reactor_backend.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <unordered_map>

namespace hpactor {

namespace net {

/// \brief Dispatches proactor (async I/O) completion events to actor
/// mailboxes or the timer system.
///
/// In proactor mode, async operations complete asynchronously and the
/// dispatcher routes the completion to the correct destination:
/// - \c TimerFired events → timer handler callback.
/// - I/O completions (\c Send, \c Recv, \c Accept, \c Connect,
///   \c RecvFrom, \c SendTo) → \c ActorSystem for actor delivery.
///
/// Also tracks active I/O operations per file descriptor for
/// bookkeeping and supports a test-only completion capture callback.
///
/// \note Thread safety: Called from the event loop thread only.
class ProactorDispatcher {
  public:
    // Timer handler callback - called when a TimerFired completion arrives.
    // The user_data from the completion identifies which timer expired.
    using timer_handler = std::function<void(uint64_t user_data)>;

    void set_timer_handler(timer_handler handler) {
        timer_handler_ = std::move(handler);
    }

    void set_actor_system(ActorSystem* system) {
        system_ = system;
    }

    // Test-only: capture completions for verification.
    // When set, delivery goes to this callback instead of ActorSystem.
    using completion_callback = std::function<void(OpCompletion)>;
    void set_completion_callback(completion_callback cb) {
        completion_callback_ = std::move(cb);
    }

    // Track an active I/O operation for an fd.
    // Used for optional bookkeeping (e.g., to check if an fd has
    // an in-flight operation).
    void register_io(int fd, ActorId actor, OpType type) {
        active_ops_[fd] = {actor, type};
    }

    // Remove tracking for a completed or cancelled operation.
    void unregister_io(int fd) {
        active_ops_.erase(fd);
    }

    // Check if an fd has a tracked operation.
    bool has_active_io(int fd) const {
        return active_ops_.find(fd) != active_ops_.end();
    }

    // Called by the proactor backend when an async operation completes.
    // Routes the completion to the appropriate destination.
    void on_completion(OpCompletion completion) {
        switch (completion.type) {
            case OpType::TimerFired:
                if (timer_handler_) {
                    timer_handler_(completion.user_data);
                }
                break;

            case OpType::Send:
            case OpType::Recv:
            case OpType::Accept:
            case OpType::Connect:
            case OpType::RecvFrom:
            case OpType::SendTo:
                deliver_to_actor(completion);
                break;
        }
    }

  private:
    void deliver_to_actor(const OpCompletion& completion) {
        active_ops_.erase(completion.fd);
        if (completion_callback_) {
            completion_callback_(completion);
        } else if (system_) {
            system_->enqueue_completion(completion);
        }
    }

    struct ActiveOp {
        ActorId actor;
        OpType type;
    };

    ActorSystem* system_ = nullptr;
    timer_handler timer_handler_;
    completion_callback completion_callback_;
    std::unordered_map<int, ActiveOp> active_ops_;
};

} // namespace net
} // namespace hpactor