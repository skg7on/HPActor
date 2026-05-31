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
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/detail/backpressure_signal_gate.hpp>
#include <hpactor/mailbox/detail/overflow_handler_factory.hpp>
#include <hpactor/mailbox/detail/pressure_state_machine.hpp>
#include <hpactor/mailbox/detail/reservation_manager.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/mailbox/mpsc_mailbox.hpp>
#include <hpactor/mailbox/multi_lane_queue.hpp>
#include <hpactor/mailbox/overflow_queue.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

namespace hpactor::mailbox {

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
        overflow_handler_ =
            detail::make_overflow_handler<T>(config_.overflow_policy);
    }

    ~MPSCActorMailbox() {
        T* p = lanes_.release_pending_free();
        if (p) {
            p->~T();
            mem::deallocate(p);
        }
    }

    void set_continuation_callback(ActorContinuationCallback callback) {
        continuation_callback_ = std::move(callback);
    }

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
        overflow_handler_ =
            detail::make_overflow_handler<T>(config_.overflow_policy);
        lanes_.set_num_user_lanes(config_.priority_levels);
    }

    const MailboxConfig& config() const noexcept {
        return config_;
    }

    EnqueueResult try_push(T&& msg, MailboxEnvelopeMeta meta = {}) noexcept {
        FAULT_INJECT("hpactor.mailbox.try_push.fail") {
            EnqueueResult r;
            r.code = EnqueueResultCode::Rejected;
            r.target = actor_id_;
            return r;
        }
        if (meta.estimated_bytes == 0) {
            meta.estimated_bytes = estimate_node_bytes(msg);
        }

        uint8_t lane = route_lane(meta);

        // System messages use the dedicated system lane.
        if (lane == MultiLaneQueue<T>::kSystemLaneSentinel) {
            if (static_cast<uint32_t>(
                    lanes_.lane_depth(MultiLaneQueue<T>::kSystemLaneSentinel))
                >= config_.protected_system_messages) {
                update_pressure_state(/*hard_failure=*/true);
                total_rejected_.fetch_add(1, std::memory_order_relaxed);
                EnqueueResult r;
                r.code = EnqueueResultCode::Rejected;
                r.target = actor_id_;
                r.depth = static_cast<uint32_t>(lanes_.total_depth());
                r.capacity = config_.capacity.max_messages;
                r.bytes = reservation_.queued_bytes();
                r.byte_capacity = config_.capacity.max_bytes;
                r.pressure_ratio = pressure_ratio();
                r.pressure_state = pressure_state_.current_state();
                return r;
            }
            void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(T), actor_id_);
            auto* node = new (raw) T(std::move(msg));
            enqueue_reserved(node, meta,
                             MultiLaneQueue<T>::kSystemLaneSentinel);
            return make_result(pressure_state_.code_after_accept());
        }

        // User messages: reserve capacity, then enqueue to the routed lane.
        auto reserve_result = reservation_.try_reserve(
            meta.estimated_bytes, config_.capacity.max_messages,
            config_.capacity.max_bytes);

        if (reserve_result != detail::ReservationResult::Reserved) {
            update_pressure_state(/*hard_failure=*/true);

            detail::OverflowContext<T> ctx{
                msg,
                meta,
                reservation_,
                overflow_queue_,
                total_rejected_,
                total_dropped_,
                total_dead_letters_,
                metrics_ring_buffer_,
                config_,
                actor_id_,
                static_cast<uint32_t>(lanes_.total_depth()),
                reservation_.queued_bytes(),
                [this]() { return drop_one_oldest(); },
                nullptr,                                      // dlq
                [this]() { return drop_one_oldest(); }};       // drop_lowest_priority_fn

            auto result = overflow_handler_->handle(ctx, reserve_result);

            result.pressure_state = pressure_state_.current_state();
            result.pressure_ratio = pressure_ratio();
            if (result.retry_after.count() == 0) {
                auto base =
                    std::chrono::milliseconds(config_.signal_min_interval_ms);
                if (result.pressure_state == MailboxPressureState::HardPressure) {
                    result.retry_after = base * 2;
                } else if (result.pressure_state ==
                               MailboxPressureState::SoftPressure ||
                           result.pressure_state ==
                               MailboxPressureState::Recovering) {
                    result.retry_after = base;
                }
            }

            if (result.code == EnqueueResultCode::DroppedExisting) {
                reserve_result = reservation_.try_reserve(
                    meta.estimated_bytes, config_.capacity.max_messages,
                    config_.capacity.max_bytes);
                if (reserve_result == detail::ReservationResult::Reserved) {
                    void* raw = mem::allocate(mem::RegionType::kMessage,
                                              sizeof(T), actor_id_);
                    auto* node = new (raw) T(std::move(msg));
                    enqueue_reserved(node, meta, lane);
                    return make_result(pressure_state_.code_after_accept());
                }
                result.code = EnqueueResultCode::Rejected;
            }
            return result;
        }

        void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(T), actor_id_);
        auto* node = new (raw) T(std::move(msg));
        enqueue_reserved(node, meta, lane);
        return make_result(pressure_state_.code_after_accept());
    }

    void push(T&& msg) noexcept {
        (void)try_push(std::move(msg));
    }

    void enqueue(T* node) noexcept {
        FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
            return;  // caller sees this as rejected
        }
        uint64_t bytes = estimate_node_bytes(*node);
        if (reservation_.try_reserve(bytes, config_.capacity.max_messages,
                                     config_.capacity.max_bytes) !=
            detail::ReservationResult::Reserved) {
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

    void enqueue_reserved(T* node, const MailboxEnvelopeMeta& meta,
                          uint8_t lane_idx = 0,
                          bool suppress_wakeup = false) noexcept {
        FAULT_INJECT("hpactor.mailbox.enqueue_reserved.drop") {
            return;  // drop after capacity committed
        }
        bool was_empty = empty();
        lanes_.enqueue(node, lane_idx);
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        update_max_depth();
        update_pressure_state();

        int64_t depth = lanes_.total_depth();
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
                if (continuation_callback_) {
                    continuation_callback_();
                }
                scheduler_->notify_ready(actor_id_, meta.priority, meta.deadline_ns);
            }
        }
    }

    T* dequeue() noexcept {
        lock_consumer();
        T* node = lanes_.dequeue();
        FAULT_INJECT("hpactor.mailbox.dequeue.drop") {
            // Silently drop: release reservation but return null to caller
            if (node != nullptr) {
                reservation_.release(estimate_node_bytes(*node));
            }
            unlock_consumer();
            return nullptr;
        }
        if (node != nullptr) {
            uint64_t bytes = estimate_node_bytes(*node);
            bool is_sys = false;
            if constexpr (std::is_same_v<T, TypedMessage>) {
                is_sys = is_system_message(node->type_id());
            }
            if (!is_sys) {
                reservation_.release(bytes);
            }
            total_dequeued_.fetch_add(1, std::memory_order_relaxed);
            update_pressure_state();
            if (empty()) {
                mailbox_was_empty_.store(true, std::memory_order_release);
            }
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
        return lanes_.empty();
    }

    bool was_empty() const noexcept {
        return mailbox_was_empty_.load(std::memory_order_acquire);
    }

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

    void inject_for_test(T* node) noexcept {
        reservation_.inject_count(estimate_node_bytes(*node));
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        lanes_.enqueue(node, 0);
        mailbox_was_empty_.store(false, std::memory_order_release);
    }

    cli::MboxSnapshot snapshot() const {
        cli::MboxSnapshot s;
        s.depth = static_cast<uint32_t>(lanes_.total_depth());
        s.capacity = config_.capacity.max_messages;
        s.queued_bytes = reservation_.queued_bytes();
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
        s.system_lane_depth = static_cast<uint32_t>(
            lanes_.lane_depth(MultiLaneQueue<T>::kSystemLaneSentinel));
        s.num_user_lanes = lanes_.num_user_lanes();
        for (uint8_t i = 0; i < s.num_user_lanes && i < 8; ++i) {
            s.lane_depths[i] = static_cast<uint32_t>(lanes_.lane_depth(i));
        }
        s.high_priority_depth = s.lane_depths[0];
        s.pressure_state = to_string(pressure_state_.current_state());
        s.overflow_policy = to_string(config_.overflow_policy);
        return s;
    }

    std::optional<uint64_t>
    try_acquire_backpressure_signal(uint64_t now_ns, MailboxPressureState state,
                                    bool force = false) noexcept {
        return backpressure_signal_gate_.try_acquire(
            now_ns, state, config_.signal_min_interval_ms, force);
    }

  private:
    bool drop_one_oldest() noexcept {
        FAULT_INJECT("hpactor.mailbox.drop_oldest.fail") {
            return false;  // eviction failed
        }
        lock_consumer();
        T* node = lanes_.try_drop_from_lowest_user_lane();
        if (!node) {
            unlock_consumer();
            return false;
        }
        uint64_t bytes = estimate_node_bytes(*node);
        reservation_.release(bytes);
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
        lanes_.set_pending_free(node);
        return true;
    }

    void drain_overflow() noexcept {
        FAULT_INJECT("hpactor.mailbox.drain_overflow.fail") {
            return;  // pretend drained
        }
        while (config_.overflow_policy == OverflowPolicy::SpillToOverflowQueue) {
            if (reservation_.try_reserve(0, config_.capacity.max_messages,
                                         config_.capacity.max_bytes) !=
                detail::ReservationResult::Reserved)
                break;
            T overflow_msg;
            if (!overflow_queue_.try_pop(overflow_msg)) {
                reservation_.release(0);
                break;
            }
            MailboxEnvelopeMeta meta;
            meta.estimated_bytes = estimate_node_bytes(overflow_msg);
            enqueue_reserved(new (mem::allocate(mem::RegionType::kMessage,
                                                sizeof(T), actor_id_))
                                 T(std::move(overflow_msg)),
                             meta, /*lane_idx=*/0, /*suppress_wakeup=*/true);
        }
    }

    double pressure_ratio() const noexcept {
        const uint32_t cap = config_.capacity.max_messages;
        const uint32_t depth = static_cast<uint32_t>(lanes_.total_depth());
        double count_ratio = 0.0;
        if (cap > 0) {
            count_ratio = static_cast<double>(depth) / static_cast<double>(cap);
        }
        double byte_ratio = 0.0;
        const uint64_t byte_cap = config_.capacity.max_bytes;
        if (byte_cap > 0) {
            byte_ratio = static_cast<double>(reservation_.queued_bytes()) /
                         static_cast<double>(byte_cap);
        }
        return count_ratio > byte_ratio ? count_ratio : byte_ratio;
    }

    void update_pressure_state(bool hard_failure = false) noexcept {
        pressure_state_.update(pressure_ratio(), hard_failure,
                               config_.high_watermark, config_.low_watermark,
                               config_.critical_watermark);
    }

    void update_max_depth() noexcept {
        uint64_t depth = static_cast<uint64_t>(lanes_.total_depth());
        uint64_t prev = max_depth_.load(std::memory_order_acquire);
        while (depth > prev) {
            if (max_depth_.compare_exchange_weak(prev, depth, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                break;
            }
        }
    }

    EnqueueResult
    make_result(EnqueueResultCode code,
                BackpressureReason reason = BackpressureReason::HighWatermark) const noexcept {
        EnqueueResult r;
        r.code = code;
        r.target = actor_id_;
        r.depth = static_cast<uint32_t>(lanes_.total_depth());
        r.capacity = config_.capacity.max_messages;
        r.bytes = reservation_.queued_bytes();
        r.byte_capacity = config_.capacity.max_bytes;
        r.pressure_ratio = pressure_ratio();
        r.pressure_reason = reason;
        r.pressure_state = pressure_state_.current_state();

        auto base = std::chrono::milliseconds(config_.signal_min_interval_ms);
        if (r.pressure_state == MailboxPressureState::HardPressure) {
            r.retry_after = base * 2;
        } else if (r.pressure_state == MailboxPressureState::SoftPressure ||
                   r.pressure_state == MailboxPressureState::Recovering) {
            r.retry_after = base;
        }
        return r;
    }

    static uint64_t estimate_node_bytes(const T& node) noexcept {
        if constexpr (std::is_same_v<T, TypedMessage>) {
            return estimate_message_bytes(node);
        } else {
            return sizeof(T);
        }
    }

    uint8_t route_lane(const MailboxEnvelopeMeta& meta) const noexcept {
        if (is_system_message(meta.type_tag))
            return MultiLaneQueue<T>::kSystemLaneSentinel;
        if (!config_.priority_aware)
            return 0;
        return std::min<uint8_t>(meta.priority, lanes_.num_user_lanes() - 1);
    }

    void lock_consumer() noexcept {
        while (consumer_lock_.test_and_set(std::memory_order_acquire)) {
        }
    }
    void unlock_consumer() noexcept {
        consumer_lock_.clear(std::memory_order_release);
    }

    // --- Composed components ---
    detail::ReservationManager<T> reservation_;
    detail::PressureStateMachine pressure_state_;
    detail::BackpressureSignalGate backpressure_signal_gate_;
    std::unique_ptr<detail::IOverflowHandler<T>> overflow_handler_;

    // --- Core queue members ---
    ActorId actor_id_;
    sched::IScheduler* scheduler_;
    MultiLaneQueue<T> lanes_{1};
    OverflowQueue<T> overflow_queue_;
    MailboxConfig config_;
    std::atomic_flag consumer_lock_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> mailbox_was_empty_{true};

    // --- Counters ---
    std::atomic<uint64_t> total_enqueued_{0};
    std::atomic<uint64_t> total_dequeued_{0};
    std::atomic<uint64_t> total_rejected_{0};
    std::atomic<uint64_t> total_dropped_{0};
    std::atomic<uint64_t> total_dead_letters_{0};
    std::atomic<uint64_t> max_depth_{0};

    // --- Dependencies ---
    ActorContinuationCallback continuation_callback_;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_{nullptr};
    log::Logger* logger_ = nullptr;
};

} // namespace hpactor::mailbox
