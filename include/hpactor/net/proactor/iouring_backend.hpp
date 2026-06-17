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

#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__linux__)
#    include <liburing.h>
#else
// Stub definition for macOS compilation safety
struct io_uring {
    int unused;
};
struct io_uring_sqe {
    int unused;
};
struct io_uring_cqe {
    int unused;
};
struct io_uring_params {
    int unused;
};
#endif

namespace hpactor {
namespace net {

class IoUringBackend : public IReactorBackend {
  public:
    IoUringBackend();
    ~IoUringBackend() override;

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
    void
    async_sendto(int fd, const iovec* bufs, int buf_count, const sockaddr* addr,
                 socklen_t addrlen, ActorId actor, uint32_t op_type) override;
    void async_recvfrom(int fd, const iovec* bufs, int buf_count, ActorId actor,
                        uint32_t op_type) override;

    // Read handler management - no-op (proactor uses async_recv)
    void set_read_handler(int fd, read_callback handler) override {
        (void)fd;
        (void)handler;
    }
    void clear_read_handler(int fd) override {
        (void)fd;
    }

    bool supports_read_handler() const override {
        return false;
    }

    // Write handler management - no-op (proactor uses async_connect)
    void set_write_handler(int fd, write_callback handler) override {
        (void)fd;
        (void)handler;
    }
    void clear_write_handler(int fd) override {
        (void)fd;
    }

    bool supports_write_handler() const override {
        return false;
    }

    // Called by completions to deliver to actor
    void deliver_completion(OpCompletion completion);

    /// Set the owning EventLoop (required for deliver_completion).
    void set_loop(class EventLoop* loop) {
        loop_ = loop;
    }

    // --- Extended proactor methods (not in IReactorBackend) ---
    void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type);
    void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type);

  private:
    // Encode fd+actor+op_type into user_data
    static uint64_t encode_user_data(int fd, ActorId actor, uint32_t op_type);
    static void decode_user_data(uint64_t user_data, int& fd, ActorId& actor,
                                 uint32_t& op_type);

    // Submit pending SQEs to kernel
    int submit();

    struct io_uring ring_;

    // File fd → registered index (for IORING_REGISTER_FILES)
    std::unordered_map<int, unsigned> registered_fds_;

    // Registered buffers: buffer_id → (addr, len)
    std::vector<std::pair<const void*, size_t>> registered_buffers_;

    // Timer handle → actor (for cancellation)
    std::unordered_map<uint64_t, ActorId> timer_actors_;
    std::unordered_set<uint64_t> cancelled_timers_;

    // Ops pending submission (not yet submitted to kernel)
    std::vector<struct io_uring_sqe*> pending_sqes_;

    class EventLoop* loop_ = nullptr;

    bool running_ = false;

    std::atomic<uint64_t> next_timer_handle_{1};
};

} // namespace net
} // namespace hpactor