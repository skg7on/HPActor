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
#include <hpactor/mailbox/overflow_queue.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>
#include <functional>
#include <optional>
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
        overflow_queue_.set_max_depth(config_.max_overflow_depth);
    }

    ~MPSCActorMailbox() {
        if (pending_free_) {
            pending_free_->~T();
            mem::deallocate(pending_free_);
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
        // Guard against misconfiguration: critical_watermark <= 0 would
        // cause perpetual HardPressure (ratio >= 0 is always true).
        if (config_.critical_watermark <= 0.0) {
            config_.critical_watermark = (config_.high_watermark > 0.0)
                                              ? config_.high_watermark * 2.0
                                              : 1.0;
        }
        if (config_.critical_watermark < config_.high_watermark) {
            config_.critical_watermark = config_.high_watermark;
        }
        overflow_queue_.set_max_depth(config_.max_overflow_depth);
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
        auto reserve_result = try_reserve(meta.estimated_bytes);
        if (reserve_result != ReservationResult::Reserved) {
            // System messages get a second chance through the protected
            // reserve.
            bool sys_reserved = false;
            if (is_system_message(meta.type_tag)) {
                sys_reserved = try_reserve_system(meta.estimated_bytes);
            }

            if (!sys_reserved) {
                update_pressure_state(true);
                auto reserve_reason = reserve_result == ReservationResult::ByteCapacity
                                          ? BackpressureReason::ByteCapacity
                                          : BackpressureReason::HardCapacity;

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
                        return make_result(EnqueueResultCode::DroppedNewest,
                                           BackpressureReason::OverflowPolicy);

                    case OverflowPolicy::DropOldest:
                        if (drop_one_oldest()) {
                            // Freed a slot — retry normal reservation.
                            if (try_reserve(meta.estimated_bytes) !=
                                ReservationResult::Reserved) {
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
                                return make_result(EnqueueResultCode::Rejected,
                                                   reserve_reason);
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
                        return make_result(EnqueueResultCode::Rejected,
                                           reserve_reason);

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
                        return make_result(EnqueueResultCode::ReroutedToDeadLetter,
                                           BackpressureReason::OverflowPolicy);

                    case OverflowPolicy::RejectNewest:
                        total_rejected_.fetch_add(1, std::memory_order_relaxed);
                        if (metrics_ring_buffer_) [[unlikely]] {
                            metrics::MetricEvent evt{};
                            evt.actor_id = actor_id_;
                            evt.event_type =
                                metrics::MetricEventType::kMailboxRejected;
                            evt.value_hi = 1;
                            metrics_ring_buffer_->try_push(evt);
                        }
                        return make_result(EnqueueResultCode::Rejected,
                                           reserve_reason);

                    case OverflowPolicy::SignalOnly: {
                        total_rejected_.fetch_add(1, std::memory_order_relaxed);
                        if (metrics_ring_buffer_) [[unlikely]] {
                            metrics::MetricEvent evt{};
                            evt.actor_id = actor_id_;
                            evt.event_type =
                                metrics::MetricEventType::kMailboxRejected;
                            evt.value_hi = 1;
                            metrics_ring_buffer_->try_push(evt);
                        }
                        auto result = make_result(EnqueueResultCode::Rejected,
                                                  reserve_reason);
                        result.retry_after = std::chrono::milliseconds(
                            config_.signal_min_interval_ms);
                        return result;
                    }

                    case OverflowPolicy::SpillToOverflowQueue: {
                        if (overflow_queue_.try_push(std::move(msg))) {
                            return make_result(EnqueueResultCode::ReroutedToOverflow,
                                               reserve_reason);
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
                        return make_result(EnqueueResultCode::Rejected,
                                           reserve_reason);
                    }

                    default:
                        // DropLowestPriority, BlockWhenAllowed — not yet
                        // implemented; reject with full observability.
                        total_rejected_.fetch_add(1, std::memory_order_relaxed);
                        if (metrics_ring_buffer_) [[unlikely]] {
                            metrics::MetricEvent evt{};
                            evt.actor_id = actor_id_;
                            evt.event_type =
                                metrics::MetricEventType::kMailboxRejected;
                            evt.value_hi = 1;
                            metrics_ring_buffer_->try_push(evt);
                        }
                        return make_result(EnqueueResultCode::Rejected,
                                           reserve_reason);
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
        if (try_reserve(bytes) != ReservationResult::Reserved) {
            update_pressure_state(true);
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
    //
    // When suppress_wakeup is true, skips the continuation callback and
    // scheduler notification — used by drain_overflow() to avoid re-entrant
    // coroutine resume while the actor is already active on the call stack.
    void enqueue_reserved(T* node, const MailboxEnvelopeMeta& meta,
                          bool suppress_wakeup = false) noexcept {
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
        if (was_empty && !suppress_wakeup) {
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

    // Consumer: dequeue one message.
    //
    // Acquires the consumer spinlock so that only one thread (scheduler
    // dispatch or overflow-driven drop_one_oldest) operates on the
    // underlying MPSC queue at a time.
    //
    // The returned node is owned by the caller, who must free it via
    // mem::deallocate (see try_pop).
    T* dequeue() noexcept {
        lock_consumer();
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
            update_pressure_state();
            if (empty()) {
                mailbox_was_empty_.store(true, std::memory_order_release);
            }

            // Drain from overflow queue back into main mailbox now that a
            // slot has freed up.
            drain_overflow();
        }
        unlock_consumer();

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

        double ratio = pressure_ratio();
        s.pressure_ratio_ppm = static_cast<uint32_t>(ratio * 1'000'000.0);

        s.total_enqueued = total_enqueued_.load(std::memory_order_acquire);
        s.total_dequeued = total_dequeued_.load(std::memory_order_acquire);
        s.total_rejected = total_rejected_.load(std::memory_order_acquire);
        s.total_dropped = total_dropped_.load(std::memory_order_acquire);
        s.total_dead_letters = total_dead_letters_.load(std::memory_order_acquire);
        s.max_depth = max_depth_.load(std::memory_order_acquire);
        {
            auto oq_snap = overflow_queue_.snapshot();
            s.overflow_depth = oq_snap.depth;
            s.overflow_max_depth = oq_snap.max_depth;
            s.overflow_total_pushed = oq_snap.total_pushed;
            s.overflow_total_popped = oq_snap.total_popped;
            s.overflow_total_lost = oq_snap.total_lost;
        }
        s.high_priority_depth = 0;
        s.pressure_state =
            to_string(pressure_state_.load(std::memory_order_acquire));
        s.overflow_policy = to_string(config_.overflow_policy);
        return s;
    }

    std::optional<uint64_t>
    try_acquire_backpressure_signal(uint64_t now_ns,
                                    MailboxPressureState state,
                                    bool force = false) noexcept {
        const uint64_t interval_ns =
            static_cast<uint64_t>(config_.signal_min_interval_ms) * 1'000'000ULL;
        const auto severity = pressure_severity(state);

        uint64_t last =
            last_backpressure_signal_ns_.load(std::memory_order_acquire);
        uint8_t last_severity =
            last_backpressure_signal_severity_.load(std::memory_order_acquire);

        while (true) {
            const bool first = last == 0;
            const bool interval_elapsed = now_ns >= last + interval_ns;
            const bool escalation = severity > last_severity;

            if (!force && !first && !interval_elapsed && !escalation) {
                return std::nullopt;
            }

            if (last_backpressure_signal_ns_.compare_exchange_weak(
                    last, now_ns, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                last_backpressure_signal_severity_.store(
                    severity, std::memory_order_release);
                return backpressure_signal_sequence_.fetch_add(
                           1, std::memory_order_acq_rel) +
                       1;
            }

            last_severity =
                last_backpressure_signal_severity_.load(std::memory_order_acquire);
        }
    }

  private:
    static uint8_t pressure_severity(MailboxPressureState state) noexcept {
        switch (state) {
            case MailboxPressureState::Normal:
                return 0;
            case MailboxPressureState::Recovering:
                return 1;
            case MailboxPressureState::SoftPressure:
                return 2;
            case MailboxPressureState::HardPressure:
                return 3;
        }
        return 0;
    }

    enum class ReservationResult : uint8_t {
        Reserved,
        CountCapacity,
        ByteCapacity,
    };

    // Try to reserve one message slot + byte budget via CAS.
    // Two-phase reservation: count first, then bytes with rollback on failure.
    // Returns the specific capacity reason on failure.
    ReservationResult try_reserve(uint64_t bytes) noexcept {
        uint32_t msg_cap = config_.capacity.max_messages;
        uint64_t byte_cap = config_.capacity.max_bytes;

        // Phase 1: count reservation (unchanged logic).
        if (msg_cap > 0) {
            uint32_t cur = reserved_messages_.load(std::memory_order_acquire);
            do {
                if (cur >= msg_cap)
                    return ReservationResult::CountCapacity;
            } while (!reserved_messages_.compare_exchange_weak(
                cur, cur + 1, std::memory_order_acq_rel, std::memory_order_acquire));
        }

        // Phase 2: byte budget reservation.
        if (byte_cap > 0) {
            uint64_t cur = queued_bytes_.load(std::memory_order_acquire);
            do {
                if (cur + bytes > byte_cap) {
                    if (msg_cap > 0)
                        reserved_messages_.fetch_sub(1, std::memory_order_release);
                    return ReservationResult::ByteCapacity;
                }
            } while (!queued_bytes_.compare_exchange_weak(
                cur, cur + bytes, std::memory_order_acq_rel,
                std::memory_order_acquire));
            return ReservationResult::Reserved;
        }

        // Unlimited bytes: still track for observability.
        queued_bytes_.fetch_add(bytes, std::memory_order_release);
        return ReservationResult::Reserved;
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
    // Uses deferred free: the dropped node is kept alive until the next
    // consumer operation to avoid a stale head_ write from producers.
    bool drop_one_oldest() noexcept {
        lock_consumer();
        T* node = mailbox_.dequeue();
        if (!node) {
            unlock_consumer();
            return false;
        }

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
        update_pressure_state();
        if (metrics_ring_buffer_) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = actor_id_;
            evt.event_type = metrics::MetricEventType::kMailboxDropped;
            evt.value_hi = 1;
            metrics_ring_buffer_->try_push(evt);
        }

        if (empty()) {
            mailbox_was_empty_.store(true, std::memory_order_release);
        }
        unlock_consumer();

        // Deferred free: the previously dropped node is safe to reclaim.
        if (pending_free_) {
            pending_free_->~T();
            mem::deallocate(pending_free_);
        }
        pending_free_ = node;

        return true;
    }

    double pressure_ratio() const noexcept {
        const uint32_t cap = config_.capacity.max_messages;
        const uint32_t depth = static_cast<uint32_t>(mailbox_.count());
        double count_ratio = 0.0;
        if (cap > 0) {
            count_ratio = static_cast<double>(depth) / static_cast<double>(cap);
        }

        double byte_ratio = 0.0;
        const uint64_t byte_cap = config_.capacity.max_bytes;
        if (byte_cap > 0) {
            byte_ratio =
                static_cast<double>(queued_bytes_.load(std::memory_order_acquire)) /
                static_cast<double>(byte_cap);
        }

        return count_ratio > byte_ratio ? count_ratio : byte_ratio;
    }

    MailboxPressureState
    next_pressure_state(double ratio, bool hard_failure) const noexcept {
        if (hard_failure || ratio >= config_.critical_watermark) {
            return MailboxPressureState::HardPressure;
        }

        auto current = pressure_state_.load(std::memory_order_acquire);
        if (current == MailboxPressureState::HardPressure ||
            current == MailboxPressureState::Recovering) {
            if (ratio < config_.low_watermark) {
                return MailboxPressureState::Normal;
            }
            return MailboxPressureState::Recovering;
        }

        if (ratio >= config_.high_watermark) {
            return MailboxPressureState::SoftPressure;
        }
        if (ratio < config_.low_watermark) {
            return MailboxPressureState::Normal;
        }
        return current;
    }

    // Determine the result code after accepting a message, based on current
    // watermarks.
    EnqueueResultCode pressure_code_after_accept() const noexcept {
        auto state = pressure_state_.load(std::memory_order_acquire);
        if (state == MailboxPressureState::SoftPressure ||
            state == MailboxPressureState::HardPressure ||
            state == MailboxPressureState::Recovering) {
            return EnqueueResultCode::AcceptedWithSoftPressure;
        }
        return EnqueueResultCode::Accepted;
    }

    // Fill an EnqueueResult from the given code and current state.
    EnqueueResult
    make_result(EnqueueResultCode code,
                BackpressureReason reason = BackpressureReason::HighWatermark) const noexcept {
        EnqueueResult r;
        r.code = code;
        r.target = actor_id_;
        r.depth = static_cast<uint32_t>(mailbox_.count());
        r.capacity = config_.capacity.max_messages;
        r.bytes = queued_bytes_.load(std::memory_order_acquire);
        r.byte_capacity = config_.capacity.max_bytes;
        r.pressure_ratio = pressure_ratio();
        r.pressure_reason = reason;
        r.pressure_state = pressure_state_.load(std::memory_order_acquire);

        auto base = std::chrono::milliseconds(config_.signal_min_interval_ms);
        if (r.pressure_state == MailboxPressureState::HardPressure) {
            r.retry_after = base * 2;
        } else if (r.pressure_state == MailboxPressureState::SoftPressure ||
                   r.pressure_state == MailboxPressureState::Recovering) {
            r.retry_after = base;
        }
        return r;
    }

    // Drain messages from overflow queue back into the main mailbox.
    // Called after each dequeue when capacity may have freed up.
    //
    // Uses suppress_wakeup to avoid re-entrant coroutine resume while the
    // actor is already active on the dequeue() call stack.
    void drain_overflow() noexcept {
        while (config_.overflow_policy == OverflowPolicy::SpillToOverflowQueue) {
            if (try_reserve(0) != ReservationResult::Reserved)
                break;
            T overflow_msg;
            if (!overflow_queue_.try_pop(overflow_msg)) {
                release_reservation(0);
                break;
            }
            MailboxEnvelopeMeta meta;
            meta.estimated_bytes = estimate_node_bytes(overflow_msg);
            enqueue_reserved(new (mem::allocate(mem::RegionType::kMessage,
                                                sizeof(T), actor_id_))
                                 T(std::move(overflow_msg)),
                             meta, /*suppress_wakeup=*/true);
        }
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
    void update_pressure_state(bool hard_failure = false) noexcept {
        const double ratio = pressure_ratio();
        pressure_state_.store(next_pressure_state(ratio, hard_failure),
                              std::memory_order_release);
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

    // Consumer spinlock: serialises dequeue() and drop_one_oldest() so
    // the underlying Vyukov MPSC queue sees exactly one consumer.
    void lock_consumer() noexcept {
        while (consumer_lock_.test_and_set(std::memory_order_acquire)) {
            // spin — the critical section is a single dequeue (tens of ns)
        }
    }
    void unlock_consumer() noexcept {
        consumer_lock_.clear(std::memory_order_release);
    }

    ActorId actor_id_;
    sched::IScheduler* scheduler_;
    MPSCMailbox<T> mailbox_;
    MailboxConfig config_;
    std::atomic_flag consumer_lock_ = ATOMIC_FLAG_INIT;
    T* pending_free_{nullptr}; // deferred-free: freed on next consumer op
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
    std::atomic<uint64_t> last_backpressure_signal_ns_{0};
    std::atomic<uint8_t> last_backpressure_signal_severity_{0};
    std::atomic<uint64_t> backpressure_signal_sequence_{0};
    ActorContinuationCallback continuation_callback_;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_{nullptr};
    log::Logger* logger_ = nullptr;
    OverflowQueue<T> overflow_queue_;
};

} // namespace hpactor::mailbox
