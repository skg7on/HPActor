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

#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/gcd_backend.hpp>

#if defined(__APPLE__)
#    include <cerrno>
#    include <cstdlib>
#    include <cstring>
#    include <dispatch/dispatch.h>
#    include <fcntl.h>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <sys/uio.h>
#    include <unistd.h>
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

struct RunAfterContext {
    GcdBackend* self;
    uint64_t handle;
    ActorId actor;
};

struct TimerContext {
    GcdBackend* self;
    uint64_t handle;
    ActorId actor;
    int interval_ms;
};

void async_send_trampoline(void* ctx) {
    auto* c = static_cast<AsyncSendContext*>(ctx);
    ssize_t n = ::write(c->fd, c->data.data(), c->data.size());
    int result = (n < 0) ? -errno : static_cast<int>(n);
    OpCompletion completion{
        .actor = c->actor,
        .type = static_cast<OpType>(c->op_type),
        .fd = c->fd,
        .result = result,
        .user_data = 0,
    };
    c->self->deliver_completion(completion);
    delete c;
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
            .result = -errno,
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
    delete c;
}

void accept_trampoline(void* ctx) {
    auto* c = static_cast<AcceptContext*>(ctx);
    int client_fd = ::accept(c->fd, nullptr, nullptr);
    if (client_fd >= 0) {
        // Register client_fd with event loop before delivering completion
        // so caller can immediately use it with async_recv
        c->self->add_fd(client_fd, IoEvent::Read);

        OpCompletion completion{
            .actor = c->actor,
            .type = OpType::Accept,
            .fd = client_fd,
            .result = client_fd,
            .user_data = 0,
        };
        c->self->deliver_completion(completion);
    }
    // Note: We don't delete c here - the AcceptContext is reused for subsequent
    // accepts. The dispatch_source remains active and will fire again when
    // another connection arrives.
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
    delete c;
}

} // anonymous namespace

// Helper function for run_after dispatch (must be static to convert to function
// pointer)
static void run_after_dispatch(void* ctx) {
    auto* c = static_cast<RunAfterContext*>(ctx);
    if (c->self->is_timer_cancelled(c->handle)) {
        delete c;
        return;
    }
    // Also check if backend is still running (not destroyed)
    if (!c->self->is_running()) {
        delete c;
        return;
    }
    OpCompletion completion{
        .actor = c->actor,
        .type = OpType::TimerFired,
        .fd = -1,
        .result = 0,
        .user_data = c->handle,
    };
    c->self->deliver_completion(completion);
    delete c;
}

bool GcdBackend::start() {
    dispatch_queue_ =
        dispatch_queue_create("com.hpactor.gcdbackend", DISPATCH_QUEUE_SERIAL);

    // Create wakeup pipe for signaling wait()
    if (pipe(wakeup_pipe_) < 0) {
        wakeup_pipe_[0] = -1;
        wakeup_pipe_[1] = -1;
    } else {
        // Set non-blocking for the read end
        int flags = fcntl(wakeup_pipe_[0], F_GETFL, 0);
        fcntl(wakeup_pipe_[0], F_SETFL, flags | O_NONBLOCK);
    }

    running_ = true;
    return true;
}

void GcdBackend::stop() {
    running_ = false;

    // Cancel all timer sources
    for (auto& [handle, source] : timer_sources_) {
        dispatch_source_cancel(source);
        dispatch_release(source);
    }
    timer_sources_.clear();

    // Cancel all fd sources
    for (auto& [fd, source] : fd_sources_) {
        dispatch_source_cancel(source);
        dispatch_release(source);
    }
    fd_sources_.clear();

    // Cancel all accept sources
    for (auto& [fd, source] : accept_sources_) {
        dispatch_source_cancel(source);
        dispatch_release(source);
    }
    accept_sources_.clear();

    // Cancel all connect sources
    for (auto& [fd, source] : connect_sources_) {
        dispatch_source_cancel(source);
        dispatch_release(source);
    }
    connect_sources_.clear();

    // Close wakeup pipe
    if (wakeup_pipe_[0] >= 0) {
        close(wakeup_pipe_[0]);
        close(wakeup_pipe_[1]);
        wakeup_pipe_[0] = -1;
        wakeup_pipe_[1] = -1;
    }

    if (dispatch_queue_) {
        dispatch_release(dispatch_queue_);
        dispatch_queue_ = nullptr;
    }
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
    for (int i = 0; i < buf_count; ++i)
        total_len += bufs[i].iov_len;
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
    for (int i = 0; i < buf_count; ++i)
        recv_capacity += bufs[i].iov_len;
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

void GcdBackend::async_send_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) {
    auto [buf_addr, buf_len] = registered_buffers_[static_cast<size_t>(buffer_id)];
    iovec single_buf{static_cast<char*>(const_cast<void*>(buf_addr)) + offset, len};
    async_send(fd, &single_buf, 1, actor, op_type);
}

void GcdBackend::async_recv_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) {
    auto [buf_addr, buf_len] = registered_buffers_[static_cast<size_t>(buffer_id)];
    iovec single_buf{static_cast<char*>(const_cast<void*>(buf_addr)) + offset, len};
    async_recv(fd, &single_buf, 1, actor, op_type);
}

void GcdBackend::async_accept(int fd, ActorId actor) {
    // Cancel any existing source for this fd to prevent context leak
    auto existing = accept_sources_.find(fd);
    if (existing != accept_sources_.end()) {
        dispatch_source_cancel(existing->second);
        dispatch_release(existing->second);
        accept_sources_.erase(existing);
    }

    auto* ctx = new AcceptContext{this, fd, actor};
    dispatch_source_t source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
                               static_cast<uintptr_t>(fd), 0, dispatch_queue_);
    dispatch_set_context(source, ctx);
    // Note: finalizer only runs when source is cancelled/released, not on each
    // accept. Context is reused for subsequent accepts.
    dispatch_set_finalizer_f(
        source, [](void* ctx) { delete static_cast<AcceptContext*>(ctx); });
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
        dispatch_source_t source =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE,
                                   static_cast<uintptr_t>(fd), 0, dispatch_queue_);
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

uint64_t GcdBackend::run_after(ActorId actor, int delay_ms) {
    uint64_t handle = next_timer_handle_++;
    auto* ctx = new RunAfterContext{this, handle, actor};
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
                                   static_cast<int64_t>(delay_ms) * 1000000LL),
                     dispatch_queue_, ctx, run_after_dispatch);
    return handle;
}

void timer_trampoline(void* ctx) {
    auto* c = static_cast<TimerContext*>(ctx);
    // Check if backend is still running (not destroyed)
    if (!c->self->is_running()) {
        delete c;
        return;
    }
    if (c->self->is_timer_cancelled(c->handle)) {
        // Cancelled - delete context and don't reschedule
        delete c;
        return;
    }
    // Deliver the completion
    OpCompletion completion{
        .actor = c->actor,
        .type = OpType::TimerFired,
        .fd = -1,
        .result = 0,
        .user_data = c->handle,
    };
    c->self->deliver_completion(completion);
    // Reschedule for next interval (context stays alive)
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
                                   static_cast<int64_t>(c->interval_ms) * 1000000LL),
                     c->self->get_dispatch_queue(), c, timer_trampoline);
}

uint64_t GcdBackend::run_every(ActorId actor, int interval_ms) {
    uint64_t handle = next_timer_handle_++;
    auto* ctx = new TimerContext{this, handle, actor, interval_ms};
    // Start the repeating cycle with first fire after interval
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW,
                                   static_cast<int64_t>(interval_ms) * 1000000LL),
                     dispatch_queue_, ctx, timer_trampoline);
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
    if (wakeup_pipe_[0] < 0) {
        // No wakeup pipe - just sleep briefly
        usleep(static_cast<useconds_t>(timeout_ms) * 1000);
        return 0;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(wakeup_pipe_[0], &read_fds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int result = select(wakeup_pipe_[0] + 1, &read_fds, nullptr, nullptr, &tv);
    if (result > 0 && FD_ISSET(wakeup_pipe_[0], &read_fds)) {
        // Drain the wakeup pipe
        uint8_t buf[64];
        while (read(wakeup_pipe_[0], buf, sizeof(buf)) > 0) {
        }
        return 1;
    }
    return result;
}

void GcdBackend::process_completions() {
    std::lock_guard<std::mutex> lock(completion_mutex_);
    while (!completion_queue_.empty()) {
        auto completion = completion_queue_.front();
        completion_queue_.pop();
        // Route to EventLoop if set
        if (loop_) {
            loop_->enqueue_completion(completion);
        }
    }
}

void GcdBackend::deliver_completion(OpCompletion completion) {
    {
        std::lock_guard<std::mutex> lock(completion_mutex_);
        completion_queue_.push(completion);
    }
    // Wake up wait() by writing to the pipe
    if (wakeup_pipe_[1] >= 0) {
        uint8_t byte = 1;
        (void)write(wakeup_pipe_[1], &byte, 1);
    }
}

void GcdBackend::set_read_handler(int fd, read_callback handler) {
    // Cancel any existing source for this fd
    auto existing = read_handlers_.find(fd);
    if (existing != read_handlers_.end()) {
        if (existing->second.source) {
            dispatch_source_cancel(existing->second.source);
            dispatch_release(existing->second.source);
        }
    }

    // Allocate buffer and create dispatch source for read
    constexpr size_t kRecvBufferSize = 65536;
    ReadHandler rh;
    rh.buffer.resize(kRecvBufferSize);
    rh.callback = std::move(handler);

    // Create dispatch source for reading
    dispatch_source_t source =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
                             static_cast<uintptr_t>(fd), 0,
                             dispatch_queue_);
    rh.source = source;

    // Set up the event handler
    dispatch_source_set_event_handler(source, ^{
        // Read available data
        ssize_t n = ::read(fd, const_cast<uint8_t*>(rh.buffer.data()), rh.buffer.size());
        if (n > 0) {
            bytes data(rh.buffer.data(), rh.buffer.data() + n);
            rh.callback(data);
        } else if (n == 0) {
            // EOF - connection closed
            clear_read_handler(fd);
        }
        // If n < 0 and EAGAIN, just wait for next event
    });

    read_handlers_[fd] = std::move(rh);
    dispatch_resume(source);
}

void GcdBackend::clear_read_handler(int fd) {
    auto it = read_handlers_.find(fd);
    if (it != read_handlers_.end()) {
        if (it->second.source) {
            dispatch_source_cancel(it->second.source);
            dispatch_release(it->second.source);
        }
        read_handlers_.erase(it);
    }
}

#else // !defined(__APPLE__)

// Linux stubs

GcdBackend::GcdBackend() = default;
GcdBackend::~GcdBackend() = default;

bool GcdBackend::start() {
    return false;
}
void GcdBackend::stop() {}

bool GcdBackend::add_fd(int fd, IoEvent events) {
    (void)fd;
    (void)events;
    return false;
}
bool GcdBackend::update_fd(int fd, IoEvent events) {
    (void)fd;
    (void)events;
    return false;
}
bool GcdBackend::remove_fd(int fd) {
    (void)fd;
    return false;
}

int GcdBackend::register_buffer(const void* addr, size_t len) {
    (void)addr;
    (void)len;
    return -1;
}
bool GcdBackend::unregister_buffer(int buffer_id) {
    (void)buffer_id;
    return false;
}

void GcdBackend::async_send(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}
void GcdBackend::async_recv(int fd, const iovec* bufs, int buf_count,
                            ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}

void GcdBackend::async_send_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    (void)actor;
    (void)op_type;
}
void GcdBackend::async_recv_fixed(int fd, int buffer_id, size_t offset,
                                  size_t len, ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)buffer_id;
    (void)offset;
    (void)len;
    (void)actor;
    (void)op_type;
}

void GcdBackend::async_accept(int fd, ActorId actor) {
    (void)fd;
    (void)actor;
}
void GcdBackend::async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                               ActorId actor) {
    (void)fd;
    (void)addr;
    (void)addrlen;
    (void)actor;
}

void GcdBackend::async_recvfrom(int fd, const iovec* bufs, int buf_count,
                                ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)actor;
    (void)op_type;
}
void GcdBackend::async_sendto(int fd, const iovec* bufs, int buf_count,
                              const sockaddr* addr, socklen_t addrlen,
                              ActorId actor, uint32_t op_type) {
    (void)fd;
    (void)bufs;
    (void)buf_count;
    (void)addr;
    (void)addrlen;
    (void)actor;
    (void)op_type;
}

void GcdBackend::set_read_handler(int fd, read_callback handler) {
    (void)fd;
    (void)handler;
}

void GcdBackend::clear_read_handler(int fd) {
    (void)fd;
}

uint64_t GcdBackend::run_after(ActorId actor, int delay_ms) {
    (void)actor;
    (void)delay_ms;
    return 0;
}
uint64_t GcdBackend::run_every(ActorId actor, int interval_ms) {
    (void)actor;
    (void)interval_ms;
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
