#pragma once

#include <hpactor/net/async_io_backend.hpp>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/timerfd.h>
#else
// Stub definitions for non-Linux compilation
struct epoll_event { uint32_t events; void* ptr; };
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

class EpollBackend : public AsyncIoBackend {
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

    void async_send(int fd, const iovec* bufs, int buf_count,
                    ActorId actor, uint32_t op_type) override;
    void async_recv(int fd, const iovec* bufs, int buf_count,
                    ActorId actor, uint32_t op_type) override;

    void async_send_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type) override;
    void async_recv_fixed(int fd, int buffer_id, size_t offset, size_t len,
                          ActorId actor, uint32_t op_type) override;

    void async_accept(int fd, ActorId actor) override;
    void async_connect(int fd, const sockaddr* addr, socklen_t addrlen,
                        ActorId actor) override;

    void async_recvfrom(int fd, const iovec* bufs, int buf_count,
                         ActorId actor, uint32_t op_type) override;
    void async_sendto(int fd, const iovec* bufs, int buf_count,
                        const sockaddr* addr, socklen_t addrlen,
                        ActorId actor, uint32_t op_type) override;

    uint64_t run_after(ActorId actor, int delay_ms) override;
    uint64_t run_every(ActorId actor, int interval_ms) override;
    void cancel_timer(uint64_t handle) override;

    int wait(int timeout_ms) override;
    void process_completions() override;

    // Called by completions to deliver to actor
    void deliver_completion(OpCompletion completion) override;

    // Set the EventLoop pointer for routing completions
    void set_loop(net::EventLoop* loop) { loop_ = loop; }

private:
    // Timer entry
    struct TimerEntry {
        int64_t expires_at_ms;  // absolute time in ms
        ActorId actor;
        int interval_ms;  // 0 for one-shot, >0 for repeating
        uint64_t handle;
    };

    // Encode fd+actor+op_type into user_data
    static uint64_t encode_user_data(int fd, ActorId actor, uint32_t op_type);
    static void decode_user_data(uint64_t user_data, int& fd, ActorId& actor, uint32_t& op_type);

    // Process expired timers, returns number of triggered timers
    int process_timers();

    int epoll_fd_ = -1;
    int timerfd_ = -1;

    // EventLoop pointer for routing completions
    net::EventLoop* loop_ = nullptr;

    // Timer management
    std::vector<TimerEntry> timers_;  // sorted by expires_at_ms
    std::unordered_set<uint64_t> cancelled_timers_;
    std::unordered_map<uint64_t, size_t> handle_to_timer_index_;  // for cancellation lookup
    std::atomic<uint64_t> next_timer_handle_{1};

    // fd -> registered events for update tracking
    std::unordered_map<int, uint32_t> fd_events_;

    // Registered buffers (not supported in epoll - always empty)
    std::vector<std::pair<const void*, size_t>> registered_buffers_;

    bool running_ = false;

    // Thread safety
    std::mutex mutex_;
};

} // namespace net
} // namespace hpactor
