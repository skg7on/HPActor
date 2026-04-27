// Copyright 2026 HPActor Contributors
#pragma once

#include <hpactor/net/async_io_fwd.hpp>

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

    virtual int wait(int timeout_ms) = 0;
    virtual void process_events() = 0;

    virtual int register_buffer(const void* addr, size_t len) = 0;
    virtual bool unregister_buffer(int buffer_id) = 0;
};

} // namespace net
} // namespace hpactor
