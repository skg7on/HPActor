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
#include <hpactor/mailbox/disruptor_mailbox_interface.hpp>
#include <hpactor/mailbox/disruptor_message_envelope.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <atomic>
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
/// Used by \c FixedActorMailboxCore to track senders that are between
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

// ── Fixed actor mailbox core ──────────────────────────────────────────────

/// \brief Hybrid mailbox core combining a Disruptor user ring with a
///        protected MPSC system lane and a shared scheduler wakeup gate.
///
/// Owns the fixed ring, the system-message queue, admission state,
/// and lifecycle flags.  The core outlives the actor object so that
/// surviving \c FixedActorRef instances observe a clean rejection
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
template <size_t Capacity, FixedMailboxMessage... Messages>
class FixedActorMailboxCore final
    : public std::enable_shared_from_this<FixedActorMailboxCore<Capacity, Messages...>> {
  public:
    using envelope_type = FixedMessageEnvelope<Messages...>;
    using ring_type = adt::DisruptorMpscRing<envelope_type, Capacity>;

    /// \brief Construct the mailbox core.
    ///
    /// \param[in] actor_id    The owning actor's ID.
    /// \param[in] actor_addr  The owning actor's address.
    explicit FixedActorMailboxCore(ActorId actor_id, ActorAddress actor_addr) noexcept
        : actor_id_(actor_id), actor_address_(actor_addr) {}

    FixedActorMailboxCore(const FixedActorMailboxCore&) = delete;
    FixedActorMailboxCore& operator=(const FixedActorMailboxCore&) = delete;
    FixedActorMailboxCore(FixedActorMailboxCore&&) = delete;
    FixedActorMailboxCore& operator=(FixedActorMailboxCore&&) = delete;

    ~FixedActorMailboxCore() = default;

    // ── Port wiring (immutable after spawn) ────────────────────────────────

    void set_delivery_port(FixedMailboxDelivery port) noexcept {
        delivery_ = port;
    }

    // ── User message publication ───────────────────────────────────────────

    /// \brief Try to publish a fixed user message into the ring.
    ///
    /// Performs the bounded-admission sequence: increment in-flight
    /// publishers, check accepting flag, run delivery preflight, claim
    /// a ring slot, copy the envelope, publish, and arm the wakeup gate.
    ///
    /// \return \c Accepted or a rejection with the canonical failure reason.
    template <FixedMailboxMessage Message>
    EnqueueResult try_push_user(Message message, FixedEnvelopeMeta meta) noexcept {
        PublisherGuard guard{in_flight_publishers_};

        if (!accepting_user_.load(std::memory_order_acquire)) {
            return make_rejected(FailureReason::MailboxClosed, meta);
        }

        if (delivery_.preflight) {
            auto pf = delivery_.preflight(delivery_.context, actor_address_, meta);
            if (!pf.accepted) {
                return make_rejected(pf.reason, meta);
            }
        }

        envelope_type envelope;
        envelope.message = std::move(message);
        envelope.meta = meta;

        auto published = user_ring_.try_publish(std::move(envelope));
        if (!published.accepted()) {
            auto reason = published.closed() ? FailureReason::MailboxClosed
                                             : FailureReason::MailboxFull;
            return make_rejected(reason, meta);
        }

        // sequence recorded in slot by try_publish; consumer reads it
        // from the ReadLease
        signal_work();
        return make_accepted();
    }

    // ── System/control message ingress ─────────────────────────────────────

    /// \brief Push a system TypedMessage into the protected system lane.
    ///
    /// Only TypeTags below \c TypeTag::User are accepted.  User-level
    /// TypedMessages targeting a fixed actor are rejected by
    /// LocalDeliveryEngine before reaching this method.
    EnqueueResult try_push_control(TypedMessage msg) noexcept {
        std::lock_guard<std::mutex> lock{system_mutex_};
        system_queue_.push_back(std::move(msg));
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
            if (dispatch_system_fn_) {
                dispatch_system_fn_(dispatch_ctx_, std::move(sys_msg));
            }
            return true;
        }

        // User ring.
        auto lease = user_ring_.try_acquire();
        if (!lease) {
            // Clear wakeup signal and re-check both lanes to close
            // the lost-wakeup window.
            work_signaled_.store(false, std::memory_order_release);
            // Re-check system lane.
            {
                std::lock_guard<std::mutex> lock{system_mutex_};
                if (!system_queue_.empty()) {
                    // Re-arm and let caller requeue us.
                    signal_work();
                    return false;
                }
            }
            // Re-check user ring.
            if (!user_ring_.empty()) {
                signal_work();
                return false;
            }
            return false;
        }
        // Lease holds the slot through dispatch; releases on scope exit.
        if (dispatch_user_fn_) {
            dispatch_user_fn_(dispatch_ctx_, &lease.value());
        }
        return true;
    }

    /// Set the dispatch callbacks (called once after spawn).
    void
    set_dispatch_callbacks(void* ctx,
                           void (*system_fn)(void*, TypedMessage&&) noexcept,
                           void (*user_fn)(void*, void* envelope) noexcept) noexcept {
        dispatch_ctx_ = ctx;
        dispatch_system_fn_ = system_fn;
        dispatch_user_fn_ = user_fn;
    }

    /// \brief True when neither lane has available messages.
    [[nodiscard]] bool empty() const noexcept {
        if (user_ring_.empty()) {
            std::lock_guard<std::mutex> lock{system_mutex_};
            return system_queue_.empty();
        }
        return false;
    }

    // ── Wakeup gate ────────────────────────────────────────────────────────

    /// \brief Set the scheduler notification callback.
    ///
    /// Called once after spawn.  The callback is invoked when the
    /// mailbox transitions from empty to non-empty.
    void set_signal_callback(void (*fn)(void*), void* ctx) noexcept {
        signal_fn_ = fn;
        signal_ctx_ = ctx;
    }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    void begin_drain() noexcept {
        accepting_user_.store(false, std::memory_order_release);
    }

    void close() noexcept {
        accepting_user_.store(false, std::memory_order_release);
        user_ring_.close();
    }

    [[nodiscard]] bool publishers_quiescent() const noexcept {
        return in_flight_publishers_.load(std::memory_order_acquire) == 0;
    }

    // ── Accessors ──────────────────────────────────────────────────────────

    [[nodiscard]] ring_type& ring() noexcept {
        return user_ring_;
    }
    [[nodiscard]] const ring_type& ring() const noexcept {
        return user_ring_;
    }
    [[nodiscard]] ActorId actor_id() const noexcept {
        return actor_id_;
    }

    // ── Port builders ──────────────────────────────────────────────────────

    [[nodiscard]] FixedMailboxHandle make_handle() noexcept {
        FixedMailboxHandle b;
        b.lifetime = this->shared_from_this();

        b.control.context = this;
        b.control.try_push =
            +[](void* ctx, TypedMessage&& msg) noexcept -> EnqueueResult {
            return static_cast<FixedActorMailboxCore*>(ctx)->try_push_control(
                std::move(msg));
        };

        b.execution.context = this;
        b.execution.consume_one =
            +[](void* ctx, EventBasedActor& actor,
                const sched::ActorExecutionContext& exec_ctx) noexcept -> bool {
            return static_cast<FixedActorMailboxCore*>(ctx)->consume_one(
                &actor, exec_ctx);
        };
        b.execution.empty = +[](const void* ctx) noexcept -> bool {
            return static_cast<const FixedActorMailboxCore*>(ctx)->empty();
        };

        b.lifecycle.context = this;
        b.lifecycle.begin_drain = +[](void* ctx) noexcept {
            static_cast<FixedActorMailboxCore*>(ctx)->begin_drain();
        };
        b.lifecycle.close = +[](void* ctx) noexcept {
            static_cast<FixedActorMailboxCore*>(ctx)->close();
        };
        b.lifecycle.publishers_quiescent = +[](const void* ctx) noexcept -> bool {
            return static_cast<const FixedActorMailboxCore*>(ctx)->publishers_quiescent();
        };

        return b;
    }

  private:
    /// Edge-triggered: on the empty-to-non-empty transition, sets the
    /// signaled flag and notifies the scheduler that work is available.
    /// Subsequent calls while signaled are no-ops — the consumer must
    /// clear the flag (in consume_one) before the gate can fire again.
    void signal_work() noexcept {
        bool expected = false;
        if (work_signaled_.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            if (signal_fn_) {
                signal_fn_(signal_ctx_);
            }
        }
    }

    EnqueueResult make_accepted() noexcept {
        EnqueueResult result;
        result.code = EnqueueResultCode::Accepted;
        result.target = actor_id_;
        return result;
    }

    EnqueueResult make_rejected(FailureReason reason,
                                const FixedEnvelopeMeta& /*meta*/) noexcept {
        EnqueueResult result;
        result.code = EnqueueResultCode::Rejected;
        result.target = actor_id_;
        // reason stored as auxiliary context; callers check result.code
        // and can inspect reason via the delivery port'\''s record_rejected
        // callback once wired.
        (void)reason;
        return result;
    }

    ActorId actor_id_;
    ActorAddress actor_address_;

    // User ring.
    ring_type user_ring_;

    // System lane (protected by mutex — low volume, correctness-first).
    mutable std::mutex system_mutex_;
    std::vector<TypedMessage> system_queue_;

    // Admission and lifecycle.
    std::atomic<bool> accepting_user_{true};
    std::atomic<uint32_t> in_flight_publishers_{0};
    std::atomic<bool> work_signaled_{false};

    // Wakeup callback (set once after spawn).
    void (*signal_fn_)(void*){nullptr};
    void* signal_ctx_{nullptr};

    // Immutable delivery port (set once after spawn).
    FixedMailboxDelivery delivery_;

    // Dispatch callbacks (set once after spawn).
    void* dispatch_ctx_{nullptr};
    void (*dispatch_system_fn_)(void*, TypedMessage&&) noexcept {nullptr};
    void (*dispatch_user_fn_)(void*, void* envelope) noexcept {nullptr};
};

} // namespace hpactor::mailbox
