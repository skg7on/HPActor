#include <hpactor/net/gcd_backend.hpp>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#endif

namespace hpactor {
namespace net {

GcdBackend::GcdBackend() = default;

GcdBackend::~GcdBackend() {
    stop();
}

#if defined(__APPLE__)

namespace {

struct AsyncSendContext {
    GcdBackend* self;
    int fd;
    ActorId actor;
    uint32_t op_type;
    std::vector<uint8_t> data;
};

struct AsyncRecvContext {
    GcdBackend* self;
    int fd;
    ActorId actor;
    uint32_t op_type;
    std::vector<iovec> bufs;
    size_t recv_capacity;
};

struct AcceptContext {
    GcdBackend* self;
    int fd;
    ActorId actor;
};

struct ConnectContext {
    GcdBackend* self;
    int fd;
    ActorId actor;
};

void async_send_trampoline(void* ctx) {
    auto* c = static_cast<AsyncSendContext*>(ctx);
    ssize_t n = ::write(c->fd, c->data.data(), c->data.size());
    int result = (n < 0) ? errno : static_cast<int>(n);
    OpCompletion completion{
        .actor = c->actor,
        .type = static_cast<OpType>(c->op_type),
        .fd = c->fd,
        .result = result,
        .user_data = 0,
    };
    c->self->deliver_completion(completion);
}

void async_recv_trampoline(void* ctx) {
    auto* c = static_cast<AsyncRecvContext*>(ctx);
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(c->recv_capacity));
    ssize_t n = ::read(c->fd, buf, c->recv_capacity);
    if (n < 0) {
        OpCompletion completion{
            .actor = c->actor,
            .type = static_cast<OpType>(c->op_type),
            .fd = c->fd,
            .result = errno,
            .user_data = 0,
        };
        c->self->deliver_completion(completion);
    } else {
        size_t total_received = static_cast<size_t>(n);
        size_t offset = 0;
        for (size_t i = 0; i < c->bufs.size() && offset < total_received; ++i) {
            size_t chunk = std::min(c->bufs[i].iov_len, total_received - offset);
            std::memcpy(c->bufs[i].iov_base, buf + offset, chunk);
            offset += chunk;
        }
        OpCompletion completion{
            .actor = c->actor,
            .type = static_cast<OpType>(c->op_type),
            .fd = c->fd,
            .result = static_cast<int>(total_received),
            .user_data = 0,
        };
        c->self->deliver_completion(completion);
    }
    std::free(buf);
}

void accept_trampoline(void* ctx) {
    auto* c = static_cast<AcceptContext*>(ctx);
    int client_fd = ::accept(c->fd, nullptr, nullptr);
    if (client_fd >= 0) {
        OpCompletion completion{
            .actor = c->actor,
            .type = OpType::Accept,
            .fd = client_fd,
            .result = client_fd,
            .user_data = 0,
        };
        c->self->deliver_completion(completion);
    }
}

void connect_trampoline(void* ctx) {
    auto* c = static_cast<ConnectContext*>(ctx);
    int err = 0;
    socklen_t errlen = sizeof(err);
    ::getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    OpCompletion completion{
        .actor = c->actor,
        .type = OpType::Connect,
        .fd = c->fd,
        .result = err,
        .user_data = 0,
    };
    c->self->deliver_completion(completion);
}

} // anonymous namespace

bool GcdBackend::start() {
    dispatch_queue_ = dispatch_queue_create("com.hpactor.gcdbackend",
                                            DISPATCH_QUEUE_SERIAL);
    running_ = true;
    return true;
}

void GcdBackend::stop() {
    if (dispatch_queue_) {
        dispatch_release(dispatch_queue_);
        dispatch_queue_ = nullptr;
    }
    running_ = false;
}

bool GcdBackend::add_fd(int fd, IoEvent events) {
    (void)fd;
    (void)events;
    return true;
}

bool GcdBackend::update_fd(int fd, IoEvent events) {
    (void)fd;
    (void)events;
    return true;
}

bool GcdBackend::remove_fd(int fd) {
    auto it = fd_sources_.find(fd);
    if (it != fd_sources_.end()) {
        dispatch_source_cancel(it->second);
        dispatch_release(it->second);
        fd_sources_.erase(it);
    }
    return true;
}

int GcdBackend::register_buffer(const void* addr, size_t len) {
    int buffer_id = static_cast<int>(registered_buffers_.size());
    registered_buffers_.emplace_back(addr, len);
    return buffer_id;
}

bool GcdBackend::unregister_buffer(int buffer_id) {
    (void)buffer_id;
    return false;
}

void GcdBackend::async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) {
    size_t total_len = 0;
    for (int i = 0; i < buf_count; ++i) total_len += bufs[i].iov_len;
    auto* c = new AsyncSendContext{
        .self = this,
        .fd = fd,
        .actor = actor,
        .op_type = op_type,
        .data = std::vector<uint8_t>(total_len),
    };
    size_t offset = 0;
    for (int i = 0; i < buf_count; ++i) {
        std::memcpy(c->data.data() + offset, bufs[i].iov_base, bufs[i].iov_len);
        offset += bufs[i].iov_len;
    }
    dispatch_async_f(dispatch_queue_, c, async_send_trampoline);
}

void GcdBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) {
    size_t recv_capacity = 0;
    for (int i = 0; i < buf_count; ++i) recv_capacity += bufs[i].iov_len;
    auto* c = new AsyncRecvContext{
        .self = this,
        .fd = fd,
        .actor = actor,
        .op_type = op_type,
        .bufs = std::vector<iovec>(bufs, bufs + buf_count),
        .recv_capacity = recv_capacity,
    };
    dispatch_async_f(dispatch_queue_, c, async_recv_trampoline);
}

void GcdBackend::async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                  ActorId actor, uint32_t op_type) {
    auto [buf_addr, buf_len] = registered_buffers_[static_cast<size_t>(buffer_id)];
    iovec single_buf{static_cast<char*>(const_cast<void*>(buf_addr)) + offset, len};
    async_send(fd, &single_buf, 1, actor, op_type);
}

void GcdBackend::async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                  ActorId actor, uint32_t op_type) {
    auto [buf_addr, buf_len] = registered_buffers_[static_cast<size_t>(buffer_id)];
    iovec single_buf{static_cast<char*>(const_cast<void*>(buf_addr)) + offset, len};
    async_recv(fd, &single_buf, 1, actor, op_type);
}

void GcdBackend::async_accept(int fd, ActorId actor) {
    auto* ctx = new AcceptContext{this, fd, actor};
    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(fd), 0, dispatch_queue_);
    dispatch_set_context(source, ctx);
    dispatch_set_finalizer_f(source, [](void* ctx) {
        delete static_cast<AcceptContext*>(ctx);
    });
    dispatch_source_set_event_handler_f(source, accept_trampoline);
    dispatch_resume(source);
    accept_sources_[fd] = source;
}

void GcdBackend::async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                               ActorId actor) {
    (void)addr;
    (void)addrlen;
    int ret = ::connect(fd, addr, addrlen);
    if (ret < 0 && errno == EINPROGRESS) {
        auto* ctx = new ConnectContext{this, fd, actor};
        dispatch_source_t source = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_WRITE, static_cast<uintptr_t>(fd), 0, dispatch_queue_);
        dispatch_set_context(source, ctx);
        dispatch_set_finalizer_f(source, [](void* ctx) {
            delete static_cast<ConnectContext*>(ctx);
        });
        dispatch_source_set_event_handler_f(source, connect_trampoline);
        dispatch_resume(source);
        connect_sources_[fd] = source;
    } else if (ret == 0) {
        OpCompletion completion{
            .actor = actor,
            .type = OpType::Connect,
            .fd = fd,
            .result = 0,
            .user_data = 0,
        };
        deliver_completion(completion);
    }
}

void GcdBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) {
    async_recv(fd, bufs, buf_count, actor, op_type);
}

void GcdBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
                              const sockaddr* addr, socklen_t addrlen,
                              ActorId actor, uint32_t op_type) {
    (void)addr;
    (void)addrlen;
    async_send(fd, bufs, buf_count, actor, op_type);
}

struct RunAfterContext {
    GcdBackend* self;
    uint64_t handle;
    ActorId actor;
};

void run_after_trampoline(void* ctx) {
    auto* c = static_cast<RunAfterContext*>(ctx);
    if (c->self->is_timer_cancelled(c->handle)) return;
    OpCompletion completion{
        .actor = c->actor,
        .type = OpType::TimerFired,
        .fd = -1,
        .result = 0,
        .user_data = c->handle,
    };
    c->self->deliver_completion(completion);
}

uint64_t GcdBackend::run_after(ActorId actor, int delay_ms) {
    uint64_t handle = next_timer_handle_++;
    auto* ctx = new RunAfterContext{this, handle, actor};
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(delay_ms) * 1000000LL),
                    dispatch_queue_, ctx, [](void* ctx) {
        delete static_cast<RunAfterContext*>(ctx);
    });
    return handle;
}

struct TimerContext {
    GcdBackend* self;
    uint64_t handle;
    ActorId actor;
};

void timer_trampoline(void* ctx) {
    auto* c = static_cast<TimerContext*>(ctx);
    if (c->self->is_timer_cancelled(c->handle)) return;
    OpCompletion completion{
        .actor = c->actor,
        .type = OpType::TimerFired,
        .fd = -1,
        .result = 0,
        .user_data = c->handle,
    };
    c->self->deliver_completion(completion);
}

uint64_t GcdBackend::run_every(ActorId actor, int interval_ms) {
    uint64_t handle = next_timer_handle_++;
    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_queue_);
    int64_t interval_ns = static_cast<int64_t>(interval_ms) * 1000000LL;
    dispatch_source_set_timer(source,
                              dispatch_time(DISPATCH_TIME_NOW, interval_ns),
                              static_cast<uint64_t>(interval_ns), 0);
    auto* ctx = new TimerContext{this, handle, actor};
    dispatch_set_context(source, ctx);
    dispatch_set_finalizer_f(source, [](void* ctx) {
        delete static_cast<TimerContext*>(ctx);
    });
    dispatch_source_set_event_handler_f(source, timer_trampoline);
    dispatch_resume(source);
    timer_sources_[handle] = source;
    return handle;
}

void GcdBackend::cancel_timer(uint64_t handle) {
    cancelled_timers_.insert(handle);
    auto it = timer_sources_.find(handle);
    if (it != timer_sources_.end()) {
        dispatch_source_cancel(it->second);
        dispatch_release(it->second);
        timer_sources_.erase(it);
    }
}

int GcdBackend::wait(int timeout_ms) {
    (void)timeout_ms;
    return 0;
}

void GcdBackend::process_completions() {
}

void GcdBackend::deliver_completion(OpCompletion completion) {
    (void)completion;
}

#else // !defined(__APPLE__)

// Linux stubs

GcdBackend::GcdBackend() = default;
GcdBackend::~GcdBackend() = default;

bool GcdBackend::start() { return false; }
void GcdBackend::stop() {}

bool GcdBackend::add_fd(int fd, IoEvent events) {
    (void)fd; (void)events;
    return false;
}
bool GcdBackend::update_fd(int fd, IoEvent events) {
    (void)fd; (void)events;
    return false;
}
bool GcdBackend::remove_fd(int fd) {
    (void)fd;
    return false;
}

int GcdBackend::register_buffer(const void* addr, size_t len) {
    (void)addr; (void)len;
    return -1;
}
bool GcdBackend::unregister_buffer(int buffer_id) {
    (void)buffer_id;
    return false;
}

void GcdBackend::async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)actor; (void)op_type;
}
void GcdBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)actor; (void)op_type;
}

void GcdBackend::async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                  ActorId actor, uint32_t op_type) {
    (void)fd; (void)buffer_id; (void)offset; (void)len; (void)actor; (void)op_type;
}
void GcdBackend::async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                                  ActorId actor, uint32_t op_type) {
    (void)fd; (void)buffer_id; (void)offset; (void)len; (void)actor; (void)op_type;
}

void GcdBackend::async_accept(int fd, ActorId actor) {
    (void)fd; (void)actor;
}
void GcdBackend::async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                               ActorId actor) {
    (void)fd; (void)addr; (void)addrlen; (void)actor;
}

void GcdBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)actor; (void)op_type;
}
void GcdBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
                              const sockaddr* addr, socklen_t addrlen,
                              ActorId actor, uint32_t op_type) {
    (void)fd; (void)bufs; (void)buf_count; (void)addr; (void)addrlen; (void)actor; (void)op_type;
}

uint64_t GcdBackend::run_after(ActorId actor, int delay_ms) {
    (void)actor; (void)delay_ms;
    return 0;
}
uint64_t GcdBackend::run_every(ActorId actor, int interval_ms) {
    (void)actor; (void)interval_ms;
    return 0;
}
void GcdBackend::cancel_timer(uint64_t handle) {
    (void)handle;
}

int GcdBackend::wait(int timeout_ms) {
    (void)timeout_ms;
    return -1;
}
void GcdBackend::process_completions() {}

void GcdBackend::deliver_completion(OpCompletion completion) {
    (void)completion;
}

#endif

} // namespace net
} // namespace hpactor
