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

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/mailbox/mpsc_mailbox.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>
#include <functional>
#include <type_traits>

namespace hpactor::mailbox {

// Continuation callback type - called when actor should be resumed
using ActorContinuationCallback = std::function<void()>;

template <typename T> class MPSCActorMailbox {
  public:
    MPSCActorMailbox(ActorId actor_id, sched::IScheduler* scheduler,
                     MailboxConfig config = {}) noexcept
        : actor_id_(actor_id), scheduler_(scheduler), config_(config) {
        if (config_.capacity.max_messages == 0) {
            config_.capacity.max_messages = 1024;
        }
    }

    // Set the continuation callback to resume the actor's coroutine
    void set_continuation_callback(ActorContinuationCallback callback) {
        continuation_callback_ = std::move(callback);
    }

    // Runtime config updates.
    // Not safe to call concurrently with try_push/enqueue.
    void set_config(const MailboxConfig& cfg) noexcept {
        config_ = cfg;
        if (config_.capacity.max_messages == 0) {
            config_.capacity.max_messages = 1024;
        }
    }
    const MailboxConfig& config() const noexcept {
        return config_;
    }

    // Bounded admission: try to enqueue with full feedback.
    //
    // Reserves a slot via CAS, allocates and enqueues the node, then returns
    // an EnqueueResult describing the outcome.  Returns Rejected when the
    // mailbox is at hard capacity.
    EnqueueResult try_push(T&& msg, MailboxEnvelopeMeta meta = {}) noexcept {
        if (meta.estimated_bytes == 0) {
            meta.estimated_bytes = estimate_node_bytes(msg);
        }

        // Primary reservation attempt through normal capacity pool.
        if (!try_reserve(meta.estimated_bytes)) {
            // System messages get a second chance through the protected
            // reserve.
            bool sys_reserved = false;
            if (is_system_message(meta.type_tag)) {
                sys_reserved = try_reserve_system(meta.estimated_bytes);
            }

            if (!sys_reserved) {
                // Apply overflow policy when both pools are exhausted.
                switch (config_.overflow_policy) {
                    case OverflowPolicy::DropNewest:
                        total_dropped_.fetch_add(1, std::memory_order_relaxed);
                        if (metrics_ring_buffer_) [[unlikely]] {
                            metrics::MetricEvent evt{};
                            evt.actor_id = actor_id_;
                            evt.event_type =
                                metrics::MetricEventType::kMailboxDropped;
                            evt.value_hi = 1;
                            metrics_ring_buffer_->try_push(evt);
                        }
                        return make_result(EnqueueResultCode::DroppedNewest);

                    case OverflowPolicy::DropOldest:
                        if (drop_one_oldest()) {
                            // Freed a slot — retry normal reservation.
                            if (!try_reserve(meta.estimated_bytes)) {
                                total_rejected_.fetch_add(
                                    1, std::memory_order_relaxed);
                                if (metrics_ring_buffer_) [[unlikely]] {
                                    metrics::MetricEvent evt{};
                                    evt.actor_id = actor_id_;
                                    evt.event_type =
                                        metrics::MetricEventType::kMailboxRejected;
                                    evt.value_hi = 1;
                                    metrics_ring_buffer_->try_push(evt);
                                }
                                return make_result(EnqueueResultCode::Rejected);
                            }
                            // Fall through to enqueue below.
                            break;
                        }
                        total_rejected_.fetch_add(1, std::memory_order_relaxed);
                        if (metrics_ring_buffer_) [[unlikely]] {
                            metrics::MetricEvent evt{};
                            evt.actor_id = actor_id_;
                            evt.event_type =
                                metrics::MetricEventType::kMailboxRejected;
                            evt.value_hi = 1;
                            metrics_ring_buffer_->try_push(evt);
                        }
                        return make_result(EnqueueResultCode::Rejected);

                    case OverflowPolicy::DeadLetter:
                        total_dead_letters_.fetch_add(1, std::memory_order_relaxed);
                        if (metrics_ring_buffer_) [[unlikely]] {
                            metrics::MetricEvent evt{};
                            evt.actor_id = actor_id_;
                            evt.event_type =
                                metrics::MetricEventType::kMailboxDeadLetter;
                            evt.value_hi = 1;
                            metrics_ring_buffer_->try_push(evt);
                        }
                        return make_result(EnqueueResultCode::ReroutedToDeadLetter);

                    default:
                        // RejectNewest, DropLowestPriority,
                        // SpillToOverflowQueue, SignalOnly,
                        // BlockWhenAllowed
                        total_rejected_.fetch_add(1, std::memory_order_relaxed);
                        if (metrics_ring_buffer_) [[unlikely]] {
                            metrics::MetricEvent evt{};
                            evt.actor_id = actor_id_;
                            evt.event_type =
                                metrics::MetricEventType::kMailboxRejected;
                            evt.value_hi = 1;
                            metrics_ring_buffer_->try_push(evt);
                        }
                        return make_result(EnqueueResultCode::Rejected);
                }
            }
        }

        void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(T), actor_id_);
        auto* node = new (raw) T(std::move(msg));
        enqueue_reserved(node, meta);

        return make_result(pressure_code_after_accept());
    }

    // Convenience: enqueue from a Message<T> rvalue (heap-allocates via custom
    // allocator).  Delegates to try_push so bounded admission is always
    // applied; the result is discarded for callers that don't need feedback.
    void push(T&& msg) noexcept {
        (void)try_push(std::move(msg));
    }

    // Low-level: enqueue an already-allocated node.
    //
    // Attempts reservation before enqueuing.  If the mailbox is at hard
    // capacity the node is NOT enqueued (caller is responsible for the
    // memory).  Prefer try_push() for new code.
    void enqueue(T* node) noexcept {
        uint64_t bytes = estimate_node_bytes(*node);
        if (!try_reserve(bytes)) {
            total_rejected_.fetch_add(1, std::memory_order_relaxed);
            if (metrics_ring_buffer_) [[unlikely]] {
                metrics::MetricEvent evt{};
                evt.actor_id = actor_id_;
                evt.event_type = metrics::MetricEventType::kMailboxRejected;
                evt.value_hi = 1;
                metrics_ring_buffer_->try_push(evt);
            }
            return;
        }
        MailboxEnvelopeMeta meta;
        meta.estimated_bytes = bytes;
        enqueue_reserved(node, meta);
    }

    // Enqueue an already-reserved node (reservation was done externally).
    void enqueue_reserved(T* node, const MailboxEnvelopeMeta& meta) noexcept {
        bool was_empty = empty();
        mailbox_.enqueue(node);
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        update_max_depth();
        update_pressure_state();

        int64_t depth = mailbox_.count();
        if (depth > 1024) [[unlikely]] {
            HPACTOR_LOG_WARNING(
                log::LogCategory::kMailbox, actor_id_,
                static_cast<uint32_t>(log::LogEventId::kMailboxDepthHigh),
                "mailbox depth high",
                log::field("depth", static_cast<uint64_t>(depth)));
        }

        if (metrics_ring_buffer_) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = actor_id_;
            evt.event_type = metrics::MetricEventType::kMailboxEnqueue;
            evt.value_hi = 1;
            metrics_ring_buffer_->try_push(evt);
        }
        if (was_empty) {
            bool expected = true;
            if (mailbox_was_empty_.compare_exchange_strong(
                    expected, false, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                // Directly resume the actor's continuation if available
                // This avoids the latency of queuing and later pickup
                if (continuation_callback_) {
                    continuation_callback_();
                }
                // Also notify scheduler for bookkeeping and potential requeue
                scheduler_->notify_ready(actor_id_, meta.priority, meta.deadline_ns);
            }
        }
    }

    // Consumer: dequeue one message
    T* dequeue() noexcept {
        T* node = mailbox_.dequeue();
        if (node != nullptr) {
            // Release from the correct reservation pool.
            if constexpr (std::is_same_v<T, TypedMessage>) {
                if (is_system_message(node->type_id()) &&
                    reserved_system_messages_.load(std::memory_order_acquire) > 0) {
                    release_system_reservation(estimate_node_bytes(*node));
                } else {
                    release_reservation(estimate_node_bytes(*node));
                }
            } else {
                release_reservation(estimate_node_bytes(*node));
            }
            total_dequeued_.fetch_add(1, std::memory_order_relaxed);
            if (empty()) {
                mailbox_was_empty_.store(true, std::memory_order_release);
            }
        }
        if (metrics_ring_buffer_) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = actor_id_;
            evt.event_type = metrics::MetricEventType::kMailboxDequeue;
            evt.value_hi = 1;
            metrics_ring_buffer_->try_push(evt);
        }
        return node;
    }

    // Non-blocking pop matching ActorMailbox interface
    bool try_pop(T& out) noexcept {
        T* node = dequeue();
        if (!node)
            return false;
        out = std::move(*node);
        node->~T();
        mem::deallocate(node);
        return true;
    }

    bool empty() const noexcept {
        return mailbox_.empty();
    }

    // For MailboxAwaiter: was_empty before suspension?
    bool was_empty() const noexcept {
        return mailbox_was_empty_.load(std::memory_order_acquire);
    }

    // Reset edge-trigger (called when actor suspends via await_suspend)
    void set_was_empty(bool val) noexcept {
        mailbox_was_empty_.store(val, std::memory_order_release);
    }

    void
    set_metrics_ring_buffer(metrics::MpscRingBuffer<metrics::MetricEvent>* buf) noexcept {
        metrics_ring_buffer_ = buf;
    }

    void set_logger(log::Logger* logger) noexcept {
        logger_ = logger;
    }

    // Inject a message for testing (bypasses scheduler notify_ready).
    // Must update reservation counters to keep dequeue accounting consistent.
    void inject_for_test(T* node) noexcept {
        reserved_messages_.fetch_add(1, std::memory_order_relaxed);
        queued_bytes_.fetch_add(estimate_node_bytes(*node),
                                std::memory_order_release);
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        mailbox_.enqueue(node);
        mailbox_was_empty_.store(false, std::memory_order_release);
    }

    // Return a snapshot of current mailbox stats.
    cli::MboxSnapshot snapshot() const {
        cli::MboxSnapshot s;
        s.depth = static_cast<uint32_t>(mailbox_.count());
        s.capacity = config_.capacity.max_messages;
        s.queued_bytes = queued_bytes_.load(std::memory_order_acquire);
        s.byte_capacity = config_.capacity.max_bytes;

        if (s.capacity > 0) {
            double ratio =
                static_cast<double>(s.depth) / static_cast<double>(s.capacity);
            s.pressure_ratio_ppm = static_cast<uint32_t>(ratio * 1'000'000.0);
        }

        s.total_enqueued = total_enqueued_.load(std::memory_order_acquire);
        s.total_dequeued = total_dequeued_.load(std::memory_order_acquire);
        s.total_rejected = total_rejected_.load(std::memory_order_acquire);
        s.total_dropped = total_dropped_.load(std::memory_order_acquire);
        s.total_dead_letters = total_dead_letters_.load(std::memory_order_acquire);
        s.max_depth = max_depth_.load(std::memory_order_acquire);
        s.high_priority_depth = 0;
        s.pressure_state =
            to_string(pressure_state_.load(std::memory_order_acquire));
        s.overflow_policy = to_string(config_.overflow_policy);
        return s;
    }

  private:
    // Try to reserve one message slot + byte budget via CAS.
    // Two-phase reservation: count first, then bytes with rollback on failure.
    // Returns false when at hard count or byte capacity.
    bool try_reserve(uint64_t bytes) noexcept {
        uint32_t msg_cap = config_.capacity.max_messages;
        uint64_t byte_cap = config_.capacity.max_bytes;

        // Phase 1: count reservation (unchanged logic).
        if (msg_cap > 0) {
            uint32_t cur = reserved_messages_.load(std::memory_order_acquire);
            do {
                if (cur >= msg_cap)
                    return false;
            } while (!reserved_messages_.compare_exchange_weak(
                cur, cur + 1, std::memory_order_acq_rel,
                std::memory_order_acquire));
        }

        // Phase 2: byte budget reservation.
        if (byte_cap > 0) {
            uint64_t cur = queued_bytes_.load(std::memory_order_acquire);
            do {
                if (cur + bytes > byte_cap) {
                    if (msg_cap > 0)
                        reserved_messages_.fetch_sub(1,
                                                     std::memory_order_release);
                    return false;
                }
            } while (!queued_bytes_.compare_exchange_weak(
                cur, cur + bytes, std::memory_order_acq_rel,
                std::memory_order_acquire));
            return true;
        }

        // Unlimited bytes: still track for observability.
        queued_bytes_.fetch_add(bytes, std::memory_order_release);
        return true;
    }

    // Release a previously reserved slot + bytes.
    void release_reservation(uint64_t bytes) noexcept {
        reserved_messages_.fetch_sub(1, std::memory_order_release);
        if (bytes > 0) {
            queued_bytes_.fetch_sub(bytes, std::memory_order_release);
        }
    }

    // Try to reserve a system message slot via CAS on the protected reserve.
    // Only used when the normal capacity pool is exhausted.
    // System messages bypass the byte budget but still track bytes for
    // snapshot accuracy — the count cap (default 32) already bounds memory.
    bool try_reserve_system(uint64_t bytes) noexcept {
        uint32_t limit = config_.protected_system_messages;
        if (limit == 0)
            return false;

        uint32_t current =
            reserved_system_messages_.load(std::memory_order_acquire);
        while (true) {
            if (current >= limit) {
                return false;
            }
            if (reserved_system_messages_.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                queued_bytes_.fetch_add(bytes, std::memory_order_release);
                return true;
            }
        }
    }

    // Release a previously reserved system slot + bytes.
    void release_system_reservation(uint64_t bytes) noexcept {
        reserved_system_messages_.fetch_sub(1, std::memory_order_release);
        if (bytes > 0) {
            queued_bytes_.fetch_sub(bytes, std::memory_order_release);
        }
    }

    // Drop the oldest message from the mailbox to free a slot.
    // Returns true if a message was successfully dropped.
    bool drop_one_oldest() noexcept {
        T* node = mailbox_.dequeue();
        if (!node)
            return false;

        // Release from the correct pool and update drop counter.
        if constexpr (std::is_same_v<T, TypedMessage>) {
            if (is_system_message(node->type_id()) &&
                reserved_system_messages_.load(std::memory_order_acquire) > 0) {
                release_system_reservation(estimate_node_bytes(*node));
            } else {
                release_reservation(estimate_node_bytes(*node));
            }
        } else {
            release_reservation(estimate_node_bytes(*node));
        }

        total_dropped_.fetch_add(1, std::memory_order_relaxed);
        if (metrics_ring_buffer_) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = actor_id_;
            evt.event_type = metrics::MetricEventType::kMailboxDropped;
            evt.value_hi = 1;
            metrics_ring_buffer_->try_push(evt);
        }

        node->~T();
        mem::deallocate(node);

        if (empty()) {
            mailbox_was_empty_.store(true, std::memory_order_release);
        }

        return true;
    }

    // Determine the result code after accepting a message, based on current
    // watermarks.
    EnqueueResultCode pressure_code_after_accept() const noexcept {
        uint32_t cap = config_.capacity.max_messages;
        if (cap == 0)
            return EnqueueResultCode::Accepted;
        uint32_t depth = static_cast<uint32_t>(mailbox_.count());
        double ratio = static_cast<double>(depth) / static_cast<double>(cap);
        if (ratio >= config_.high_watermark) {
            return EnqueueResultCode::AcceptedWithSoftPressure;
        }
        return EnqueueResultCode::Accepted;
    }

    // Fill an EnqueueResult from the given code and current state.
    EnqueueResult make_result(EnqueueResultCode code) const noexcept {
        EnqueueResult r;
        r.code = code;
        r.target = actor_id_;
        r.depth = static_cast<uint32_t>(mailbox_.count());
        r.capacity = config_.capacity.max_messages;
        if (r.capacity > 0) {
            r.pressure_ratio =
                static_cast<double>(r.depth) / static_cast<double>(r.capacity);
        }
        return r;
    }

    // Update max_depth_ tracking via CAS.
    void update_max_depth() noexcept {
        uint64_t depth = static_cast<uint64_t>(mailbox_.count());
        uint64_t prev = max_depth_.load(std::memory_order_acquire);
        while (depth > prev) {
            if (max_depth_.compare_exchange_weak(prev, depth, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                break;
            }
        }
    }

    // Update pressure state based on current depth vs watermarks.
    void update_pressure_state() noexcept {
        uint32_t cap = config_.capacity.max_messages;
        if (cap == 0) {
            pressure_state_.store(MailboxPressureState::Normal,
                                  std::memory_order_release);
            return;
        }
        uint32_t depth = static_cast<uint32_t>(mailbox_.count());
        double ratio = static_cast<double>(depth) / static_cast<double>(cap);
        if (ratio >= config_.high_watermark) {
            pressure_state_.store(MailboxPressureState::SoftPressure,
                                  std::memory_order_release);
        } else {
            pressure_state_.store(MailboxPressureState::Normal,
                                  std::memory_order_release);
        }
    }

    // Estimate bytes for a node.  Uses the TypedMessage-specific helper when T
    // is TypedMessage, otherwise falls back to sizeof(T).
    static uint64_t estimate_node_bytes(const T& node) noexcept {
        if constexpr (std::is_same_v<T, TypedMessage>) {
            return estimate_message_bytes(node);
        } else {
            return sizeof(T);
        }
    }

    ActorId actor_id_;
    sched::IScheduler* scheduler_;
    MPSCMailbox<T> mailbox_;
    MailboxConfig config_;
    std::atomic<bool> mailbox_was_empty_{true};
    std::atomic<uint32_t> reserved_messages_{0};
    std::atomic<uint32_t> reserved_system_messages_{0};
    std::atomic<uint64_t> queued_bytes_{0};
    std::atomic<uint64_t> total_enqueued_{0};
    std::atomic<uint64_t> total_dequeued_{0};
    std::atomic<uint64_t> total_rejected_{0};
    std::atomic<uint64_t> total_dropped_{0};
    std::atomic<uint64_t> total_dead_letters_{0};
    std::atomic<uint64_t> max_depth_{0};
    std::atomic<MailboxPressureState> pressure_state_{MailboxPressureState::Normal};
    ActorContinuationCallback continuation_callback_;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_{nullptr};
    log::Logger* logger_ = nullptr;
};

} // namespace hpactor::mailbox
