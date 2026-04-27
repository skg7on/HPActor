// Copyright 2026 HPActor Contributors
#pragma once

#include <hpactor/net/async_io_fwd.hpp>

namespace hpactor {
namespace net {

class IProactorBackend {
public:
    virtual ~IProactorBackend() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual void async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;
    virtual void async_accept(int fd, ActorId actor) = 0;
    virtual void async_connect(int fd, const sockaddr* addr,
                               socklen_t addrlen, ActorId actor) = 0;

    virtual void async_send_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) = 0;
    virtual void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) = 0;
    virtual void async_sendto(int fd, const iovec* bufs, int buf_count,
                               const sockaddr* addr, socklen_t addrlen,
                               ActorId actor, uint32_t op_type) = 0;

    virtual uint64_t run_after(ActorId actor, int delay_ms) = 0;
    virtual uint64_t run_every(ActorId actor, int interval_ms) = 0;
    virtual void cancel_timer(uint64_t handle) = 0;

    virtual int wait(int timeout_ms) = 0;
    virtual void process_completions() = 0;

    virtual void deliver_completion(OpCompletion completion) = 0;
};

} // namespace net
} // namespace hpactor
