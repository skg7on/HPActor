#pragma once

#include <hpactor/actor_system.hpp>
#include <hpactor/types.hpp>

#include <atomic>
#include <thread>
#include <vector>

namespace hpactor {

// -----------------------------------------------------------------------------
// Scheduler - thread pool that processes actor mailboxes
// -----------------------------------------------------------------------------
class Scheduler {
public:
    explicit Scheduler(ActorSystem& system, size_t num_threads);
    ~Scheduler();

    // Non-copyable, non-movable
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // Start the scheduler threads
    void start();

    // Stop the scheduler threads
    void stop();

    // Enqueue a message for delivery to an actor
    void enqueue(ActorId target, MessageVariant msg);

    // Check if scheduler is running
    bool is_running() const { return running_.load(std::memory_order_acquire); }

private:
    void worker_loop(size_t thread_index);
    void process_actor(ActorId actor_id);

    ActorSystem& system_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};

    // Work queue for actors that have messages
    // Each actor with a non-empty mailbox gets enqueued here
    struct WorkItem {
        ActorId actor_id;
        uint64_t sequence;  // For FIFO ordering
    };

    std::atomic<uint64_t> next_sequence_{0};

    // Simple work queue using a lock - sufficient for initial implementation
    // TODO: lock-free queue for better scalability
    std::vector<WorkItem> work_queue_;
    std::mutex work_mutex_;
    std::condition_variable work_cv_;
};

} // namespace hpactor
