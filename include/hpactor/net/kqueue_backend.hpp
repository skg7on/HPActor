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

#include <hpactor/net/async_io_backend.hpp>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__)
#    include <sys/event.h>
#    include <sys/time.h>
#else
// Stub definitions for non-BSD compilation
struct kevent {
    int ident;
    int filter;
    int flags;
    int fflags;
    int64_t data;
    void* udata;
};
#endif

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hpactor {
namespace net {

// Forward declaration to avoid circular include
class EventLoop;

class KqueueBackend : public AsyncIoBackend {
  public:
    KqueueBackend();
    ~KqueueBackend() override;

    bool start() override;
    void stop() override;

    bool add_fd(int fd, IoEvent events) override;
    bool update_fd(int fd, IoEvent events) override;
    bool remove_fd(int fd) override;

    int register_buffer(const void* addr, size_t len) override;
    bool unregister_buffer(int buffer_id) override;

    void async_send(int fd, const iovec* bufs, int buf_count, ActorId actor,
                    uint32_t op_type) override;
    void async_recv(int fd, const iovec* bufs, int buf_count, ActorId actor,
                    uint32_t op_type) override;

    void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type) override;
    void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type) override;

    void async_accept(int fd, ActorId actor) override;
    void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                       ActorId actor) override;

    void async_recvfrom(int fd, const iovec* bufs, int buf_count, ActorId actor,
                        uint32_t op_type) override;
    void
    async_sendto(int fd, const iovec* bufs, int buf_count, const sockaddr* addr,
                 socklen_t addrlen, ActorId actor, uint32_t op_type) override;

    uint64_t run_after(ActorId actor, int delay_ms) override;
    uint64_t run_every(ActorId actor, int interval_ms) override;
    void cancel_timer(uint64_t handle) override;

    int wait(int timeout_ms) override;
    void process_completions() override;

    // Called by completions to deliver to actor
    void deliver_completion(OpCompletion completion) override;

    // Set the EventLoop pointer for routing completions
    void set_loop(net::EventLoop* loop) {
        loop_ = loop;
    }

    // Read handler management for connection receive
    void set_read_handler(int fd, read_callback handler) override;
    void clear_read_handler(int fd) override;

  private:
    // Timer entry
    struct TimerEntry {
        int64_t expires_at_ms; // absolute time in ms
        ActorId actor;
        int interval_ms; // 0 for one-shot, >0 for repeating
        uint64_t handle;
    };

    // Pending I/O operation tracked per fd
    struct PendingOp {
        ActorId actor;
        uint32_t op_type;
        std::vector<uint8_t> data; // concatenated buffers for send
        int buf_count = 0;
        iovec saved_bufs[16]; // original buffers for recv
        sockaddr_storage addr;
        socklen_t addrlen = 0;
    };

    // Encode fd+actor+op_type into user_data
    static uint64_t encode_user_data(int fd, ActorId actor, uint32_t op_type);
    static void decode_user_data(uint64_t user_data, int& fd, ActorId& actor,
                                 uint32_t& op_type);

    int kqueue_fd_ = -1;

    // EventLoop pointer for routing completions
    net::EventLoop* loop_ = nullptr;

    // Timer management
    std::vector<TimerEntry> timers_; // sorted by expires_at_ms
    std::unordered_set<uint64_t> cancelled_timers_;
    std::unordered_map<uint64_t, size_t> handle_to_timer_index_; // for
                                                                 // cancellation
                                                                 // lookup
    std::atomic<uint64_t> next_timer_handle_{1};

    // fd -> registered events for update tracking
    std::unordered_map<int, uint32_t> fd_events_;

    // fd -> pending I/O operation for edge-triggered async I/O
    std::unordered_map<int, PendingOp> pending_ops_;

    // fd -> actor waiting for accept (listening sockets)
    std::unordered_map<int, ActorId> accept_actors_;

    // fd -> read handler for connection receive
    struct ReadHandler {
        bytes buffer;
        read_callback callback;
    };
    std::unordered_map<int, ReadHandler> read_handlers_;

    // Registered buffers (not supported in kqueue - always empty)
    std::vector<std::pair<const void*, size_t>> registered_buffers_;

    bool running_ = false;

    // Thread safety
    std::mutex mutex_;

    // Pending completions from async_* calls (for process_completions)
    std::vector<OpCompletion> pending_completions_;
    mutable std::mutex completions_mutex_;

    // Storage for kevent array from last wait()
    struct kevent events_[16];
    int last_num_events_ = 0;
};

} // namespace net
} // namespace hpactor
