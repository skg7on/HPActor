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

#if defined(__linux__)
#    include <sys/epoll.h>
#    include <sys/timerfd.h>
#else
// Stub definitions for non-Linux compilation
struct epoll_event {
    uint32_t events;
    void* ptr;
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

class EpollBackend : public IReactorBackend {
  public:
    EpollBackend();
    ~EpollBackend() override;

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

    // Read handler management — epoll reads data in wait() and dispatches
    void set_read_handler(int fd, read_callback handler) override;
    void clear_read_handler(int fd) override;

    bool supports_read_handler() const override { return true; }

    // Set the EventLoop pointer for routing completions
    void set_loop(net::EventLoop* loop) {
        loop_ = loop;
    }

    // Called by completions to deliver to actor
    void deliver_completion(OpCompletion completion);

    // --- Extended proactor methods (not in IReactorBackend) ---
    void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type);
    void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type);

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
        iovec saved_bufs[16];  // original buffers for recv
        sockaddr_storage addr; // for connect/recvfrom/sendto
        socklen_t addrlen = sizeof(addr);
    };

    // Encode fd+actor+op_type into user_data
    static uint64_t encode_user_data(int fd, ActorId actor, uint32_t op_type);
    static void decode_user_data(uint64_t user_data, int& fd, ActorId& actor,
                                 uint32_t& op_type);

    // Process expired timers, returns number of triggered timers
    int process_timers();

    // Process a pending I/O operation for a socket event
    void process_pending_op(int fd, uint32_t events);

    // Dispatch a single epoll event to the appropriate handler
    void dispatch_event(int fd, uint32_t events);

    // Sub-methods for process_pending_op — each handles one op type
    void try_pending_accept(int fd, PendingOp& op);
    bool try_pending_connect(int fd, PendingOp& op, uint32_t events, int& error);
    bool try_pending_send(int fd, PendingOp& op, uint32_t events,
                          ssize_t& total, int& error);
    bool try_pending_recv(int fd, PendingOp& op, uint32_t events,
                          ssize_t& total, int& error);

    // Enqueue an OpCompletion for a completed pending operation
    void deliver_op_completion(const PendingOp& op, OpType optype, int fd,
                               ssize_t total, int error);

    int epoll_fd_ = -1;
    int timerfd_ = -1;

    // EventLoop pointer for routing completions
    net::EventLoop* loop_ = nullptr;

    // Timer management
    std::vector<TimerEntry> timers_; // sorted by expires_at_ms
    std::unordered_set<uint64_t> cancelled_timers_;
    std::unordered_map<uint64_t, size_t> handle_to_timer_index_;
    std::atomic<uint64_t> next_timer_handle_{1};

    // fd -> registered events for update tracking
    std::unordered_map<int, uint32_t> fd_events_;

    // fd -> pending I/O operation for true async I/O
    std::unordered_map<int, PendingOp> pending_ops_;

    // Registered buffers (not supported in epoll - always empty)
    std::vector<std::pair<const void*, size_t>> registered_buffers_;

    bool running_ = false;

    // Thread safety
    std::mutex mutex_;

    // Pending completions from async_* calls (for process_completions)
    std::vector<OpCompletion> pending_completions_;
    mutable std::mutex completions_mutex_;

    // Read handler callbacks for PlainConnection-style consumers
    std::unordered_map<int, read_callback> read_handlers_;

    // Read data from fd and dispatch to registered read handler
    void service_read_handler(int fd);
};

} // namespace net
} // namespace hpactor