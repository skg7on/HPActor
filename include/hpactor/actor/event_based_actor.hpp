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

#include <hpactor/actor/actor_state.hpp>
#include <hpactor/actor/lifecycle/circuit_breaker.hpp>
#include <hpactor/actor/lifecycle/drain_config.hpp>
#include <hpactor/actor/lifecycle/failure_rate_tracker.hpp>
#include <hpactor/actor/lifecycle/quarantine_policy.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <hpactor/mem/std_allocator.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#if HPACTOR_SUPPORT_COROUTINES
#    include <hpactor/coroutine/actor_coroutine.hpp>
#    include <hpactor/coroutine/coroutine_awaiters.hpp>
#    include <hpactor/coroutine/coroutine_task.hpp>
#endif

namespace hpactor {

// Forward declarations
class ProtoTypeRegistry;
namespace log {
class Logger;
} // namespace log

/// \brief Internal type-erased protobuf handler storage.
///
/// Avoids template bloat in the handler map by storing deserialization
/// and invocation as type-erased \c std::function objects.
struct ProtoHandler {
    std::string type_name; ///< Fully-qualified protobuf message type name.

    ProtoHandler() = default;
    ProtoHandler(ProtoHandler&&) = default;
    ProtoHandler& operator=(ProtoHandler&&) = default;
    ProtoHandler(const ProtoHandler&) = delete;
    ProtoHandler& operator=(const ProtoHandler&) = delete;

    /// \brief Deserialize bytes into a \c shared_ptr<void> holding the
    ///        concrete protobuf type.
    std::function<std::shared_ptr<void>(const StreamBuffer&)> deserialize;

    /// \brief Invoke the handler with a deserialized message.
    ///
    /// Returns serialized response bytes (empty for fire-and-forget).
    std::function<StreamBuffer(std::shared_ptr<void>)> invoke;
};

/// \brief Cooperatively scheduled actor with behavior-based handling,
///        protobuf handler dispatch, and optional coroutine support.
///
/// The primary actor type for most use cases. Supports become() semantics,
/// fire-and-forget protobuf handlers via \c on<T>(), request-response
/// handlers via \c on_request<ReqT, ResT>(), and opt-in quarantine with
/// circuit breaker.
///
/// \note Thread safety: All message handling and state transitions execute
///       on a single scheduler worker thread.
class EventBasedActor : public LocalActor {
  public:
    /// \brief Replace the current behavior.
    ///
    /// Subsequent messages are dispatched to \p bh.
    /// \param[in] bh New behavior to install.
    void become(Behavior bh);

    /// \brief Remove the current behavior, effectively dropping all messages.
    void become_empty();

    void receive(TypedMessage& msg) override;

    /// \brief RTTI-free query for \c EventBasedActor subclasses.
    bool is_event_based_actor() const override {
        return true;
    }

    /// \brief Register a fire-and-forget handler for a protobuf message type.
    ///
    /// Called from \c register_handlers(). The handler is invoked on each
    /// incoming message of type \p ProtoMsgT.
    /// \tparam ProtoMsgT Protobuf message type.
    /// \param[in] handler Callable invoked with a const reference to the
    ///                    deserialized message.
    template <typename ProtoMsgT>
    void on(std::function<void(const ProtoMsgT&)> handler) {
        TypeTag tag = type_tag_for<ProtoMsgT>();
        auto handler_ptr =
            mem::allocate_shared<std::function<void(const ProtoMsgT&)>>(
                id_ptr(), mem::RegionType::kActor, std::move(handler));

        ProtoHandler entry;
        entry.type_name = ProtoMsgT().GetTypeName();
        entry.deserialize = [](const StreamBuffer& data) -> std::shared_ptr<void> {
            auto msg = mem::allocate_shared<ProtoMsgT>(mem::current_actor_id(),
                                                       mem::RegionType::kMessage);
            if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
                return nullptr;
            }
            return msg;
        };
        entry.invoke = [handler_ptr](std::shared_ptr<void> raw) -> StreamBuffer {
            auto& msg = *static_cast<ProtoMsgT*>(raw.get());
            (*handler_ptr)(msg);
            return {};
        };

        proto_handlers_[tag] = std::move(entry);
    }

    /// \brief Register a request-response handler for protobuf types.
    ///
    /// Called from \c register_handlers(). The handler receives a
    /// deserialized request and returns a response that is automatically
    /// serialized and sent back to the caller.
    /// \tparam ReqT Request protobuf message type.
    /// \tparam ResT Response protobuf message type.
    /// \param[in] handler Callable invoked with a const reference to the
    ///                    request; must return a \c ResT by value.
    template <typename ReqT, typename ResT>
    void on_request(std::function<ResT(const ReqT&)> handler) {
        TypeTag tag = type_tag_for<ReqT>();
        auto handler_ptr = mem::allocate_shared<std::function<ResT(const ReqT&)>>(
            id_ptr(), mem::RegionType::kActor, std::move(handler));

        ProtoHandler entry;
        entry.type_name = ReqT().GetTypeName();
        entry.deserialize = [](const StreamBuffer& data) -> std::shared_ptr<void> {
            auto msg = mem::allocate_shared<ReqT>(mem::current_actor_id(),
                                                  mem::RegionType::kMessage);
            if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
                return nullptr;
            }
            return msg;
        };
        entry.invoke = [handler_ptr](std::shared_ptr<void> raw) -> StreamBuffer {
            auto& req = *static_cast<ReqT*>(raw.get());
            ResT res = (*handler_ptr)(req);
            StreamBuffer result(res.ByteSizeLong());
            (void)res.SerializeToArray(result.data(),
                                       static_cast<int>(result.size()));
            return result;
        };

        proto_handlers_[tag] = std::move(entry);
    }

    /// \brief Dispatch an incoming protobuf message by \c TypeTag.
    ///
    /// Looks up the handler registered for \p tag and invokes it with the
    /// deserialized payload.
    /// \param[in] tag Type tag identifying the protobuf message type.
    /// \param[in] payload Serialized protobuf message bytes.
    void on_proto_message(TypeTag tag, const StreamBuffer& payload);

    /// \brief Returns \c true if this actor has a handler for \p tag.
    ///
    /// \param[in] tag Type tag to check.
    [[nodiscard]] bool handles(TypeTag tag) const {
        return proto_handlers_.find(tag) != proto_handlers_.end();
    }

#if HPACTOR_SUPPORT_COROUTINES
    // Coroutine support (C++20 only)
    virtual sched::CoroutineTask act() {
        co_return;
    }

    sched::ActorCoroutine& get_actor_coroutine() {
        return actor_coroutine_;
    }
    const sched::ActorCoroutine& get_actor_coroutine() const {
        return actor_coroutine_;
    }
    void set_actor_coroutine(sched::ActorCoroutine&& coroutine) {
        actor_coroutine_ = std::move(coroutine);
    }

    std::coroutine_handle<sched::CoroutinePromise> get_coro_handle() {
        return coro_handle_;
    }
    void set_coro_handle(std::coroutine_handle<sched::CoroutinePromise> h) {
        coro_handle_ = h;
    }

    sched::MailboxAwaiter<TypedMessage> make_mailbox_awaiter() {
        return sched::MailboxAwaiter<TypedMessage>{
            coro_handle_.promise(), mailbox_, home_system().dead_letter_queue(),
            home_system().metrics_ring_buffer(), id()};
    }

    void ensure_coroutine_started() {
        if (!actor_coroutine_) {
            auto task = act();
            if (task) {
                coro_handle_ = task.handle();
                coro_handle_.promise().actor_state = &actor_state_;
                actor_coroutine_ = sched::ActorCoroutine{std::move(task), id()};

                if (mailbox_) {
                    // Continuation callback disabled: direct resume races with
                    // the scheduler's execute_actor state machine, causing
                    // await_suspend CAS(kRunning→kIdle) to fail and the
                    // coroutine to busy-loop. Wakeups go through notify_ready()
                    // which transitions state correctly.
                    //
                    // auto* coro_ptr = &actor_coroutine_;
                    // mailbox_->set_continuation_callback([coro_ptr]() {
                    //     if (!coro_ptr->done()) {
                    //         coro_ptr->promise().notify_mailbox_nonempty();
                    //     }
                    // });
                }
            }
        }
    }

#else  // !HPACTOR_SUPPORT_COROUTINES
    void ensure_coroutine_started() {}
#endif // HPACTOR_SUPPORT_COROUTINES

    sched::IScheduler* get_scheduler() {
        return scheduler_;
    }

    mailbox::MPSCActorMailbox<TypedMessage>* get_mailbox() {
        return mailbox_;
    }

    ActorState& actor_state() {
        return actor_state_;
    }
    const ActorState& actor_state() const {
        return actor_state_;
    }

    bool mailbox_has_messages() const {
        return mailbox_ && !mailbox_->empty();
    }
    bool mailbox_is_empty() const {
        return !mailbox_ || mailbox_->empty();
    }

    /// \brief Process one message under the actor's drain policy.
    ///
    /// \param[in,out] msg The message to process or dead-letter.
    /// \return \c true if the message should be processed normally.
    /// \retval false The message was dead-lettered by the drain policy.
    bool drain_one(TypedMessage& msg);

    /// \brief Dead-letter all messages currently in the mailbox.
    ///
    /// Used for \c DrainPolicy::ImmediateStop.
    void drain_all_immediate();

    // ── Quarantine & circuit breaker ────────────────────

    /// \brief Configure quarantine and circuit breaker for this actor.
    ///
    /// Initializes the \c FailureRateTracker bucket interval from the
    /// policy's observation window. Must be called before the actor
    /// starts processing messages (typically from
    /// \c ActorSystem::spawn_configured).
    ///
    /// \param[in] policy The per-actor quarantine and circuit breaker
    ///                   policy. Copied into the actor.
    /// \note Thread safety: call once before the actor is activated.
    ///       Not safe for concurrent reconfiguration.
    void configure_quarantine(const QuarantinePolicy& policy);

    /// \brief Whether quarantine or circuit breaker is enabled for this actor.
    ///
    /// \return \c quarantine_policy_.enabled — \c false by default.
    [[nodiscard]] bool quarantine_enabled() const noexcept {
        return quarantine_policy_.enabled;
    }

    /// \brief Read-only access to the actor's quarantine policy.
    ///
    /// \return A const reference to the stored \c QuarantinePolicy.
    [[nodiscard]] const QuarantinePolicy& quarantine_policy() const noexcept {
        return quarantine_policy_;
    }

    /// \brief Access the circuit breaker tracker.
    ///
    /// \return Pointer to the \c CircuitBreakerTracker, or \c nullptr
    ///         if quarantine is not enabled for this actor.
    /// \note Thread safety: the returned pointer is valid for the
    ///       actor's lifetime. Only the owning scheduler thread may
    ///       mutate the tracker.
    [[nodiscard]] CircuitBreakerTracker* circuit_breaker() noexcept {
        return quarantine_policy_.enabled ? &circuit_breaker_ : nullptr;
    }

    /// \brief Access the failure rate tracker.
    ///
    /// \return Pointer to the \c FailureRateTracker, or \c nullptr
    ///         if quarantine is not enabled for this actor.
    /// \note Thread safety: same contract as \c circuit_breaker().
    [[nodiscard]] FailureRateTracker* failure_rate_tracker() noexcept {
        return quarantine_policy_.enabled ? &failure_rate_tracker_ : nullptr;
    }

    /// \brief Record a message-processing outcome for the circuit breaker.
    ///
    /// Must only be called when \c quarantine_enabled() is \c true.
    /// On success in \c kHalfOpen, closes the circuit. On failure,
    /// advances the rate tracker, updates the EMA, and trips the
    /// circuit or escalates to quarantine if thresholds are exceeded.
    ///
    /// \param[in] success \c true if the message handler completed
    ///                    without error; \c false if deserialization
    ///                    or processing failed.
    /// \note Thread safety: must be called from the owning scheduler
    ///       thread (same thread as \c receive()).
    void record_circuit_breaker_result(bool success);

    /// \brief Record a timeout event for circuit breaker rate tracking.
    ///
    /// Advances the timeout bucket, computes the timeout rate over the
    /// observation window, and trips the circuit if the rate exceeds
    /// \c timeout_rate_threshold. A no-op when quarantine is disabled
    /// or the threshold is zero.
    ///
    /// \note Thread safety: must be called from the owning scheduler
    ///       thread (same thread as \c receive()).
    void record_circuit_breaker_timeout();

    /// \brief Check mailbox pressure and trip circuit if threshold exceeded.
    ///
    /// Compares the current mailbox pressure ratio (0.0–1.0) against
    /// \c mailbox_pressure_threshold. When exceeded, trips the circuit
    /// to \c kOpen. A no-op when quarantine is disabled or the threshold
    /// is zero.
    ///
    /// \note Thread safety: must be called from the owning scheduler
    ///       thread (same thread as \c receive()).
    void check_mailbox_pressure();

  private:
    /// \brief Transition the circuit to \c kOpen and emit a metric.
    ///
    /// \param[in] now Current steady_clock timestamp.
    /// \note Thread safety: must be called from the owning scheduler thread.
    void trip_circuit(std::chrono::steady_clock::time_point now);

  public:
    // System message handlers invoked from receive().
    bool handle_link_msg(const TypedMessage& msg);
    bool handle_unlink_msg(const TypedMessage& msg);
    bool handle_monitor_msg(const TypedMessage& msg);
    bool handle_demonitor_msg(const TypedMessage& msg);
    void handle_down_msg(const TypedMessage& msg);

    // Pipeline stages extracted from receive().
    bool apply_drain_gate(TypedMessage& msg);
    void try_drain_completion();
    bool dispatch_system_message(const TypedMessage& msg);
    bool apply_lifecycle_gate(const TypedMessage& msg);
    bool dispatch_cli_message(TypedMessage& msg);
    void dispatch_user_message(TypedMessage& msg);

    // Drain timer management.
    void start_drain_timer();
    void cancel_drain_timer();

    void set_scheduler(sched::IScheduler* scheduler) override {
        scheduler_ = scheduler;
    }
    void set_mailbox(mailbox::MPSCActorMailbox<TypedMessage>* mailbox) override {
        mailbox_ = mailbox;
    }

    void set_metrics_ring_buffer(void* buf) noexcept override {
        metrics_ring_buffer_ =
            static_cast<metrics::MpscRingBuffer<metrics::MetricEvent>*>(buf);
    }

    /// \brief Access the actor's metrics ring buffer.
    ///
    /// \return Pointer to the ring buffer, or \c nullptr if metrics are
    ///         not configured for this actor.
    [[nodiscard]] metrics::MpscRingBuffer<metrics::MetricEvent>*
    metrics_ring_buffer() noexcept {
        return metrics_ring_buffer_;
    }

    void set_logger(void* logger) noexcept override {
        logger_ = static_cast<log::Logger*>(logger);
    }

    cli::MboxSnapshot mailbox_snapshot() const override;

  protected:
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_{nullptr};
    log::Logger* logger_{nullptr};

    /// \brief Override to return the actor's initial behavior.
    ///
    /// Default returns an empty (no-op) behavior.
    virtual Behavior make_behavior() {
        return {};
    }

    /// \brief Override to register protobuf handlers.
    ///
    /// Call \c on<T>() and \c on_request<ReqT,ResT>() from this override.
    virtual void register_handlers() {}

    /// \brief Called by the framework after construction.
    ///
    /// Invokes \c register_handlers() and sets up the protobuf dispatch table.
    void initialize_proto_handlers();

  public:
    void on_activate() override;
    void on_deactivate() override;

    // Get TypeTag for a protobuf type from MessageTraits (compile-time
    // dispatch)
    template <typename ProtoMsgT> TypeTag type_tag_for() const {
        return MessageTraits<ProtoMsgT>::tag();
    }

  public:
    virtual void on_exit();

    void set_exit_reason(uint32_t code) {
        exit_reason_ = code;
    }
    uint32_t exit_reason() const {
        return exit_reason_;
    }

    EventBasedActor(ActorContext* ctx, ActorSystem& sys);

  private:
#if HPACTOR_SUPPORT_COROUTINES
    sched::ActorCoroutine actor_coroutine_;
    std::coroutine_handle<sched::CoroutinePromise> coro_handle_;
#endif
    Behavior behavior_;
    ActorState actor_state_;
    uint32_t exit_reason_ = 0;
    mailbox::MPSCActorMailbox<TypedMessage>* mailbox_ = nullptr;
    sched::IScheduler* scheduler_ = nullptr;
    sched::TimerHandle drain_timer_handle_{};

    bool handlers_initialized_ = false;

    // Quarantine & circuit breaker (opt-in via QuarantinePolicy::enabled)
    QuarantinePolicy quarantine_policy_{};
    CircuitBreakerTracker circuit_breaker_{};
    FailureRateTracker failure_rate_tracker_{};

    using ProtoHandlerMap =
        std::unordered_map<TypeTag, ProtoHandler, std::hash<TypeTag>, std::equal_to<>,
                           mem::MemStdAllocator<std::pair<const TypeTag, ProtoHandler>>>;
    ProtoHandlerMap proto_handlers_{
        mem::MemStdAllocator<std::pair<const TypeTag, ProtoHandler>>(
            id_ptr(), mem::RegionType::kActor)};
};

} // namespace hpactor
