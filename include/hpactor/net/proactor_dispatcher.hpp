// Copyright 2026 HPActor Contributors
#pragma once

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/async_io_fwd.hpp>
#include <hpactor/net/reactor_backend.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <unordered_map>

namespace hpactor {

namespace net {

// ProactorDispatcher - dispatches proactor (async I/O) completion events
// to the appropriate actor mailbox or timer system.
//
// In proactor mode, async operations complete asynchronously and the
// dispatcher routes the completion to the correct destination:
//   - TimerFired events -> timer handler callback
//   - I/O completions (Send/Recv/Accept/Connect/RecvFrom/SendTo) -> ActorSystem
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
