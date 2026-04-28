// Copyright 2026 HPActor Contributors
#pragma once

#include <hpactor/net/async_io_fwd.hpp>

#include <sys/socket.h>
#include <sys/uio.h>

namespace hpactor {
namespace net {

enum class IoEvent : uint32_t {
    Read = 1 << 0,
    Write = 1 << 1,
};

class IReactorBackend {
public:
    virtual ~IReactorBackend() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;

    virtual bool add_fd(int fd, IoEvent events) = 0;
    virtual bool update_fd(int fd, IoEvent events) = 0;
    virtual bool remove_fd(int fd) = 0;

    virtual int register_buffer(const void* addr, size_t len) = 0;
    virtual bool unregister_buffer(int buffer_id) = 0;

    virtual uint64_t run_after(ActorId actor, int delay_ms) = 0;
    virtual uint64_t run_every(ActorId actor, int interval_ms) = 0;
    virtual void cancel_timer(uint64_t handle) = 0;

    virtual int wait(int timeout_ms) = 0;
    virtual void process_events() = 0;

    // Proactor methods - used directly by connection code
    virtual void async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;
    virtual void async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) = 0;
    virtual void async_accept(int fd, ActorId actor) = 0;
    virtual void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                               ActorId actor) = 0;
    virtual void async_sendto(int fd, const iovec* bufs, int buf_count,
                               const sockaddr* addr, socklen_t addrlen,
                               ActorId actor, uint32_t op_type) = 0;
    virtual void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) = 0;

    // Read handler management — reactor backends read data in wait() and
    // dispatch via callback. Proactor backends use async_recv completions.
    virtual void set_read_handler(int fd, read_callback handler) = 0;
    virtual void clear_read_handler(int fd) = 0;

    // Returns true if this backend supports calling read handlers directly
    // from wait(). Reactor backends (epoll, kqueue) return true.
    // Proactor backends (io_uring, GCD) return false.
    virtual bool supports_read_handler() const = 0;
};

} // namespace net
} // namespace hpactor
