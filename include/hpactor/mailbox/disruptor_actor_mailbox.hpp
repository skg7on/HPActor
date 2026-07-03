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

#include <hpactor/adt/disruptor_mpsc_ring.hpp>
#include <hpactor/adt/reservation_manager.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/mailbox/detail/backpressure_signal_gate.hpp>
#include <hpactor/mailbox/detail/disruptor_overflow_context.hpp>
#include <hpactor/mailbox/detail/disruptor_overflow_handler_factory.hpp>
#include <hpactor/mailbox/detail/pressure_state_machine.hpp>
#include <hpactor/mailbox/disruptor_mailbox_interface.hpp>
#include <hpactor/mailbox/disruptor_message_envelope.hpp>
#include <hpactor/mailbox/overflow_queue.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace hpactor {
class EventBasedActor;
} // namespace hpactor

namespace hpactor::mailbox {

// ── Publisher guard (RAII in-flight tracking) ─────────────────────────────

/// \brief RAII guard that increments an in-flight publisher count on
///        construction and decrements on destruction.
///
/// Used by \c DisruptorActorMailboxCore to track senders that are between
/// the \c accepting_user_ check and publication/rejection.  When the
/// count reaches zero after drain starts, drain can declare quiescence.
class PublisherGuard final {
  public:
    explicit PublisherGuard(std::atomic<uint32_t>& counter) noexcept
        : counter_(&counter) {
        counter_->fetch_add(1, std::memory_order_release);
    }

    ~PublisherGuard() {
        if (counter_) {
            counter_->fetch_sub(1, std::memory_order_release);
        }
    }

    PublisherGuard(const PublisherGuard&) = delete;
    PublisherGuard& operator=(const PublisherGuard&) = delete;

  private:
    std::atomic<uint32_t>* counter_;
};

// ── Disruptor actor mailbox core
// ──────────────────────────────────────────────

/// \brief Hybrid mailbox core combining a Disruptor user ring with a
///        protected MPSC system lane and a shared scheduler wakeup gate.
///
/// Owns the fixed ring, the system-message queue, admission state,
/// and lifecycle flags.  The core outlives the actor object so that
/// surviving \c DisruptorActorRef instances observe a clean rejection
/// rather than a dangling pointer.
///
/// \tparam Capacity Power-of-two ring capacity.
/// \tparam Messages The closed set of fixed user-message types.
///
/// \note Thread safety:
///       - \c try_push_user() is safe from multiple producer threads.
///       - \c try_push_control() is safe from any thread (internal mutex).
///       - \c consume_one() is called by at most one scheduler worker.
///       - \c begin_drain(), \c close() are called from the owning actor
///         or scheduler thread.
template <size_t Capacity, DisruptorMessage... Messages>
class DisruptorActorMailboxCore final
    : public std::enable_shared_from_this<DisruptorActorMailboxCore<Capacity, Messages...>> {
  public:
    using envelope_type = DisruptorMessageEnvelope<Messages...>;
    using ring_type = adt::DisruptorMpscRing<envelope_type, Capacity>;

    /// Maximum number of priority user lanes (0–7).
    static constexpr uint8_t kMaxPriorityLanes = 8;

    /// \brief Typed callback target — the actor inherits from this to
    ///        receive system/user messages and work-available signals
    ///        without \c void* casts.
    class DispatchTarget {
      public:
        virtual ~DispatchTarget() = default;

        /// Handle a system-level TypedMessage.
        virtual void on_system_message(TypedMessage&& msg) = 0;

        /// Handle a user message while the ring lease is held.
        virtual void on_user_message(envelope_type& env) = 0;

        /// Called when the mailbox transitions empty-to-non-empty.
        virtual void on_work_available() = 0;
    };

    /// \brief Set the dispatch target (called once after spawn).
    void set_target(DispatchTarget* target) noexcept {
        target_ = target;
    }

    /// \brief Construct the mailbox core.
    ///
    /// \param[in] actor_id    The owning actor's ID.
    /// \param[in] actor_addr  The owning actor's address.
    /// \param[in] config       Mailbox configuration (capacity, watermarks,
    /// etc.).
    /// \param[in] protected_system_msgs  Max system-lane depth (default 32).
    explicit DisruptorActorMailboxCore(ActorId actor_id, ActorAddress actor_addr,
                                       const MailboxConfig& config = MailboxConfig{},
                                       uint32_t protected_system_msgs = 32)
        : actor_id_(actor_id), actor_address_(actor_addr),
          protected_system_messages_(protected_system_msgs) {
        config_ = config;
        uint32_t clamped = config.capacity.max_messages;
        if (clamped == 0 || clamped > static_cast<uint32_t>(Capacity)) {
            clamped = static_cast<uint32_t>(Capacity);
        }
        config_.capacity.max_messages = clamped;
        // Set up priority lanes from config.
        if (config.priority_aware && config.priority_levels > 1) {
            num_user_lanes_ = std::min(config.priority_levels, kMaxPriorityLanes);
        } else {
            num_user_lanes_ = 1;
        }
        overflow_handler_ = detail::make_disruptor_overflow_handler<envelope_type>(
            config_.overflow_policy);
    }

    DisruptorActorMailboxCore(const DisruptorActorMailboxCore&) = delete;
    DisruptorActorMailboxCore& operator=(const DisruptorActorMailboxCore&) = delete;
    DisruptorActorMailboxCore(DisruptorActorMailboxCore&&) = delete;
    DisruptorActorMailboxCore& operator=(DisruptorActorMailboxCore&&) = delete;

    ~DisruptorActorMailboxCore() = default;

    // ── Port wiring (immutable after spawn) ────────────────────────────────

    void set_delivery_port(DisruptorMailboxDelivery port) noexcept {
        delivery_ = port;
    }

    // ── Configuration ──────────────────────────────────────────────────────

    /// \brief Reconfigure capacity, watermarks, and overflow policy at runtime.
    void set_config(const MailboxConfig& cfg) noexcept {
        config_ = cfg;
        uint32_t clamped = cfg.capacity.max_messages;
        if (clamped == 0 || clamped > static_cast<uint32_t>(Capacity)) {
            clamped = static_cast<uint32_t>(Capacity);
        }
        config_.capacity.max_messages = clamped;
        overflow_handler_ = detail::make_disruptor_overflow_handler<envelope_type>(
            config_.overflow_policy);
        overflow_queue_.set_max_depth(config_.max_overflow_depth);
    }

    [[nodiscard]] const MailboxConfig& config() const noexcept {
        return config_;
    }

    /// \brief Wire a dead-letter queue for overflow policies that route to DLQ.
    void set_dlq(DeadLetterQueue* dlq) noexcept {
        dlq_ = dlq;
    }

    /// \brief Resize the logical message capacity at runtime.
    ///
    /// Adjusts \c config_.capacity.max_messages. The physical ring capacity
    /// (\c Capacity) is a compile-time constant — this method changes only
    /// the soft cap enforced by the \c ReservationManager. Values above
    /// \c Capacity are clamped. Values of 0 are clamped to \c Capacity.
    ///
    /// \param[in] new_capacity The desired logical message capacity.
    /// \return The actual capacity after clamping.
    [[nodiscard]] uint32_t resize(uint32_t new_capacity) noexcept {
        if (new_capacity == 0 || new_capacity > static_cast<uint32_t>(Capacity)) {
            new_capacity = static_cast<uint32_t>(Capacity);
        }
        config_.capacity.max_messages = new_capacity;
        return new_capacity;
    }

    /// \brief Current reserved/queued message count.
    [[nodiscard]] uint32_t depth() const noexcept {
        return reservation_.reserved_count();
    }

    // ── Pressure and backpressure ────────────────────────────────────────

    /// \brief Current pressure state for observability.
    [[nodiscard]] MailboxPressureState pressure_state() const noexcept {
        return pressure_state_.current_state();
    }

    /// \brief Current pressure ratio (0.0 to 1.0+).
    [[nodiscard]] double pressure_ratio() const noexcept {
        uint32_t max_msg = config_.capacity.max_messages;
        if (max_msg == 0)
            return 0.0;
        return static_cast<double>(reservation_.reserved_count()) /
               static_cast<double>(max_msg);
    }

    /// \brief Rate-limited backpressure signal acquisition.
    [[nodiscard]] std::optional<uint64_t>
    try_acquire_backpressure_signal(uint64_t now_ns, MailboxPressureState state,
                                    bool force = false) noexcept {
        return backpressure_signal_gate_.try_acquire(now_ns, state, force);
    }

    /// \brief Update pressure state after a successful enqueue.
    void update_pressure_on_enqueue() noexcept {
        double ratio = pressure_ratio();
        pressure_state_.update(ratio, /*hard_failure=*/false,
                               config_.high_watermark, config_.low_watermark,
                               config_.critical_watermark);
    }

    /// \brief Update pressure state after a rejection or hard failure.
    void update_pressure_on_rejection() noexcept {
        double ratio = pressure_ratio();
        pressure_state_.update(ratio, /*hard_failure=*/true, config_.high_watermark,
                               config_.low_watermark, config_.critical_watermark);
    }

    // ── User message publication ───────────────────────────────────────────

    /// \brief Try to publish a fixed user message into the ring.
    ///
    /// Performs the bounded-admission sequence: increment in-flight
    /// publishers, check accepting flag, run delivery preflight, claim
    /// a ring slot, copy the envelope, publish, and arm the wakeup gate.
    ///
    /// \return \c Accepted or a rejection with the canonical failure reason.
    template <DisruptorMessage Message>
    EnqueueResult
    try_push_user(Message message, DisruptorEnvelopeMeta meta) noexcept {
        PublisherGuard guard{in_flight_publishers_};

        if (!accepting_user_.load(std::memory_order_acquire)) {
            auto result = make_rejected(FailureReason::MailboxClosed, meta);
            record_reject(result, FailureReason::MailboxClosed, meta);
            update_pressure_on_rejection();
            return result;
        }

        // Phase 4: Producer-side deadline check.
        // Uses steady clock; INT64_MAX deadline means "no deadline".
        if (is_expired(meta.deadline_ns, steady_now_ns())) {
            auto result = make_rejected(FailureReason::Expired, meta);
            record_reject(result, FailureReason::Expired, meta);
            update_pressure_on_rejection();
            return result;
        }

        if (delivery_.preflight) {
            auto pf = delivery_.preflight(delivery_.context, actor_address_, meta);
            if (!pf.accepted) {
                auto result = make_rejected(pf.reason, meta);
                record_reject(result, pf.reason, meta);
                update_pressure_on_rejection();
                return result;
            }
        }

        // Reserve a logical slot via the ReservationManager.
        uint64_t envelope_bytes = sizeof(envelope_type);
        auto reservation = reservation_.try_reserve(envelope_bytes,
                                                    config_.capacity.max_messages,
                                                    config_.capacity.max_bytes);
        if (reservation != adt::ReservationResult::Reserved) {
            update_pressure_on_rejection();

            envelope_type overflow_env;
            overflow_env.message = std::move(message);
            overflow_env.meta = meta;

            uint32_t cur_depth = depth();
            uint64_t cur_bytes = reservation_.queued_bytes();

            detail::DisruptorOverflowContext<envelope_type> ctx{
                overflow_env,
                reservation_,
                overflow_queue_,
                total_rejected_,
                total_dropped_,
                total_dead_letters_,
                config_,
                actor_id_,
                cur_depth,
                cur_bytes,
                [this]() { return drop_one_oldest_ring(); },
                [this]() { return drop_one_lowest_priority_ring(); },
                dlq_,
                &block_mutex_,
                &block_cv_};

            auto result = overflow_handler_->handle(ctx, reservation);

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
                // Handler evicted an old message; retry reservation.
                auto retry_res = reservation_.try_reserve(
                    envelope_bytes, config_.capacity.max_messages,
                    config_.capacity.max_bytes);
                if (retry_res == adt::ReservationResult::Reserved) {
                    // Re-construct envelope with the original message.
                    envelope_type retry_env;
                    retry_env.message = std::move(overflow_env.message);
                    retry_env.meta = overflow_env.meta;

                    auto& retry_ring = ring_for(retry_env.meta.priority);
                    auto pub = retry_ring.try_publish(std::move(retry_env));
                    if (pub.accepted()) {
                        retry_env.meta.enqueue_sequence = pub.sequence;
                        record_accept(retry_env.meta);
                        update_pressure_on_enqueue();
                        signal_work();
                        return make_accepted();
                    }
                    reservation_.release(envelope_bytes);
                    auto pub_result =
                        make_rejected(pub.closed() ? FailureReason::MailboxClosed
                                                   : FailureReason::MailboxFull,
                                      retry_env.meta);
                    record_reject(pub_result,
                                  pub.closed() ? FailureReason::MailboxClosed
                                               : FailureReason::MailboxFull,
                                  retry_env.meta);
                    update_pressure_on_rejection();
                    return pub_result;
                }
                // Retry reservation also failed — reject.
                result.code = EnqueueResultCode::Rejected;
                total_rejected_.fetch_add(1, std::memory_order_relaxed);
                update_pressure_on_rejection();
            }

            if (!result.accepted()) {
                record_reject(result, FailureReason::MailboxFull, overflow_env.meta);
            }
            return result;
        }

        envelope_type envelope;
        envelope.message = std::move(message);
        envelope.meta = meta;

        auto& ring = ring_for(meta.priority);
        auto published = ring.try_publish(std::move(envelope));
        if (!published.accepted()) {
            reservation_.release(envelope_bytes);
            auto reason = published.closed() ? FailureReason::MailboxClosed
                                             : FailureReason::MailboxFull;
            auto result = make_rejected(reason, meta);
            record_reject(result, reason, meta);
            update_pressure_on_rejection();
            return result;
        }

        // Stash enqueue sequence for the accepted callback.
        // Reservation is held until the consumer releases it in consume_one().
        meta.enqueue_sequence = published.sequence;
        record_accept(meta);
        update_pressure_on_enqueue();
        signal_work();
        return make_accepted();
    }

    // ── System/control message ingress ─────────────────────────────────────

    /// \brief Push a system TypedMessage into the protected system lane.
    ///
    /// Only TypeTags below \c TypeTag::User are accepted.  User-level
    /// TypedMessages targeting a fixed actor are rejected by
    /// LocalDeliveryEngine before reaching this method.
    ///
    /// Enforces \c protected_system_messages_ depth limit.
    EnqueueResult try_push_control(TypedMessage msg) noexcept {
        std::lock_guard<std::mutex> lock{system_mutex_};
        if (system_queue_.size() >= protected_system_messages_) {
            return make_rejected(FailureReason::SystemLaneFull,
                                 DisruptorEnvelopeMeta{});
        }
        system_queue_.push_back(std::move(msg));
        signal_work();
        return make_accepted();
    }

    // ── Dynamic message lane (Phase 7) ───────────────────────────────────

    /// \brief Push a dynamic (protobuf/variable) TypedMessage into the
    ///        protected dynamic lane for non-fixed message types.
    ///
    /// This lane runs alongside the fixed rings and accepts user-level
    /// TypedMessages that cannot be stored in the compile-time ring.
    EnqueueResult try_push_dynamic(TypedMessage msg) noexcept {
        std::lock_guard<std::mutex> lock{dynamic_mutex_};
        if (dynamic_queue_.size() >= protected_system_messages_) {
            return make_rejected(FailureReason::SystemLaneFull,
                                 DisruptorEnvelopeMeta{});
        }
        dynamic_queue_.push_back(std::move(msg));
        signal_work();
        return make_accepted();
    }

    /// \brief Push a dynamic TypedMessage bypassing the bounded capacity
    ///        limit. For critical system messages (DLQ replay, admin commands).
    EnqueueResult try_push_dynamic_force(TypedMessage msg) noexcept {
        std::lock_guard<std::mutex> lock{dynamic_mutex_};
        dynamic_queue_.push_back(std::move(msg));
        signal_work();
        return make_accepted();
    }

    // ── Consumer ───────────────────────────────────────────────────────────

    /// \brief Consume one message (system-first, then user).
    ///
    /// Called by the scheduler worker through the execution port.
    /// Returns true when a message was processed.
    bool consume_one(void* /*actor*/,
                     const sched::ActorExecutionContext& /*ctx*/) noexcept {
        // Acquire consumer lock to serialize with producer drop operations.
        lock_consumer();

        // System lane first (matching existing mailbox contract).
        TypedMessage sys_msg;
        bool has_system = false;
        {
            std::lock_guard<std::mutex> lock{system_mutex_};
            if (!system_queue_.empty()) {
                sys_msg = std::move(system_queue_.front());
                system_queue_.erase(system_queue_.begin());
                has_system = true;
            }
        }
        if (has_system) {
            unlock_consumer();
            if (target_) {
                target_->on_system_message(std::move(sys_msg));
            }
            return true;
        }

        // Dynamic lane (Phase 7) — second priority after system lane.
        TypedMessage dyn_msg;
        bool has_dynamic = false;
        {
            std::lock_guard<std::mutex> lock{dynamic_mutex_};
            if (!dynamic_queue_.empty()) {
                dyn_msg = std::move(dynamic_queue_.front());
                dynamic_queue_.erase(dynamic_queue_.begin());
                has_dynamic = true;
            }
        }
        if (has_dynamic) {
            unlock_consumer();
            if (target_) {
                target_->on_system_message(std::move(dyn_msg));
            }
            return true;
        }

        // User rings — drain highest-priority first.
        for (uint8_t lane = 0; lane < num_user_lanes_; ++lane) {
            auto lease = user_rings_[lane].try_acquire();
            if (!lease)
                continue;
            // Lease holds the slot through dispatch; releases on scope exit.
            if (target_) {
                target_->on_user_message(lease.value());
            }
            // Release the logical reservation after successful dispatch.
            reservation_.release(sizeof(envelope_type));
            drain_overflow();
            block_cv_.notify_one(); // Wake one blocked producer.
            unlock_consumer();
            return true;
        }

        unlock_consumer();

        // Clear wakeup signal and re-check all lanes to close
        // the lost-wakeup window.
        work_signaled_.store(false, std::memory_order_release);
        // Re-check system lane.
        {
            std::lock_guard<std::mutex> lock{system_mutex_};
            if (!system_queue_.empty()) {
                signal_work();
                return false;
            }
        }
        // Re-check dynamic lane.
        {
            std::lock_guard<std::mutex> lock{dynamic_mutex_};
            if (!dynamic_queue_.empty()) {
                signal_work();
                return false;
            }
        }
        // Re-check all user rings.
        for (uint8_t lane = 0; lane < num_user_lanes_; ++lane) {
            if (!user_rings_[lane].empty()) {
                signal_work();
                return false;
            }
        }
        return false;
    }

    /// \brief True when no lane (system, dynamic, user rings) has messages.
    [[nodiscard]] bool empty() const noexcept {
        for (uint8_t lane = 0; lane < num_user_lanes_; ++lane) {
            if (!user_rings_[lane].empty())
                return false;
        }
        {
            std::lock_guard<std::mutex> lock{dynamic_mutex_};
            if (!dynamic_queue_.empty())
                return false;
        }
        {
            std::lock_guard<std::mutex> lock{system_mutex_};
            return system_queue_.empty();
        }
    }

    // ── Wakeup gate ────────────────────────────────────────────────────────

    // ── Lifecycle ──────────────────────────────────────────────────────────

    void begin_drain() noexcept {
        accepting_user_.store(false, std::memory_order_release);
    }

    void close() noexcept {
        accepting_user_.store(false, std::memory_order_release);
        for (uint8_t lane = 0; lane < num_user_lanes_; ++lane) {
            user_rings_[lane].close();
        }
    }

    [[nodiscard]] bool publishers_quiescent() const noexcept {
        return in_flight_publishers_.load(std::memory_order_acquire) == 0;
    }

    // ── Priority lane routing ──────────────────────────────────────────

    /// \brief Set the number of active priority user lanes.
    /// Clamped to [1, kMaxPriorityLanes].
    void set_num_user_lanes(uint8_t n) noexcept {
        num_user_lanes_ =
            (n < 1) ? 1 : (n > kMaxPriorityLanes ? kMaxPriorityLanes : n);
    }

    /// \brief Number of active priority user lanes.
    [[nodiscard]] uint8_t num_user_lanes() const noexcept {
        return num_user_lanes_;
    }

    /// \brief Select the ring for a given priority level.
    [[nodiscard]] ring_type& ring_for(uint8_t priority) noexcept {
        uint8_t lane =
            std::min(priority, static_cast<uint8_t>(num_user_lanes_ - 1));
        return user_rings_[lane];
    }

    // ── Accessors ──────────────────────────────────────────────────────────

    /// \brief Access ring 0 (highest-priority) for backward-compatible usage.
    [[nodiscard]] ring_type& ring() noexcept {
        return user_rings_[0];
    }
    [[nodiscard]] const ring_type& ring() const noexcept {
        return user_rings_[0];
    }
    [[nodiscard]] ActorId actor_id() const noexcept {
        return actor_id_;
    }

    // ── Snapshot ──────────────────────────────────────────────────────────

    /// \brief Build an MboxSnapshot for CLI/metrics observability.
    [[nodiscard]] cli::MboxSnapshot build_snapshot() const noexcept {
        cli::MboxSnapshot snap;
        uint64_t total_depth = 0;
        uint64_t max_depth = 0;
        // Aggregate across all user rings.
        for (uint8_t lane = 0; lane < num_user_lanes_; ++lane) {
            auto rs = user_rings_[lane].snapshot();
            total_depth += rs.published_depth;
            if (rs.max_observed_depth > max_depth)
                max_depth = rs.max_observed_depth;
            if (lane < 8)
                snap.lane_depths[lane] = static_cast<uint32_t>(rs.published_depth);
        }
        auto ring0 = user_rings_[0].snapshot();
        snap.depth = static_cast<uint32_t>(total_depth);
        snap.capacity = ring0.capacity * num_user_lanes_;
        snap.max_depth = max_depth;
        snap.queued_bytes = total_depth * ring0.slot_bytes;
        snap.byte_capacity = static_cast<uint64_t>(ring0.capacity) *
                             num_user_lanes_ * ring0.slot_bytes;
        snap.total_enqueued = total_accepted_.load(std::memory_order_relaxed);
        snap.total_dequeued =
            total_accepted_.load(std::memory_order_relaxed) - total_depth;
        snap.total_rejected = total_rejected_.load(std::memory_order_relaxed);
        snap.total_dropped = total_dropped_.load(std::memory_order_relaxed);
        snap.total_dead_letters =
            total_dead_letters_.load(std::memory_order_relaxed);
        {
            auto oq_snap = overflow_queue_.snapshot();
            snap.overflow_depth = oq_snap.depth;
            snap.overflow_max_depth = oq_snap.max_depth;
        }
        {
            std::lock_guard<std::mutex> lock{system_mutex_};
            snap.system_lane_depth = static_cast<uint32_t>(system_queue_.size());
        }
        snap.num_user_lanes = num_user_lanes_;
        snap.overflow_policy = to_string(config_.overflow_policy);
        return snap;
    }

    // ── Drain immediate ──────────────────────────────────────────────────

    /// \brief Drain all queued messages without invoking handlers.
    ///
    /// Releases all published ring slots and clears the system lane.
    void drain_all_now() noexcept {
        begin_drain();
        // Drain all user rings: acquire and immediately reset each lease.
        for (uint8_t lane = 0; lane < num_user_lanes_; ++lane) {
            while (true) {
                auto lease = user_rings_[lane].try_acquire();
                if (!lease)
                    break;
                lease.reset();
            }
        }
        // Drain the dynamic lane.
        {
            std::lock_guard<std::mutex> lock{dynamic_mutex_};
            dynamic_queue_.clear();
        }
        // Drain the system lane.
        {
            std::lock_guard<std::mutex> lock{system_mutex_};
            system_queue_.clear();
        }
    }

    // ── Port builders ──────────────────────────────────────────────────────

    [[nodiscard]] DisruptorMailboxHandle make_handle() noexcept {
        DisruptorMailboxHandle b;
        b.lifetime = this->shared_from_this();

        b.control.context = this;
        b.control.try_push =
            +[](void* ctx, TypedMessage&& msg) noexcept -> EnqueueResult {
            return static_cast<DisruptorActorMailboxCore*>(ctx)->try_push_control(
                std::move(msg));
        };
        b.control.try_push_dynamic =
            +[](void* ctx, TypedMessage&& msg) noexcept -> EnqueueResult {
            return static_cast<DisruptorActorMailboxCore*>(ctx)->try_push_dynamic(
                std::move(msg));
        };

        b.execution.context = this;
        b.execution.consume_one =
            +[](void* ctx, EventBasedActor& actor,
                const sched::ActorExecutionContext& exec_ctx) noexcept -> bool {
            return static_cast<DisruptorActorMailboxCore*>(ctx)->consume_one(
                &actor, exec_ctx);
        };
        b.execution.empty = +[](const void* ctx) noexcept -> bool {
            return static_cast<const DisruptorActorMailboxCore*>(ctx)->empty();
        };
        b.execution.snapshot_fn =
            +[](const void* ctx) noexcept -> cli::MboxSnapshot {
            return static_cast<const DisruptorActorMailboxCore*>(ctx)->build_snapshot();
        };

        b.lifecycle.context = this;
        b.lifecycle.begin_drain = +[](void* ctx) noexcept {
            static_cast<DisruptorActorMailboxCore*>(ctx)->begin_drain();
        };
        b.lifecycle.drain_immediate =
            +[](void* ctx, EventBasedActor& /*actor*/) noexcept {
                static_cast<DisruptorActorMailboxCore*>(ctx)->drain_all_now();
            };
        b.lifecycle.close = +[](void* ctx) noexcept {
            static_cast<DisruptorActorMailboxCore*>(ctx)->close();
        };
        b.lifecycle.publishers_quiescent = +[](const void* ctx) noexcept -> bool {
            return static_cast<const DisruptorActorMailboxCore*>(ctx)
                ->publishers_quiescent();
        };

        return b;
    }

  private:
    /// \brief Acquire the consumer spin-lock (TAS).
    void lock_consumer() noexcept {
        while (consumer_lock_.test_and_set(std::memory_order_acquire)) {
        }
    }

    /// \brief Release the consumer spin-lock.
    void unlock_consumer() noexcept {
        consumer_lock_.clear(std::memory_order_release);
    }

    /// \brief Evict the oldest message from the highest-priority non-empty
    ///        user ring.
    ///
    /// \return true if a message was dropped, false if all rings were empty.
    /// \note Must be called with \c consumer_lock_ held.
    bool drop_one_oldest_ring() noexcept {
        // Scan rings from highest to lowest priority, evict from first
        // non-empty ring.
        for (uint8_t lane = 0; lane < num_user_lanes_; ++lane) {
            auto lease = user_rings_[lane].try_acquire();
            if (lease) {
                lease.reset(); // release the lease, dropping the message
                reservation_.release(sizeof(envelope_type));
                total_dropped_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    /// \brief Evict the lowest-priority message from the lowest-priority
    ///        non-empty user ring.
    ///
    /// \return true if a message was dropped, false if all rings were empty.
    /// \note Must be called with \c consumer_lock_ held.
    bool drop_one_lowest_priority_ring() noexcept {
        // Scan rings from lowest to highest priority, evict from first
        // non-empty ring.
        for (int lane = num_user_lanes_ - 1; lane >= 0; --lane) {
            auto lease = user_rings_[static_cast<uint8_t>(lane)].try_acquire();
            if (lease) {
                lease.reset(); // release the lease, dropping the message
                reservation_.release(sizeof(envelope_type));
                total_dropped_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    /// \brief Drain messages from the overflow queue back into the rings.
    ///
    /// Only active when overflow_policy is SpillToOverflowQueue. Iterates
    /// while capacity is available, popping from the overflow queue and
    /// publishing into the rings.
    ///
    /// \note Must be called with \c consumer_lock_ held.
    void drain_overflow() noexcept {
        while (config_.overflow_policy == OverflowPolicy::SpillToOverflowQueue) {
            envelope_type overflow_env;
            if (!overflow_queue_.try_pop(overflow_env))
                break;
            uint64_t bytes = sizeof(envelope_type);
            auto reserve_result = reservation_.try_reserve(
                bytes, config_.capacity.max_messages, config_.capacity.max_bytes);
            if (reserve_result != adt::ReservationResult::Reserved) {
                // Push back to overflow queue; drop if push-back fails.
                if (!overflow_queue_.try_push(std::move(overflow_env))) {
                    total_dropped_.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            auto& ring = ring_for(overflow_env.meta.priority);
            auto pub = ring.try_publish(std::move(overflow_env));
            if (!pub.accepted()) {
                reservation_.release(bytes);
                total_dropped_.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }

    /// \brief Current monotonic nanosecond timestamp for deadline checks.
    static uint64_t steady_now_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    /// Edge-triggered: on the empty-to-non-empty transition, sets the
    /// signaled flag and notifies the scheduler that work is available.
    /// Subsequent calls while signaled are no-ops — the consumer must
    /// clear the flag (in consume_one) before the gate can fire again.
    void signal_work() noexcept {
        bool expected = false;
        if (work_signaled_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            if (target_) {
                target_->on_work_available();
            }
        }
    }

    EnqueueResult make_accepted() noexcept {
        total_accepted_.fetch_add(1, std::memory_order_relaxed);
        EnqueueResult result;
        result.code = EnqueueResultCode::Accepted;
        result.target = actor_id_;
        result.depth = reservation_.reserved_count();
        result.capacity = config_.capacity.max_messages;
        result.bytes = reservation_.queued_bytes();
        result.byte_capacity = config_.capacity.max_bytes;
        result.pressure_state = pressure_state_.current_state();
        result.pressure_ratio = pressure_ratio();
        return result;
    }

    EnqueueResult make_rejected(FailureReason /*reason*/,
                                const DisruptorEnvelopeMeta& /*meta*/) noexcept {
        total_rejected_.fetch_add(1, std::memory_order_relaxed);
        EnqueueResult result;
        result.code = EnqueueResultCode::Rejected;
        result.target = actor_id_;
        result.depth = reservation_.reserved_count();
        result.capacity = config_.capacity.max_messages;
        result.bytes = reservation_.queued_bytes();
        result.byte_capacity = config_.capacity.max_bytes;
        result.pressure_state = pressure_state_.current_state();
        result.pressure_ratio = pressure_ratio();
        result.retry_after =
            std::chrono::milliseconds{config_.signal_min_interval_ms};
        return result;
    }

    void record_accept(const DisruptorEnvelopeMeta& meta) noexcept {
        if (delivery_.record_accepted) {
            DisruptorDeliveryObservation obs;
            obs.actor = actor_id_;
            obs.message_id = meta.message_id;
            obs.enqueue_sequence = meta.enqueue_sequence;
            obs.payload_bytes = static_cast<uint32_t>(sizeof(envelope_type));
            obs.deadline_ns = static_cast<uint64_t>(meta.deadline_ns);
            delivery_.record_accepted(delivery_.context, actor_id_, obs);
        }
    }

    void record_reject(const EnqueueResult& /*result*/, FailureReason reason,
                       const DisruptorEnvelopeMeta& meta) noexcept {
        if (delivery_.record_rejected) {
            DisruptorDeliveryFailure failure;
            failure.actor = actor_id_;
            failure.sender = meta.sender;
            failure.reason = reason;
            failure.message_id = meta.message_id;
            failure.deadline_ns = static_cast<uint64_t>(meta.deadline_ns);
            failure.payload_bytes = static_cast<uint32_t>(sizeof(envelope_type));
            delivery_.record_rejected(delivery_.context, actor_id_, failure);
        }
    }

    ActorId actor_id_;
    ActorAddress actor_address_;

    // User rings — one per priority lane.
    std::array<ring_type, kMaxPriorityLanes> user_rings_{};
    uint8_t num_user_lanes_{1};

    // Dynamic lane for protobuf/variable messages (Phase 7).
    mutable std::mutex dynamic_mutex_;
    std::vector<TypedMessage> dynamic_queue_;

    // System lane (protected by mutex — low volume, correctness-first).
    mutable std::mutex system_mutex_;
    std::vector<TypedMessage> system_queue_;
    uint32_t protected_system_messages_{32};

    // Admission and lifecycle.
    adt::ReservationManager<envelope_type> reservation_;
    MailboxConfig config_;
    detail::PressureStateMachine pressure_state_;
    detail::BackpressureSignalGate backpressure_signal_gate_;
    std::atomic<bool> accepting_user_{true};
    std::atomic<uint32_t> in_flight_publishers_{0};
    std::atomic<bool> work_signaled_{false};
    std::atomic_flag consumer_lock_ = ATOMIC_FLAG_INIT;

    // Overflow handling.
    std::unique_ptr<detail::IDisruptorOverflowHandler<envelope_type>> overflow_handler_;
    OverflowQueue<envelope_type> overflow_queue_;
    DeadLetterQueue* dlq_{nullptr};

    // Producer blocking (BlockWhenAllowed policy).
    std::mutex block_mutex_;
    std::condition_variable block_cv_;

    // Cumulative counters for observability.
    std::atomic<uint64_t> total_accepted_{0};
    std::atomic<uint64_t> total_rejected_{0};
    std::atomic<uint64_t> total_dropped_{0};
    std::atomic<uint64_t> total_dead_letters_{0};

    // Typed dispatch target (set once after spawn).
    DispatchTarget* target_{nullptr};

    // Immutable delivery port (set once after spawn).
    DisruptorMailboxDelivery delivery_;
};

} // namespace hpactor::mailbox
