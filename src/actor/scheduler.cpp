#include <hpactor/scheduler.hpp>

namespace hpactor {

Scheduler::Scheduler(ActorSystem& system, size_t num_threads)
    : system_(system), threads_(num_threads) {}

Scheduler::~Scheduler() {
    stop();
}

void Scheduler::start() {
    running_.store(true, std::memory_order_release);
    for (size_t i = 0; i < threads_.size(); ++i) {
        threads_[i] = std::thread([this, i] { worker_loop(i); });
    }
}

void Scheduler::stop() {
    running_.store(false, std::memory_order_release);
    work_cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void Scheduler::enqueue(ActorId target, MessageVariant /*msg*/) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        work_queue_.push_back({target, next_sequence_.fetch_add(1)});
    }
    work_cv_.notify_one();
}

void Scheduler::worker_loop(size_t /*thread_index*/) {
    while (running_.load(std::memory_order_acquire)) {
        WorkItem item{ActorId{0}, 0};

        {
            std::unique_lock<std::mutex> lock(work_mutex_);
            work_cv_.wait_for(lock, std::chrono::milliseconds(100),
                              [this] { return !work_queue_.empty() || !running_.load(std::memory_order_acquire); });

            if (!running_.load(std::memory_order_acquire)) {
                break;
            }

            if (work_queue_.empty()) {
                continue;
            }

            item = work_queue_.back();
            work_queue_.pop_back();
        }

        process_actor(item.actor_id);
    }
}

void Scheduler::process_actor(ActorId actor_id) {
    auto actor = system_.get_actor(actor_id);
    if (!actor) {
        return;
    }

    // Get the actor's mailbox and try to pop a message
    auto mailbox = system_.get_mailbox(actor_id);
    if (!mailbox) {
        return;
    }

    Message<MessageVariant> msg;
    if (mailbox->try_pop(msg)) {
        // Call the actor's receive with the message payload
        actor->receive(msg.move_payload());
    }
}

} // namespace hpactor
