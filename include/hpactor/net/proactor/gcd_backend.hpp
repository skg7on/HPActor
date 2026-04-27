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

#pragma once

#include <hpactor/net/async_io_fwd.hpp>
#include <hpactor/net/reactor_backend.hpp>

#if defined(__APPLE__)
#    include <dispatch/dispatch.h>
#else
// Stub for Linux compilation safety
struct dispatch_queue_s {
    int unused;
};
struct dispatch_source_s {
    int unused;
};
struct dispatch_data_s {
    int unused;
};
using dispatch_queue_t = struct dispatch_queue_s*;
using dispatch_source_t = struct dispatch_source_s*;
using dispatch_data_t = struct dispatch_data_s*;
#endif

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hpactor {
namespace net {

// Forward declaration to avoid circular include
class EventLoop;

class GcdBackend : public IReactorBackend {
  public:
    GcdBackend();
    ~GcdBackend() override;

    bool start() override;
    void stop() override;

    bool add_fd(int fd, IoEvent events) override;
    bool update_fd(int fd, IoEvent events) override;
    bool remove_fd(int fd) override;

    int register_buffer(const void* addr, size_t len) override;
    bool unregister_buffer(int buffer_id) override;

    uint64_t run_after(ActorId actor, int delay_ms) override;
    uint64_t run_every(ActorId actor, int interval_ms) override;
    void cancel_timer(uint64_t handle) override;

    int wait(int timeout_ms) override;
    void process_events() override;

    // Proactor methods
    void async_send(int fd, const iovec* bufs, int buf_count, ActorId actor,
                    uint32_t op_type) override;
    void async_recv(int fd, const iovec* bufs, int buf_count, ActorId actor,
                    uint32_t op_type) override;
    void async_accept(int fd, ActorId actor) override;
    void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                       ActorId actor) override;
    void async_sendto(int fd, const iovec* bufs, int buf_count,
                       const sockaddr* addr, socklen_t addrlen, ActorId actor,
                       uint32_t op_type) override;
    void async_recvfrom(int fd, const iovec* bufs, int buf_count, ActorId actor,
                        uint32_t op_type) override;

    // Read handler management - no-op in reactor mode
    void set_read_handler(int fd, read_callback handler) override {
        (void)fd;
        (void)handler;
    }
    void clear_read_handler(int fd) override {
        (void)fd;
    }

    // Deliver completion to actor (public for trampoline access)
    void deliver_completion(OpCompletion completion);

    // Check if timer handle was cancelled (public for trampoline access)
    bool is_timer_cancelled(uint64_t handle) const {
        return cancelled_timers_.count(handle) > 0;
    }

    // Set the EventLoop pointer for routing completions
    void set_loop(net::EventLoop* loop) {
        loop_ = loop;
    }

    // Get the dispatch queue for timer rescheduling
    dispatch_queue_t get_dispatch_queue() const {
        return dispatch_queue_;
    }

    // Check if backend is still running (for timer trampoline safety)
    bool is_running() const {
        return running_;
    }

    // --- Extended proactor methods (not in IReactorBackend) ---
    void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type);
    void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type);

  private:
    // Active timer handles for cancellation
    std::unordered_set<uint64_t> cancelled_timers_;

    dispatch_queue_t dispatch_queue_ = nullptr;

    // Wakeup pipe for signaling wait()
    int wakeup_pipe_[2] = {-1, -1};

    // Completion queue for delivering to EventLoop
    std::queue<OpCompletion> completion_queue_;
    std::mutex completion_mutex_;
    std::condition_variable completion_cv_;

    // Timer handle → dispatch_source_t (for cancellation)
    std::unordered_map<uint64_t, dispatch_source_t> timer_sources_;
    std::atomic<uint64_t> next_timer_handle_{1};

    // Registered buffers: buffer_id → (addr, len)
    std::vector<std::pair<const void*, size_t>> registered_buffers_;

    // fd → dispatch_source_t (for remove_fd)
    std::unordered_map<int, dispatch_source_t> fd_sources_;

    // fd → dispatch_source_t (for async_accept)
    std::unordered_map<int, dispatch_source_t> accept_sources_;

    // fd → dispatch_source_t (for async_connect)
    std::unordered_map<int, dispatch_source_t> connect_sources_;

    // EventLoop pointer for routing completions
    net::EventLoop* loop_ = nullptr;

    bool running_ = false;
};

} // namespace net
} // namespace hpactor