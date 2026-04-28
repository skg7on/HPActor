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
#    include <hpactor/net/reactor/kqueue_backend.hpp>
#    if HPACTOR_ENABLE_PROACTOR
#        include <hpactor/net/proactor/gcd_backend.hpp>
#    endif
#elif defined(__linux__)
#    include <hpactor/net/reactor/epoll_backend.hpp>
#    if HPACTOR_ENABLE_PROACTOR
#        include <hpactor/net/proactor/iouring_backend.hpp>
#    endif
#endif

namespace hpactor {

namespace net {

EventLoop::EventLoop() {
#if defined(__APPLE__)
    // Try kqueue first (for sync I/O testing)
    auto kqueue_backend = std::make_unique<KqueueBackend>();
    if (kqueue_backend->start()) {
        backend_name_ = "kqueue";
        static_cast<KqueueBackend*>(kqueue_backend.get())->set_loop(this);
        backend_ = std::move(kqueue_backend);
#if HPACTOR_ENABLE_PROACTOR
    } else {
        // Fall back to GCD only when Proactor mode is enabled
        auto gcd_backend = std::make_unique<GcdBackend>();
        if (gcd_backend->start()) {
            backend_name_ = "gcd";
            static_cast<GcdBackend*>(gcd_backend.get())->set_loop(this);
            backend_ = std::move(gcd_backend);
        }
#endif
    }
#elif defined(__linux__)
#if HPACTOR_ENABLE_PROACTOR
    // Try io_uring first (preferred on Linux)
    auto iouring_backend = std::make_unique<IoUringBackend>();
    if (iouring_backend->start()) {
        backend_name_ = "iouring";
        backend_ = std::move(iouring_backend);
    } else {
        // Fall back to epoll
        auto epoll_backend = std::make_unique<EpollBackend>();
        if (epoll_backend->start()) {
            backend_name_ = "epoll";
            static_cast<EpollBackend*>(epoll_backend.get())->set_loop(this);
            backend_ = std::move(epoll_backend);
        }
    }
#else
    // Reactor mode: use epoll directly
    auto epoll_backend = std::make_unique<EpollBackend>();
    if (epoll_backend->start()) {
        backend_name_ = "epoll";
        static_cast<EpollBackend*>(epoll_backend.get())->set_loop(this);
        backend_ = std::move(epoll_backend);
    }
#endif
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
    if (static_cast<uint32_t>(events) & static_cast<uint32_t>(Event::Write)) {
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

void EventLoop::set_read_handler(int fd, read_callback handler) {
    if (backend_) {
        backend_->set_read_handler(fd, std::move(handler));
    }
}

void EventLoop::clear_read_handler(int fd) {
    if (backend_) {
        backend_->clear_read_handler(fd);
    }
}

bool EventLoop::supports_read_handler() const {
    return backend_ && backend_->supports_read_handler();
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
        backend_->process_events();
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