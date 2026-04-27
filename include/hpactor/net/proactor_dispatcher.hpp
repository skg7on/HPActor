// Copyright 2026 HPActor Contributors
#pragma once

#include <hpactor/net/async_io_fwd.hpp>
#include <hpactor/net/reactor_backend.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {

class ActorSystem;

namespace net {

// ProactorDispatcher - dispatches proactor (async I/O) completion events
// to the appropriate actor mailbox or timer system
class ProactorDispatcher {
public:
    void on_completion(OpCompletion completion) {
        switch (completion.type) {
            case OpType::TimerFired:
                // Deliver to timer system
                break;
            case OpType::Send:
            case OpType::Recv:
            case OpType::Accept:
            case OpType::Connect:
            case OpType::RecvFrom:
            case OpType::SendTo:
                // Deliver to actor mailbox
                break;
        }
    }

    void set_actor_system(ActorSystem* system) {
        system_ = system;
    }

private:
    ActorSystem* system_ = nullptr;
};

} // namespace net
} // namespace hpactor
