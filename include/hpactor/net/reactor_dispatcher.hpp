// Copyright 2026 HPActor Contributors
#pragma once

#include <hpactor/net/async_io_fwd.hpp>
#include <hpactor/net/reactor_backend.hpp>
#include <hpactor/types/types.hpp>

#include <unordered_map>

namespace hpactor {

class ActorSystem;

namespace net {

// ReactorDispatcher - dispatches reactor (sync I/O) readiness events
// to the appropriate actor mailbox
class ReactorDispatcher {
public:
    void on_readiness(int fd, IoEvent events) {
        ActorId actor = fd_to_actor_[fd];
        // Issue sync I/O, deliver to actor mailbox
    }

    void register_fd(int fd, ActorId actor) {
        fd_to_actor_[fd] = actor;
    }

    void unregister_fd(int fd) {
        fd_to_actor_.erase(fd);
    }

private:
    std::unordered_map<int, ActorId> fd_to_actor_;
};

} // namespace net
} // namespace hpactor
