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

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/event_loop.hpp>

#if defined(__APPLE__)
#    include <hpactor/net/gcd_backend.hpp>
#    include <hpactor/net/kqueue_backend.hpp>
#elif defined(__linux__)
#    include <hpactor/net/epoll_backend.hpp>
#    include <hpactor/net/iouring_backend.hpp>
#endif

namespace hpactor {

namespace net {

namespace {

// Backend factory helper - tries to create a backend, returns nullptr on
// failure
template <typename Backend, typename... Args>
std::unique_ptr<Backend> try_create_backend(Args&&... args) {
    auto backend = std::make_unique<Backend>(std::forward<Args>(args)...);
    return backend;
}

} // anonymous namespace

// BackendAdapter wraps the real AsyncIoBackend and intercepts
// deliver_completion to route to EventLoop
class BackendAdapter : public AsyncIoBackend {
  public:
    BackendAdapter(EventLoop* loop, std::unique_ptr<AsyncIoBackend> backend)
        : loop_(loop), backend_(std::move(backend)) {}

    bool start() override {
        return backend_->start();
    }
    void stop() override {
        backend_->stop();
    }

    bool add_fd(int fd, IoEvent events) override {
        return backend_->add_fd(fd, events);
    }
    bool update_fd(int fd, IoEvent events) override {
        return backend_->update_fd(fd, events);
    }
    bool remove_fd(int fd) override {
        return backend_->remove_fd(fd);
    }

    int register_buffer(const void* addr, size_t len) override {
        return backend_->register_buffer(addr, len);
    }
    bool unregister_buffer(int buffer_id) override {
        return backend_->unregister_buffer(buffer_id);
    }

    void async_send(int fd, const iovec* bufs, int buf_count, ActorId actor,
                    uint32_t op_type) override {
        backend_->async_send(fd, bufs, buf_count, actor, op_type);
    }
    void async_recv(int fd, const iovec* bufs, int buf_count, ActorId actor,
                    uint32_t op_type) override {
        backend_->async_recv(fd, bufs, buf_count, actor, op_type);
    }

    void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type) override {
        backend_->async_send_fixed(fd, buffer_id, offset, len, actor, op_type);
    }
    void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type) override {
        backend_->async_recv_fixed(fd, buffer_id, offset, len, actor, op_type);
    }

    void async_accept(int fd, ActorId actor) override {
        backend_->async_accept(fd, actor);
    }
    void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                       ActorId actor) override {
        backend_->async_connect(fd, addr, addrlen, actor);
    }

    void async_recvfrom(int fd, const iovec* bufs, int buf_count, ActorId actor,
                        uint32_t op_type) override {
        backend_->async_recvfrom(fd, bufs, buf_count, actor, op_type);
    }
    void
    async_sendto(int fd, const iovec* bufs, int buf_count, const sockaddr* addr,
                 socklen_t addrlen, ActorId actor, uint32_t op_type) override {
        backend_->async_sendto(fd, bufs, buf_count, addr, addrlen, actor, op_type);
    }

    uint64_t run_after(ActorId actor, int delay_ms) override {
        return backend_->run_after(actor, delay_ms);
    }
    uint64_t run_every(ActorId actor, int interval_ms) override {
        return backend_->run_every(actor, interval_ms);
    }
    void cancel_timer(uint64_t handle) override {
        backend_->cancel_timer(handle);
    }

    int wait(int timeout_ms) override {
        return backend_->wait(timeout_ms);
    }
    void process_completions() override {
        backend_->process_completions();
    }

    // Override deliver_completion to intercept and route to EventLoop
    void deliver_completion(OpCompletion completion) override {
        loop_->enqueue_completion(completion);
    }

  private:
    EventLoop* loop_;
    std::unique_ptr<AsyncIoBackend> backend_;
};

EventLoop::EventLoop() {
#if defined(__APPLE__)
    // Try kqueue first (for sync I/O testing)
    auto kqueue_backend = try_create_backend<KqueueBackend>();
    if (kqueue_backend->start()) {
        backend_name_ = "kqueue";
        static_cast<KqueueBackend*>(kqueue_backend.get())->set_loop(this);
        backend_ =
            std::make_unique<BackendAdapter>(this, std::move(kqueue_backend));
    } else {
        // Fall back to GCD
        auto gcd_backend = try_create_backend<GcdBackend>();
        if (gcd_backend->start()) {
            backend_name_ = "gcd";
            static_cast<GcdBackend*>(gcd_backend.get())->set_loop(this);
            backend_ =
                std::make_unique<BackendAdapter>(this, std::move(gcd_backend));
        }
    }
#elif defined(__linux__)
    // Try io_uring first (preferred on Linux)
    auto iouring_backend = try_create_backend<IoUringBackend>();
    if (iouring_backend->start()) {
        backend_name_ = "iouring";
        backend_ =
            std::make_unique<BackendAdapter>(this, std::move(iouring_backend));
    } else {
        // Fall back to epoll
        auto epoll_backend = try_create_backend<EpollBackend>();
        if (epoll_backend->start()) {
            backend_name_ = "epoll";
            static_cast<EpollBackend*>(epoll_backend.get())->set_loop(this);
            backend_ =
                std::make_unique<BackendAdapter>(this, std::move(epoll_backend));
        }
    }
#endif
}

EventLoop::~EventLoop() = default;

bool EventLoop::run() {
    if (running_.load()) {
        return true;
    }
    if (!backend_) {
        return false;
    }
    running_.store(backend_->start());
    return running_.load();
}

void EventLoop::stop() {
    running_.store(false);
    if (backend_) {
        backend_->stop();
    }
}

const char* EventLoop::backend_name() const {
    return backend_name_;
}

bool EventLoop::add_fd(int fd, Event events) {
    if (!backend_) {
        return false;
    }
    IoEvent io_events = IoEvent::Read;
    if (int(events) & int(Event::Write)) {
        io_events = static_cast<IoEvent>(static_cast<uint32_t>(io_events) |
                                         static_cast<uint32_t>(IoEvent::Write));
    }
    fd_events_[fd] = events;
    return backend_->add_fd(fd, io_events);
}

bool EventLoop::update_fd(int fd, Event events) {
    return add_fd(fd, events);
}

bool EventLoop::remove_fd(int fd) {
    fd_events_.erase(fd);
    if (!backend_) {
        return false;
    }
    return backend_->remove_fd(fd);
}

int EventLoop::wait(int timeout_ms) {
    if (!backend_) {
        return -1;
    }
    return backend_->wait(timeout_ms);
}

bool EventLoop::has_event(int /*fd*/, Event /*event*/) const {
    return true;
}

uint64_t EventLoop::run_after(timer_callback callback, int delay_ms) {
    if (!backend_) {
        return 0;
    }
    uint64_t handle = next_timer_handle_++;
    timer_callbacks_[handle] = std::move(callback);
    // Use ActorId(0) as a sentinel - we'll intercept timer completions
    // The backend will deliver completion with user_data = handle
    uint64_t backend_handle = backend_->run_after(ActorId(0), delay_ms);
    if (backend_handle == 0) {
        timer_callbacks_.erase(handle);
        return 0;
    }
    backend_handle_to_handle_[backend_handle] = handle;
    return handle;
}

uint64_t EventLoop::run_every(timer_callback callback, int interval_ms) {
    if (!backend_) {
        return 0;
    }
    uint64_t handle = next_timer_handle_++;
    timer_callbacks_[handle] = std::move(callback);
    uint64_t backend_handle = backend_->run_every(ActorId(0), interval_ms);
    if (backend_handle == 0) {
        timer_callbacks_.erase(handle);
        return 0;
    }
    backend_handle_to_handle_[backend_handle] = handle;
    repeating_timers_.insert(handle); // Mark as repeating timer
    return handle;
}

void EventLoop::cancel_timer(uint64_t timer_handle) {
    timer_callbacks_.erase(timer_handle);
    repeating_timers_.erase(timer_handle);
    if (backend_) {
        backend_->cancel_timer(timer_handle);
    }
}

void EventLoop::process_completions() {
    if (backend_) {
        backend_->process_completions();
    }
}

void EventLoop::enqueue_completion(OpCompletion completion) {
    if (completion_callback_) {
        completion_callback_(completion);
        return;
    }
    if (completion.type == OpType::TimerFired) {
        deliver_timer_completion(completion);
    } else if (actor_system_) {
        actor_system_->enqueue_completion(completion);
    }
}

void EventLoop::deliver_timer_completion(OpCompletion completion) {
    uint64_t backend_handle = completion.user_data;
    auto it = backend_handle_to_handle_.find(backend_handle);
    if (it != backend_handle_to_handle_.end()) {
        uint64_t handle = it->second;
        auto callback_it = timer_callbacks_.find(handle);
        if (callback_it != timer_callbacks_.end()) {
            callback_it->second();
            // Only erase callback for one-shot timers; repeating timers stay
            if (repeating_timers_.find(handle) == repeating_timers_.end()) {
                timer_callbacks_.erase(callback_it);
                backend_handle_to_handle_.erase(it);
            }
        }
    }
}

void EventLoop::set_actor_system(ActorSystem* actor_system) {
    actor_system_ = actor_system;
}

} // namespace net
} // namespace hpactor
