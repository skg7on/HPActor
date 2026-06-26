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

#include <hpactor/cli/cli_types.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/actor_rate_limiter.hpp>
#include <hpactor/mailbox/admission_policy.hpp>
#include <hpactor/mailbox/detail/backpressure_signal_gate.hpp>
#include <hpactor/mailbox/detail/overflow_handler_factory.hpp>
#include <hpactor/mailbox/detail/pressure_state_machine.hpp>
#include <hpactor/mailbox/detail/reservation_manager.hpp>
#include <hpactor/mailbox/mpsc_mailbox.hpp>
#include <hpactor/mailbox/multi_lane_queue.hpp>
#include <hpactor/mailbox/overflow_queue.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>

namespace hpactor::mailbox {

/// \brief Callback invoked when a previously empty mailbox receives its first
///        message.
///
/// The callback is called from the enqueue path (producer thread) under the
/// CAS edge-triggered wakeup protocol: it fires only on the empty→non-empty
/// transition. The callee is responsible for scheduling or resuming the actor
/// coroutine.
///
/// \note Thread safety: invoked by an arbitrary producer thread. The callee
///       must be safe to call from any thread.
using ActorContinuationCallback = std::function<void()>;

/// \brief Lock-free multi-producer, single-consumer actor mailbox with
///        bounded capacity, priority lanes, backpressure, and overflow
///        handling.
///
/// This is the primary mailbox implementation for event-based actors. It
/// composes a \c MultiLaneQueue for storage, a \c ReservationManager for
/// bounded admission, a \c PressureStateMachine for hysteresis-based
/// backpressure, an \c IOverflowHandler for policy-driven overflow, and a
/// \c BackpressureSignalGate for rate-limited signal emission.
///
/// \par Producer path (lock-free)
/// Multiple producer threads call \c try_push() concurrently without
/// blocking. Admission is gated by atomic reservation of message-count and
/// byte capacity. On rejection, the configured \c OverflowPolicy determines
/// whether the message is dropped, dead-lettered, spilled to an overflow
/// queue, or rejected with a backpressure signal.
///
/// \par Consumer path (spin-lock serialized)
/// A single consumer (the actor's scheduler worker) calls \c dequeue().
/// Dequeue acquires a TAS spin-lock, drains the highest-priority
/// non-empty lane, releases the corresponding reservation, updates
/// pressure state, and drains the overflow queue if the policy is
/// \c SpillToOverflowQueue.
///
/// \par Lane routing
/// System messages (TypeTag < \c TypeTag::User) are routed to a dedicated
/// system lane that bypasses the capacity reservation system and is
/// protected by a separate depth guard (\c protected_system_messages).
/// User messages are routed to lane \c min(priority, num_user_lanes-1).
///
/// \par Wakeup protocol
/// The mailbox uses an edge-triggered CAS wakeup: when the first message
/// arrives in an empty mailbox, the \c mailbox_was_empty_ flag is CAS'd
/// from true→false, the continuation callback is invoked, and
/// \c scheduler_->notify_ready() is called. Subsequent enqueues into a
/// non-empty mailbox do not re-trigger the wakeup. The consumer resets
/// the flag to true when \c dequeue() drains the last message.
///
/// \tparam T Message type stored in the mailbox. Must have an
///           \c std::atomic<T*> \c mpsc_next member for the lock-free
///           queue. Typically \c TypedMessage.
///
/// \note Thread safety: producers are lock-free (multi-producer safe).
///       The consumer path is serialized by a TAS spin-lock. Counter
///       fields use relaxed atomics and are safe to read from any thread,
///       but may be stale by the time the caller observes them.
/// \note Ownership: messages are allocated via \c mem::allocate() on
///       enqueue. The caller of \c dequeue() or \c try_pop() owns the
///       returned pointer and must destroy and deallocate it.
template <typename T> class MPSCActorMailbox {
  public:
    /// \brief Construct a mailbox for an actor.
    ///
    /// Initializes the multi-lane queue, overflow queue, overflow handler,
    /// and pressure state machine from the supplied configuration.
    ///
    /// \param[in] actor_id The owning actor's identifier.
    /// \param[in] scheduler Scheduler used for \c notify_ready() wakeups.
    ///                      Must outlive the mailbox.
    /// \param[in] config Initial mailbox configuration. Defaults to an
    ///                   unbounded 1024-message capacity with
    ///                   \c RejectNewest overflow policy.
    /// \pre \p scheduler must not be null.
    MPSCActorMailbox(ActorId actor_id, sched::IScheduler* scheduler,
                     MailboxConfig config = {}) noexcept
        : actor_id_(actor_id), scheduler_(scheduler), config_(config) {
        if (config_.capacity.max_messages == 0) {
            config_.capacity.max_messages = 1024;
        }
        overflow_queue_.set_max_depth(config_.max_overflow_depth);
        overflow_handler_ =
            detail::make_overflow_handler<T>(config_.overflow_policy);
        lanes_.set_num_user_lanes(config_.priority_levels);
        prefill_node_pool();
    }

    /// \brief Destroy the mailbox.
    ///
    /// Drains and deallocates all pending-free ring entries. Messages
    /// remaining in lanes are leaked — the actor must drain the mailbox
    /// before destruction.
    ///
    /// \note The caller must ensure no concurrent \c try_push() or
    ///       \c dequeue() calls are in flight during destruction.
    ~MPSCActorMailbox() {
        lanes_.drain_pending_free();
    }

    /// \brief Register a callback invoked on the empty→non-empty transition.
    ///
    /// The callback is called from the enqueue path (producer thread) and
    /// typically resumes the actor coroutine or schedules the actor for
    /// execution.
    ///
    /// \param[in] callback The continuation to invoke. An empty
    ///                     \c std::function disables the callback.
    /// \note Thread safety: the callback is invoked by an arbitrary producer
    ///       thread. The registered function must be safe to call from any
    ///       thread.
    void set_continuation_callback(ActorContinuationCallback callback) {
#ifdef HPACTOR_DEBUG
        assert(!in_active_use_.load(std::memory_order_acquire) &&
               "set_continuation_callback: mailbox already in active use");
#endif
        continuation_callback_ = std::move(callback);
    }

    /// \brief Replace the mailbox configuration at runtime.
    ///
    /// Reconfigures capacity, watermarks, overflow policy, overflow handler,
    /// overflow queue depth, and priority lane count. Existing messages are
    /// not affected.
    ///
    /// \param[in] cfg New configuration. Zero \c max_messages is clamped to
    ///                1024. \c critical_watermark <= 0 is clamped to
    ///                \c high_watermark * 2 (or 1.0). If
    ///                \c critical_watermark < \c high_watermark, it is
    ///                clamped to \c high_watermark.
    /// \note Thread safety: not safe to call concurrently with
    ///       \c try_push() or \c dequeue(). Call from the actor's own
    ///       thread or during a quiescent period.
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

    /// \brief Read the current mailbox configuration.
    ///
    /// \return A const reference to the active \c MailboxConfig.
    /// \note Thread safety: the returned reference may be invalidated by a
    ///       concurrent \c set_config() call. Read from the actor's own
    ///       thread or during a quiescent period.
    const MailboxConfig& config() const noexcept {
        return config_;
    }

    /// \brief Install a rate limiter that gates message consumption on
    ///        \c dequeue().
    ///
    /// When set, \c dequeue() calls \c try_consume() before returning a
    /// user message. If the rate limiter denies the token, the message is
    /// re-enqueued to lane 0 with wakeup suppressed. System messages
    /// bypass the rate limiter.
    ///
    /// \param[in] limiter The rate limiter instance. Pass \c nullptr to
    ///                    disable rate limiting.
    /// \note Thread safety: the pointer is a non-atomic store. Set before
    ///       the mailbox is used concurrently, or from the actor's own
    ///       thread during a quiescent period.
    void set_rate_limiter(std::unique_ptr<ActorRateLimiter> limiter) noexcept {
#ifdef HPACTOR_DEBUG
        assert(!in_active_use_.load(std::memory_order_acquire) &&
               "set_rate_limiter: mailbox already in active use");
#endif
        rate_limiter_ = std::move(limiter);
    }

    /// \brief Install a chain of admission policies evaluated on
    ///        \c try_push() before lane routing.
    ///
    /// Policies are evaluated in order; the first non-\c Accept decision
    /// short-circuits the chain and rejects the message. An empty or null
    /// policy set disables admission gating.
    ///
    /// \param[in] policies Shared pointer to an ordered vector of admission
    ///                     policy instances. Pass \c nullptr or an empty
    ///                     vector to disable.
    /// \note Thread safety: the pointer is a non-atomic store. Set before
    ///       the mailbox is used concurrently, or from the actor's own
    ///       thread during a quiescent period.
    void set_admission_policies(
        std::shared_ptr<std::vector<std::unique_ptr<IAdmissionPolicy>>> policies) noexcept {
#ifdef HPACTOR_DEBUG
        assert(!in_active_use_.load(std::memory_order_acquire) &&
               "set_admission_policies: mailbox already in active use");
#endif
        admission_policies_ = std::move(policies);
    }

    /// \brief Read-only access to the installed rate limiter.
    ///
    /// \return Pointer to the \c ActorRateLimiter, or \c nullptr if none
    ///         is installed.
    /// \note Thread safety: the returned pointer may be invalidated by a
    ///       concurrent \c set_rate_limiter() call.
    const ActorRateLimiter* rate_limiter() const noexcept {
        return rate_limiter_.get();
    }

    /// \brief Attempt to enqueue a message into the mailbox (lock-free,
    ///        multi-producer safe).
    ///
    /// This is the primary producer API. Before lane routing, the optional
    /// admission policy chain is evaluated — if any policy returns a
    /// non-\c Accept decision, the message is rejected. Accepted messages
    /// are then routed to the appropriate lane (system or user), go through
    /// atomic capacity reservation, and delegate to the overflow handler on
    /// failure.
    ///
    /// \par Admission gate
    /// If \c admission_policies_ is non-empty, \c evaluate_policy_chain()
    /// runs before lane routing. A non-\c Accept decision increments
    /// \c admission_rejected_total_ and returns \c Rejected immediately.
    ///
    /// \par System messages
    /// Routed to the dedicated system lane, bypassing capacity reservation.
    /// Protected by \c protected_system_messages — if the system lane depth
    /// exceeds this threshold, the message is rejected.
    ///
    /// \par User messages
    /// Admission is gated by \c ReservationManager::try_reserve(). On
    /// success, the message is allocated and enqueued to the routed lane.
    /// On failure, the \c IOverflowHandler is invoked with the configured
    /// policy. If the handler returns \c DroppedExisting (indicating an
    /// older message was evicted to make room), retry reservation once.
    ///
    /// \param[in,out] msg The message to enqueue. Moved-from on successful
    ///                    enqueue or if the overflow handler consumes it.
    /// \param[in] meta Envelope metadata (sender, priority, deadline,
    ///                 estimated bytes). \c estimated_bytes is always
    ///                 recomputed from the message via
    ///                 \c estimate_node_bytes().
    /// \return An \c EnqueueResult describing the outcome. Check
    ///         \c result.accepted() to determine success. On rejection,
    ///         \c retry_after may contain a suggested backoff duration.
    /// \retval Accepted Message was enqueued under normal pressure.
    /// \retval AcceptedWithSoftPressure Message was enqueued, but the
    ///         mailbox is above the high watermark.
    /// \retval Rejected Admission policy denied the message, or capacity
    ///         reservation failed and the overflow policy did not make
    ///         room.
    /// \retval DroppedExisting An older message was evicted; the new
    ///         message was enqueued in its place.
    /// \retval ReroutedToDeadLetter Message was sent to the dead-letter
    ///         queue instead of the mailbox.
    /// \retval ReroutedToOverflow Message was spilled to the overflow queue.
    /// \note Thread safety: lock-free and safe to call from any thread.
    ///       Multiple producers may call concurrently.
    /// \note The caller must not access \p msg after a successful enqueue
    ///       (the mailbox owns it). After a rejection where the overflow
    ///       handler has not consumed it, \p msg remains valid.
    EnqueueResult try_push(T&& msg, MailboxEnvelopeMeta meta = {}) noexcept {
#ifdef HPACTOR_DEBUG
        in_active_use_.store(true, std::memory_order_release);
#endif

        FAULT_INJECT("hpactor.mailbox.try_push.fail") {
            EnqueueResult r;
            r.code = EnqueueResultCode::Rejected;
            r.target = actor_id_;
            return r;
        }

        meta.estimated_bytes = estimate_node_bytes(msg);

        // Admission policy gate — evaluate before lane routing.
        if (admission_policies_ && !admission_policies_->empty()) {
            auto adm_result = evaluate_policy_chain(msg, meta);
            if (adm_result.decision != AdmissionDecision::Accept) {
                admission_rejected_total_.fetch_add(1, std::memory_order_relaxed);
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
        }

        uint8_t lane = route_lane(meta);

        // System messages use the dedicated system lane.
        if (lane == MultiLaneQueue<T>::kSystemLaneSentinel) {
            uint32_t cur = system_lane_reserved_.load(std::memory_order_acquire);
            do {
                if (cur >= config_.protected_system_messages) {
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
            } while (!system_lane_reserved_.compare_exchange_weak(
                cur, cur + 1, std::memory_order_acq_rel, std::memory_order_acquire));
            void* raw =
                mem::allocate(mem::RegionType::kMessage, sizeof(T), actor_id_);
            auto* node = new (raw) T(std::move(msg));
            system_lane_bytes_.fetch_add(meta.estimated_bytes,
                                         std::memory_order_relaxed);
            enqueue_reserved(node, meta, MultiLaneQueue<T>::kSystemLaneSentinel);
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
                [this]() { return drop_one_oldest_global(); },
                nullptr,                                          // dlq
                [this]() { return drop_one_lowest_priority(); }}; // drop_lowest_priority_fn

            auto result = overflow_handler_->handle(ctx, reserve_result);

            result.pressure_state = pressure_state_.current_state();
            result.pressure_ratio = pressure_ratio();
            if (result.retry_after.count() == 0) {
                auto base =
                    std::chrono::milliseconds(config_.signal_min_interval_ms);
                if (result.pressure_state == MailboxPressureState::HardPressure) {
                    result.retry_after = base * 2;
                } else if (result.pressure_state == MailboxPressureState::SoftPressure ||
                           result.pressure_state == MailboxPressureState::Recovering) {
                    result.retry_after = base;
                }
            }

            if (result.code == EnqueueResultCode::DroppedExisting) {
                reserve_result = reservation_.try_reserve(
                    meta.estimated_bytes, config_.capacity.max_messages,
                    config_.capacity.max_bytes);
                if (reserve_result == detail::ReservationResult::Reserved) {
                    auto* node = try_acquire_node();
                    if (node) {
                        *node = std::move(msg);
                    } else {
                        void* raw = mem::allocate(mem::RegionType::kMessage,
                                                  sizeof(T), actor_id_);
                        node = new (raw) T(std::move(msg));
                    }
                    enqueue_reserved(node, meta, lane);
                    return make_result(pressure_state_.code_after_accept());
                }
                result.code = EnqueueResultCode::Rejected;
                total_rejected_.fetch_add(1, std::memory_order_relaxed);
                update_pressure_state(/*hard_failure=*/true);
            }
            return result;
        }

        // Try pre-allocated pool first, fall back to heap allocation.
        auto* node = try_acquire_node();
        if (node) {
            *node = std::move(msg);
        } else {
            void* raw =
                mem::allocate(mem::RegionType::kMessage, sizeof(T), actor_id_);
            node = new (raw) T(std::move(msg));
        }
        enqueue_reserved(node, meta, lane);
        return make_result(pressure_state_.code_after_accept());
    }

    /// \brief Batch-enqueue a range of messages with a single reservation
    ///        and a single edge-triggered wakeup.
    ///
    /// All messages in the batch share the same \p meta (priority, deadline,
    /// lane routing). The batch reserves capacity for all N messages at once,
    /// allocates N nodes, links them into a chain, and enqueues the chain
    /// head atomically. The edge-triggered wakeup fires at most once for the
    /// entire batch.
    ///
    /// \tparam Iterator Forward iterator over \c T values.
    /// \param[in] begin Start of the message range.
    /// \param[in] end   End of the message range.
    /// \param[in] meta  Envelope metadata shared by all messages.
    /// \return \c EnqueueResult describing acceptance or rejection.
    /// \note Thread safety: lock-free and safe to call from any thread.
    template <typename Iterator>
    EnqueueResult try_push_batch(Iterator begin, Iterator end,
                                 MailboxEnvelopeMeta meta = {}) noexcept {
        size_t count = 0;
        uint64_t total_bytes = 0;
        for (auto it = begin; it != end; ++it) {
            ++count;
            total_bytes += estimate_node_bytes(*it);
        }
        if (count == 0) {
            return make_result(pressure_state_.code_after_accept());
        }

        meta.estimated_bytes = total_bytes;

        // Single reservation for the entire batch.
        auto reserve_result =
            reservation_.try_reserve(total_bytes, config_.capacity.max_messages,
                                     config_.capacity.max_bytes);
        if (reserve_result != detail::ReservationResult::Reserved) {
            // Fall back to individual enqueues for each message.
            update_pressure_state(/*hard_failure=*/true);
            EnqueueResult last_result;
            for (auto it = begin; it != end; ++it) {
                last_result = try_push(std::move(*it), meta);
            }
            return last_result;
        }

        // Allocate each node and enqueue individually, suppressing wakeup
        // for all but the first (which may trigger the edge-triggered CAS).
        uint8_t lane = route_lane(meta);
        bool first = true;
        for (auto it = begin; it != end; ++it) {
            auto* node = try_acquire_node();
            if (node) {
                *node = std::move(*it);
            } else {
                void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(T),
                                          actor_id_);
                node = new (raw) T(std::move(*it));
            }
            // Suppress wakeup for all nodes after the first — the
            // edge-triggered CAS on the first node claims the wakeup
            // right, and subsequent nodes see mailbox_was_empty_=false.
            enqueue_reserved(node, meta, lane, /*suppress_wakeup=*/!first);
            first = false;
        }

        return make_result(pressure_state_.code_after_accept());
    }

    /// \brief Fire-and-forget enqueue — discards the admission result.
    ///
    /// Equivalent to \c try_push() but the caller does not inspect the
    /// outcome. Prefer \c try_push() when the caller needs to react to
    /// rejection, backpressure, or retry timing.
    ///
    /// \param[in,out] msg The message to enqueue. Moved-from on success;
    ///                    remains valid only if silently dropped.
    /// \note Thread safety: lock-free and safe to call from any thread.
    void push(T&& msg) noexcept {
        (void)try_push(std::move(msg));
    }

    /// \brief Enqueue an already-allocated message node with automatic
    ///        capacity reservation.
    ///
    /// Used by internal paths (e.g., the scheduler replaying a deferred
    /// message) where the message is already heap-allocated. Attempts
    /// reservation; on failure, rejects and emits a \c kMailboxRejected
    /// metric event.
    ///
    /// \param[in] node Heap-allocated message node. Ownership transfers to
    ///                 the mailbox on success; leaked by the caller on
    ///                 rejection (the caller cannot safely deallocate
    ///                 after a rejected \c enqueue() because the scheduler
    ///                 may have already processed it).
    /// \note Thread safety: lock-free and safe to call from any thread.
    /// \note Prefer \c try_push() for new messages. Use \c enqueue() only
    ///       when the message is already allocated and the caller can
    ///       tolerate silent rejection.
    void enqueue(T* node) noexcept {
        FAULT_INJECT("hpactor.mailbox.enqueue.fail") {
            return; // caller sees this as rejected
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

    /// \brief Enqueue a message node whose capacity has already been
    ///        reserved.
    ///
    /// Appends the node to the specified lane, updates depth and pressure
    /// state, emits a \c kMailboxEnqueue metric event, and triggers the
    /// edge-triggered wakeup if the mailbox was previously empty.
    ///
    /// \param[in] node Heap-allocated message node. Ownership transfers
    ///                 to the mailbox.
    /// \param[in] meta Envelope metadata for priority, deadline, and
    ///                 byte accounting.
    /// \param[in] lane_idx Target lane index. Use
    ///                     \c MultiLaneQueue<T>::kSystemLaneSentinel for
    ///                     system messages, 0..N-1 for user lanes.
    /// \param[in] suppress_wakeup If true, the scheduler wakeup and
    ///                            continuation callback are not invoked
    ///                            even if the mailbox was empty. Used
    ///                            during overflow drain to avoid
    ///                            redundant wakeups.
    /// \note Thread safety: lock-free and safe to call from any thread
    ///       (the inner \c MultiLaneQueue::enqueue() is lock-free).
    void enqueue_reserved(T* node, const MailboxEnvelopeMeta& meta,
                          uint8_t lane_idx = 0,
                          bool suppress_wakeup = false) noexcept {
        FAULT_INJECT("hpactor.mailbox.enqueue_reserved.drop") {
            return; // drop after capacity committed
        }
        lanes_.enqueue(node, lane_idx);
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        update_max_depth();
        update_pressure_state();

#ifndef NDEBUG
        int64_t depth = lanes_.total_depth();
        if (depth > 1024) [[unlikely]] {
            HPACTOR_LOG_WARNING(
                log::LogCategory::kMailbox, actor_id_,
                static_cast<uint32_t>(log::LogEventId::kMailboxDepthHigh),
                "mailbox depth high",
                log::field("depth", static_cast<uint64_t>(depth)));
        }
#endif

        if (metrics_ring_buffer_) [[unlikely]] {
            metrics::MetricEvent evt{};
            evt.actor_id = actor_id_;
            evt.event_type = metrics::MetricEventType::kMailboxEnqueue;
            evt.value_hi = 1;
            metrics_ring_buffer_->try_push(evt);
        }

        // Edge-triggered wakeup: always attempt to claim the wakeup right.
        // If mailbox_was_empty_ was true, we are the first enqueue after
        // the consumer observed emptiness — notify the scheduler.
        // Spurious wakeups are safe: the consumer dequeues, finds nothing,
        // and returns to idle.
        if (!suppress_wakeup) {
            bool expected = true;
            if (mailbox_was_empty_.compare_exchange_strong(
                    expected, false, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (continuation_callback_) {
                    continuation_callback_();
                }
                if (meta.schedule_edf && meta.deadline_ns != INT64_MAX) {
                    scheduler_->notify_ready_edf(actor_id_, meta.priority,
                                                 meta.deadline_ns);
                } else {
                    scheduler_->notify_ready(actor_id_, meta.priority,
                                             meta.deadline_ns);
                }
            }
        }
    }

    /// \brief Dequeue the highest-priority message from the mailbox
    ///        (single-consumer, spin-lock serialized).
    ///
    /// Acquires the consumer spin-lock, drains the highest-priority
    /// non-empty lane (system lane first, then user lanes 0..N-1). If a
    /// rate limiter is installed, \c try_consume() is checked before
    /// returning a user message — on denial the message is re-enqueued
    /// to lane 0 and \c nullptr is returned. On successful dequeue,
    /// releases the corresponding reservation, updates pressure state,
    /// drains the overflow queue if applicable, and emits a
    /// \c kMailboxDequeue metric event.
    ///
    /// \return A pointer to the dequeued message node, or \c nullptr if the
    ///         mailbox is empty or the rate limiter denied the token. The
    ///         caller owns the returned node and must destroy and deallocate
    ///         it.
    /// \note Thread safety: serialized by a TAS spin-lock — only one
    ///       consumer (the actor's scheduler worker) may call this at a
    ///       time. Safe to call concurrently with producers.
    /// \note If the mailbox becomes empty after dequeue,
    ///       \c mailbox_was_empty_ is set to \c true, re-arming the
    ///       edge-triggered wakeup for the next enqueue.
    T* dequeue() noexcept {
#ifdef HPACTOR_DEBUG
        in_active_use_.store(true, std::memory_order_release);
#endif
        // Acquire the consumer lock BEFORE re-arming the wakeup flag.
        // This closes a race where a producer checks empty() before we
        // lock, finds the mailbox non-empty, but then enqueues after we
        // dequeue the last message — capturing a stale was_empty=false
        // and skipping the notify_ready CAS, orphaning the message.
        // By re-arming under the lock, the producer's empty() check is
        // serialized: a check before the lock sees non-empty (no wakeup
        // needed — we're about to dequeue), and a check after unlock
        // sees the re-armed flag and fires the CAS correctly.
        lock_consumer();
        mailbox_was_empty_.store(true, std::memory_order_release);
        T* node = lanes_.dequeue();

        // Rate limiter gate — skip system messages.
        if (rate_limiter_ && node != nullptr) {
            bool is_sys = false;
            if constexpr (std::is_same_v<T, TypedMessage>) {
                is_sys = is_system_message(node->type_id());
            }
            if (!is_sys) {
                uint64_t now_ns = steady_now_ns();
                if (!rate_limiter_->try_consume(now_ns)) {
                    // Re-enqueue to lane 0, suppress wakeup.
                    MailboxEnvelopeMeta re_meta{};
                    enqueue_reserved(node, re_meta, 0, true);
                    // The re-enqueued message makes the mailbox non-empty.
                    // Disarm the flag we set at the top of dequeue.
                    mailbox_was_empty_.store(false, std::memory_order_release);
                    unlock_consumer();
                    return nullptr;
                }
            }
        }

        FAULT_INJECT("hpactor.mailbox.dequeue.drop") {
            // Silently drop: release reservation but return null to caller.
            // System messages bypass reservation — skip release for them.
            if (node != nullptr) {
                bool is_sys = false;
                if constexpr (std::is_same_v<T, TypedMessage>) {
                    is_sys = is_system_message(node->type_id());
                }
                if (!is_sys) {
                    reservation_.release(estimate_node_bytes(*node));
                }
            }
            // Fault-injected drop: if the mailbox is non-empty the
            // wakeup flag must stay disarmed.
            if (!empty()) {
                mailbox_was_empty_.store(false, std::memory_order_release);
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
            } else {
                system_lane_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
                system_lane_reserved_.fetch_sub(1, std::memory_order_release);
            }
            total_dequeued_.fetch_add(1, std::memory_order_relaxed);
            update_pressure_state();
            if (!empty()) {
                // Mailbox still has messages after dequeue — disarm the
                // flag so the next true empty→non-empty transition is
                // detected by enqueue_reserved.
                mailbox_was_empty_.store(false, std::memory_order_release);
            }
            drain_overflow();
        }
        unlock_consumer();

        // Double-check after unlock: a producer may have enqueued between
        // our dequeue() and the mailbox_was_empty_ store above. If the
        // producer saw our store, its CAS already triggered notify_ready.
        // If not, the producer's CAS failed because it read false —
        // we must self-requeue.
        if (!lanes_.empty()) {
            bool expected = true;
            if (mailbox_was_empty_.compare_exchange_strong(
                    expected, false, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                // We claimed the wakeup — no producer did. Self-notify.
                scheduler_->notify_ready(actor_id_, 0, 0);
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

    /// \brief Dequeue a message and move it into an output parameter.
    ///
    /// Combines \c dequeue(), move, destructor, and deallocation in a
    /// single call. Convenient for callers that need the message by value
    /// rather than as a raw pointer.
    ///
    /// \param[out] out Receives the dequeued message by move.
    /// \return \c true if a message was dequeued, \c false if the mailbox
    ///         was empty.
    /// \note Thread safety: inherits the consumer spin-lock from
    ///       \c dequeue(). Single-consumer only.
    bool try_pop(T& out) noexcept {
        T* node = dequeue();
        if (!node)
            return false;
        out = std::move(*node);
        // Defer freeing `node` by one dequeue: a concurrent producer may hold
        // `prev = node` from head_.exchange() and write prev->mpsc_next after
        // we return. Freeing immediately causes a use-after-free that leaves
        // stub_.mpsc_next == nullptr permanently, causing the consumer spin at
        // mpsc_mailbox.hpp:66 to never exit. set_pending_free destroys+frees
        // the *previously* staged node (safe — no producer references it) and
        // stages `node` for the next call.
        lanes_.set_pending_free(node);
        return true;
    }

    /// \brief Check whether the mailbox is empty.
    ///
    /// \return \c true if all lanes (system + user) are empty.
    /// \note Thread safety: lock-free and safe to call from any thread.
    ///       The result may be stale by the time the caller observes it.
    bool empty() const noexcept {
        return lanes_.empty();
    }

    /// \brief Check whether the mailbox was empty at the last consumer
    ///        observation.
    ///
    /// This reflects the state of the edge-triggered wakeup flag, not the
    /// real-time empty status. It is set to \c true by \c dequeue() when
    /// the last message is drained, and CAS'd to \c false by the first
    /// subsequent enqueue.
    ///
    /// \return \c true if the wakeup flag is armed (mailbox was empty at
    ///         last consumer observation).
    /// \note Thread safety: lock-free (atomic load with acquire ordering).
    bool was_empty() const noexcept {
        return mailbox_was_empty_.load(std::memory_order_acquire);
    }

    /// \brief Force-set the wakeup flag.
    ///
    /// Used by test infrastructure and actor lifecycle transitions to
    /// re-arm or disarm the edge-triggered wakeup without going through
    /// the normal enqueue/dequeue paths.
    ///
    /// \param[in] val The value to store in the wakeup flag.
    /// \note Thread safety: atomic store with release ordering.
    void set_was_empty(bool val) noexcept {
        mailbox_was_empty_.store(val, std::memory_order_release);
    }

    /// \brief Wire a metrics ring buffer for instrumentation.
    ///
    /// When set, enqueue, dequeue, reject, and drop events are emitted to
    /// this buffer. Pass \c nullptr to disable metrics emission.
    ///
    /// \param[in] buf Pointer to a \c MpscRingBuffer, or \c nullptr.
    /// \note Thread safety: the pointer is a raw non-atomic store. Set
    ///       before the mailbox is used concurrently, or from the actor's
    ///       own thread during a quiescent period.
    void
    set_metrics_ring_buffer(metrics::MpscRingBuffer<metrics::MetricEvent>* buf) noexcept {
#ifdef HPACTOR_DEBUG
        assert(!in_active_use_.load(std::memory_order_acquire) &&
               "set_metrics_ring_buffer: mailbox already in active use");
#endif
        metrics_ring_buffer_ = buf;
    }

    /// \brief Wire a logger for structured log emission.
    ///
    /// When set, high-depth warnings are emitted via \c HPACTOR_LOG_WARNING.
    /// Pass \c nullptr to disable logging.
    ///
    /// \param[in] logger Pointer to a \c Logger, or \c nullptr.
    /// \note Thread safety: the pointer is a raw non-atomic store. Set
    ///       before the mailbox is used concurrently, or from the actor's
    ///       own thread during a quiescent period.
    void set_logger(log::Logger* logger) noexcept {
#ifdef HPACTOR_DEBUG
        assert(!in_active_use_.load(std::memory_order_acquire) &&
               "set_logger: mailbox already in active use");
#endif
        logger_ = logger;
    }

    /// \brief Inject a message directly into the mailbox without triggering
    ///        scheduler wakeup.
    ///
    /// Bypasses capacity reservation, pressure state, metrics, and the
    /// continuation callback. Intended for test code that needs to
    /// pre-populate the mailbox and inspect its state without scheduler
    /// interference.
    ///
    /// \param[in] node Heap-allocated message node. Ownership transfers
    ///                 to the mailbox.
    /// \note This is a test-only API. Do not use in production code paths.
    /// \note Thread safety: not safe for concurrent use with producers
    ///       or the consumer. Call from the test thread with the scheduler
    ///       paused (\c scheduler_threads = 0).
    void inject_for_test(T* node) noexcept {
        uint64_t bytes = estimate_node_bytes(*node);
        uint8_t lane = 0;
        if constexpr (std::is_same_v<T, TypedMessage>) {
            if (is_system_message(node->type_id())) {
                lane = MultiLaneQueue<T>::kSystemLaneSentinel;
                system_lane_bytes_.fetch_add(bytes, std::memory_order_relaxed);
            }
        }
        if (lane != MultiLaneQueue<T>::kSystemLaneSentinel) {
            reservation_.inject_count(bytes);
        }
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        lanes_.enqueue(node, lane);
        mailbox_was_empty_.store(false, std::memory_order_release);
    }

    /// \brief Capture a point-in-time snapshot of mailbox state for CLI
    ///        introspection.
    ///
    /// Reads depth, capacity, byte accounting, pressure ratio (as ppm),
    /// cumulative counters, overflow queue stats, per-lane depths,
    /// pressure state label, and — when installed — rate limiter token
    /// state and admission policy chain statistics.
    ///
    /// \return A \c cli::MboxSnapshot populated with current mailbox state.
    /// \note Thread safety: reads atomic counters and queries the overflow
    ///       queue under its internal lock. Safe to call from any thread,
    ///       but individual fields may reflect different points in time.
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

        if (rate_limiter_) {
            s.rate_limiter_enabled = rate_limiter_->is_enabled();
            s.rate_limiter_rate = rate_limiter_->configured_rate();
            s.rate_limiter_burst = rate_limiter_->configured_burst();
            s.rate_limiter_current_tokens = rate_limiter_->current_tokens();
            s.rate_limit_blocked_total = 0; // FIXME: wire counter in
                                            // ActorRateLimiter
        }
        if (admission_policies_) {
            s.admission_policy_count =
                static_cast<uint32_t>(admission_policies_->size());
            s.admission_rejected_total =
                admission_rejected_total_.load(std::memory_order_acquire);
            s.admission_dlq_routed_total = 0; // FIXME: wire counter
        }
        s.delivery_accepted_total =
            delivery_accepted_total_.load(std::memory_order_acquire);
        s.delivery_rejected_total =
            delivery_rejected_total_.load(std::memory_order_acquire);
        s.delivery_failed_total =
            delivery_failed_total_.load(std::memory_order_acquire);
        s.delivery_retryable_total =
            delivery_retryable_total_.load(std::memory_order_acquire);

        return s;
    }

    /// \brief Record a delivery outcome for CLI observability.
    ///
    /// \param[in] status The delivery status from the caller-facing
    ///                   DeliveryResult.
    void record_delivery_result(DeliveryStatus status) noexcept {
        if (is_accepted(status)) {
            delivery_accepted_total_.fetch_add(1, std::memory_order_relaxed);
        } else {
            delivery_rejected_total_.fetch_add(1, std::memory_order_relaxed);
            if (is_retryable(status)) {
                delivery_retryable_total_.fetch_add(1, std::memory_order_relaxed);
            } else {
                delivery_failed_total_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    /// \brief Attempt to acquire a rate-limited backpressure signal emission
    ///        slot.
    ///
    /// The \c BackpressureSignalGate enforces a minimum interval between
    /// signal emissions per pressure state, preventing signal storms during
    /// sustained overload.
    ///
    /// \param[in] now_ns Current monotonic timestamp in nanoseconds.
    /// \param[in] state Current pressure state used for interval selection.
    /// \param[in] force If \c true, bypass the rate limiter and acquire
    ///                  unconditionally.
    /// \return The emission timestamp (in nanoseconds) if a slot was
    ///         acquired, or \c std::nullopt if the rate limiter blocked
    ///         the emission.
    /// \note Thread safety: the underlying gate uses atomics and is safe
    ///       to call from any thread.
    std::optional<uint64_t>
    try_acquire_backpressure_signal(uint64_t now_ns, MailboxPressureState state,
                                    bool force = false) noexcept {
        return backpressure_signal_gate_.try_acquire(
            now_ns, state, config_.signal_min_interval_ms, force);
    }

    /// \brief Evaluate the admission policy chain against a message.
    ///
    /// Iterates \c admission_policies_ in order. The first policy returning
    /// a non-\c Accept decision short-circuits the chain. If all policies
    /// accept (or the chain is empty), returns a default-constructed result
    /// with \c AdmissionDecision::Accept.
    ///
    /// \param[in] msg The message being admitted.
    /// \param[in] meta Envelope metadata for the message.
    /// \return The first non-\c Accept \c AdmissionPolicyResult, or a
    ///         default (Accept) result.
    /// \note Thread safety: reads \c admission_policies_ and
    ///       \c lanes_.total_depth() without locking. Safe to call from
    ///       producer threads.
    AdmissionPolicyResult
    evaluate_policy_chain(const TypedMessage& msg,
                          const MailboxEnvelopeMeta& meta) noexcept {
        for (const auto& policy : *admission_policies_) {
            auto result = policy->evaluate(
                msg, meta, config_, static_cast<uint64_t>(lanes_.total_depth()));
            if (result.decision != AdmissionDecision::Accept) {
                return result;
            }
        }
        return {};
    }

    /// \brief Read the current monotonic clock in nanoseconds.
    ///
    /// Wraps \c std::chrono::steady_clock::now() for use by the rate
    /// limiter and other time-sensitive paths.
    ///
    /// \return Current steady-clock timestamp in nanoseconds since epoch.
    /// \note Thread safety: safe to call from any thread.
    static uint64_t steady_now_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

  private:
    /// \brief Drop the oldest message from the highest-priority non-empty
    ///        user lane.
    ///
    /// Acquires the consumer lock, dequeues from the first non-empty user
    /// lane (lowest index = highest priority), releases the reservation,
    /// updates counters, and stages the node for deferred destruction via
    /// \c set_pending_free().
    ///
    /// \return \c true if a message was dropped, \c false if all user
    ///         lanes were empty.
    /// \note Thread safety: acquires the consumer spin-lock internally.
    bool drop_one_oldest_global() noexcept {
        FAULT_INJECT("hpactor.mailbox.drop_oldest.fail") {
            return false;
        }
        lock_consumer();
        T* node = lanes_.try_drop_oldest_user_lane();
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
        lanes_.set_pending_free(node);
        unlock_consumer();
        return true;
    }

    /// \brief Drop one message from the lowest-priority non-empty user
    ///        lane.
    ///
    /// Scans user lanes from lowest to highest priority, dequeues the
    /// first found message, releases the reservation, updates counters,
    /// and stages the node for deferred destruction.
    ///
    /// \return \c true if a message was dropped, \c false if all user
    ///         lanes were empty.
    /// \note Thread safety: acquires the consumer spin-lock internally.
    bool drop_one_lowest_priority() noexcept {
        FAULT_INJECT("hpactor.mailbox.drop_lowest_priority.fail") {
            return false;
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
        lanes_.set_pending_free(node);
        unlock_consumer();
        return true;
    }

    /// \brief Drain messages from the overflow queue back into the main
    ///        mailbox.
    ///
    /// Only active when \c overflow_policy is \c SpillToOverflowQueue.
    /// Iterates while capacity is available, popping from the overflow
    /// queue and enqueuing into the main lanes with \c suppress_wakeup
    /// to avoid redundant scheduler notifications.
    ///
    /// \note Thread safety: called from \c dequeue() under the consumer
    ///       spin-lock.
    void drain_overflow() noexcept {
        FAULT_INJECT("hpactor.mailbox.drain_overflow.fail") {
            return; // pretend drained
        }
        while (config_.overflow_policy == OverflowPolicy::SpillToOverflowQueue) {
            // Pop first so we know the actual byte size for reservation.
            T overflow_msg;
            if (!overflow_queue_.try_pop(overflow_msg))
                break;
            uint64_t bytes = estimate_node_bytes(overflow_msg);
            auto reserve_result = reservation_.try_reserve(
                bytes, config_.capacity.max_messages, config_.capacity.max_bytes);
            if (reserve_result != detail::ReservationResult::Reserved) {
                // Cannot drain now — push back to overflow queue.
                // If push-back fails (queue full), drop the message.
                if (!overflow_queue_.try_push(std::move(overflow_msg))) {
                    total_dropped_.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            MailboxEnvelopeMeta meta;
            meta.estimated_bytes = bytes;
            enqueue_reserved(new (mem::allocate(mem::RegionType::kMessage,
                                                sizeof(T), actor_id_))
                                 T(std::move(overflow_msg)),
                             meta, /*lane_idx=*/0, /*suppress_wakeup=*/true);
        }
    }

    /// \brief Compute the current pressure ratio as max(count_ratio,
    ///        byte_ratio).
    ///
    /// Count ratio = user_depth / max_messages.
    /// Byte ratio = (queued_bytes + system_lane_bytes) / max_bytes.
    /// System lane depth is excluded from the count ratio because
    /// \c max_messages governs only user messages.
    ///
    /// \return The larger of the count and byte pressure ratios, in [0, ∞).
    /// \note Thread safety: reads atomics with relaxed ordering. Safe to
    ///       call from any thread.
    double pressure_ratio() const noexcept {
        const uint32_t cap = config_.capacity.max_messages;
        // Exclude system lane: max_messages only governs user messages.
        int64_t user_depth =
            lanes_.total_depth() -
            lanes_.lane_depth(MultiLaneQueue<T>::kSystemLaneSentinel);
        const uint32_t depth =
            static_cast<uint32_t>(user_depth > 0 ? user_depth : 0);
        double count_ratio = 0.0;
        if (cap > 0) {
            count_ratio = static_cast<double>(depth) / static_cast<double>(cap);
        }
        double byte_ratio = 0.0;
        const uint64_t byte_cap = config_.capacity.max_bytes;
        if (byte_cap > 0) {
            byte_ratio = static_cast<double>(
                             reservation_.queued_bytes() +
                             system_lane_bytes_.load(std::memory_order_relaxed)) /
                         static_cast<double>(byte_cap);
        }
        return count_ratio > byte_ratio ? count_ratio : byte_ratio;
    }

    /// \brief Update the pressure state machine from the current ratio.
    ///
    /// \param[in] hard_failure If \c true, immediately transition to
    ///                         \c HardPressure regardless of ratio.
    /// \note Thread safety: the \c PressureStateMachine uses internal
    ///       atomics and is safe to call from any thread.
    void update_pressure_state(bool hard_failure = false) noexcept {
        pressure_state_.update(pressure_ratio(), hard_failure,
                               config_.high_watermark, config_.low_watermark,
                               config_.critical_watermark);
    }

    /// \brief Update the peak-depth counter if the current depth exceeds
    ///        the recorded maximum.
    ///
    /// Uses a CAS loop to handle concurrent producers.
    ///
    /// \note Thread safety: lock-free CAS loop — safe to call from any
    ///       thread.
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

    /// \brief Build an \c EnqueueResult from a result code.
    ///
    /// Populates depth, capacity, byte accounting, pressure ratio, pressure
    /// state, and a rate-limited \c retry_after duration.
    ///
    /// \param[in] code The admission result code.
    /// \param[in] reason The backpressure reason (default:
    ///                   \c HighWatermark).
    /// \return A fully populated \c EnqueueResult.
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

    /// \brief Estimate the byte footprint of a message node.
    ///
    /// For \c TypedMessage, uses \c estimate_message_bytes() (header +
    /// payload). For other types, returns \c sizeof(T).
    ///
    /// \param[in] node The message to estimate.
    /// \return Estimated byte count for reservation accounting.
    static uint64_t estimate_node_bytes(const T& node) noexcept {
        if constexpr (std::is_same_v<T, TypedMessage>) {
            return estimate_message_bytes(node);
        } else {
            return sizeof(T);
        }
    }

    /// \brief Determine the target lane for a message.
    ///
    /// System messages (TypeTag < \c TypeTag::User) are routed to the
    /// system lane sentinel. User messages are routed to
    /// \c min(priority, num_user_lanes - 1). When \c priority_aware is
    /// \c false, all user messages go to lane 0.
    ///
    /// \param[in] meta Envelope metadata with type_tag and priority.
    /// \return Lane index (0..N-1) or \c kSystemLaneSentinel.
    uint8_t route_lane(const MailboxEnvelopeMeta& meta) const noexcept {
        if (is_system_message(meta.type_tag))
            return MultiLaneQueue<T>::kSystemLaneSentinel;
        if (!config_.priority_aware)
            return 0;
        return std::min<uint8_t>(meta.priority, lanes_.num_user_lanes() - 1);
    }

    /// \brief Acquire the consumer spin-lock (TAS).
    ///
    /// Spins until the lock is acquired. Only the consumer (actor's
    /// scheduler worker) calls this.
    void lock_consumer() noexcept {
        while (consumer_lock_.test_and_set(std::memory_order_acquire)) {
        }
    }
    /// \brief Release the consumer spin-lock.
    void unlock_consumer() noexcept {
        consumer_lock_.clear(std::memory_order_release);
    }

    // --- Composed components ---
    detail::ReservationManager<T> reservation_;   ///< Atomic capacity
                                                  ///< reservation.
    detail::PressureStateMachine pressure_state_; ///< Hysteresis-based pressure
                                                  ///< tracking.
    detail::BackpressureSignalGate backpressure_signal_gate_; ///< Rate-limited
                                                              ///< signal
                                                              ///< emission.
    std::unique_ptr<detail::IOverflowHandler<T>> overflow_handler_; ///< Policy-driven
                                                                    ///< overflow
                                                                    ///< handler.

    // --- Core queue members ---
    ActorId actor_id_;                ///< Owning actor identifier.
    sched::IScheduler* scheduler_;    ///< Scheduler for wakeup notifications.
    MultiLaneQueue<T> lanes_{1};      ///< System + user lane storage.
    OverflowQueue<T> overflow_queue_; ///< Spill-overflow queue.
    MailboxConfig config_;            ///< Active mailbox configuration.

    // --- Pre-allocated node pool ---
    /// \brief Lock-free LIFO freelist of recycled nodes.  Reuses
    ///        \c mpsc_next as the link field (a node is either in the
    ///        freelist or in a mailbox lane, never both).
    std::atomic<T*> node_freelist_{nullptr};

    /// \brief Push a node to the recycling freelist (lock-free, MP-safe).
    void recycle_node(T* node) noexcept {
        T* head = node_freelist_.load(std::memory_order_acquire);
        do {
            node->mpsc_next.store(head, std::memory_order_relaxed);
        } while (!node_freelist_.compare_exchange_weak(
            head, node, std::memory_order_acq_rel, std::memory_order_acquire));
    }

    /// \brief Try to pop a node from the recycling freelist.
    /// \note Thread safety: lock-free CAS, safe for multiple producers.
    T* try_acquire_node() noexcept {
        T* head = node_freelist_.load(std::memory_order_acquire);
        while (head != nullptr) {
            T* next = head->mpsc_next.load(std::memory_order_acquire);
            if (node_freelist_.compare_exchange_weak(head, next,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                return head;
            }
        }
        return nullptr;
    }

    /// \brief Pre-fill the freelist with capacity-sized pre-allocations.
    void prefill_node_pool() noexcept {
        size_t count = config_.capacity.max_messages;
        if (count == 0)
            return;
        // Seed the freelist with pre-allocated nodes so the first N
        // messages after startup never hit mem::allocate.  Recycled
        // nodes re-enter the freelist via the MultiLaneQueue recycle hook.
        for (size_t i = 0; i < count; ++i) {
            void* raw =
                mem::allocate(mem::RegionType::kMessage, sizeof(T), actor_id_);
            auto* node = new (raw) T();
            recycle_node(node);
        }
        // Wire the recycling hook so freed nodes return to the freelist.
        lanes_.recycle_ctx_ = this;
        lanes_.recycle_hook_ = [](void* ctx, T* n) {
            static_cast<MPSCActorMailbox*>(ctx)->recycle_node(n);
        };
    }
    std::atomic_flag consumer_lock_ = ATOMIC_FLAG_INIT; ///< TAS spin-lock for
                                                        ///< consumer
                                                        ///< serialization.
    std::atomic<bool> mailbox_was_empty_{true}; ///< Edge-triggered wakeup flag.

    // --- Rate limiter ---
    std::unique_ptr<ActorRateLimiter> rate_limiter_; ///< Token-bucket rate
                                                     ///< limiter
                                                     ///< (consumer-side).
    std::shared_ptr<std::vector<std::unique_ptr<IAdmissionPolicy>>>
        admission_policies_; ///< Ordered admission policy chain
                             ///< (producer-side).
    std::atomic<uint64_t> admission_rejected_total_{0}; ///< Cumulative messages
                                                        ///< rejected by
                                                        ///< admission policies.

    // Delivery result counters (incremented from deliver_with_result).
    std::atomic<uint64_t> delivery_accepted_total_{0};
    std::atomic<uint64_t> delivery_rejected_total_{0};
    std::atomic<uint64_t> delivery_failed_total_{0};
    std::atomic<uint64_t> delivery_retryable_total_{0};

    // --- Counters ---
    std::atomic<uint64_t> total_enqueued_{0}; ///< Cumulative successful
                                              ///< enqueues.
    std::atomic<uint64_t> total_dequeued_{0}; ///< Cumulative successful
                                              ///< dequeues.
    std::atomic<uint64_t> total_rejected_{0}; ///< Cumulative rejected messages.
    std::atomic<uint64_t> total_dropped_{0};  ///< Cumulative dropped messages.
    std::atomic<uint64_t> total_dead_letters_{0}; ///< Cumulative dead-lettered
                                                  ///< messages.
    std::atomic<uint64_t> max_depth_{0};         ///< Peak observed total depth.
    std::atomic<uint64_t> system_lane_bytes_{0}; ///< Byte count in the system
                                                 ///< lane.
    std::atomic<uint32_t> system_lane_reserved_{0}; ///< Atomic admission
                                                    ///< counter for system lane
                                                    ///< depth guard.

    // --- Debug quiescence guard ---
#ifdef HPACTOR_DEBUG
    std::atomic<bool> in_active_use_{false};
#endif

    // --- Dependencies ---
    ActorContinuationCallback continuation_callback_; ///< Callback for
                                                      ///< empty→non-empty
                                                      ///< transition.
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_{
        nullptr};                   ///< Metrics sink.
    log::Logger* logger_ = nullptr; ///< Logger for structured warnings.
};

} // namespace hpactor::mailbox
