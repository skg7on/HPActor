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

#include <hpactor/adt/dedup_cache.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace hpactor {

class AbstractActor;

namespace mailbox {

class DeadLetterQueue;

template <typename T> class MPSCActorMailbox;

/// \brief Message delivery pipeline: dedup → expiration → circuit breaker →
///        mailbox enqueue → observability → backpressure.
///
/// Extracted from \c ActorSystem to isolate message admission logic.
/// All external dependencies are constructor-injected so the pipeline
/// can be tested independently.
///
/// \note Thread safety: Not internally synchronized beyond what the
///       injected components provide. The pipeline is designed to be
///       called from the scheduler thread or a single producer thread.
class DeliveryPipeline {
  public:
    /// \brief Callback to emit a backpressure signal to a local sender.
    ///
    /// \param[in] signal The backpressure signal to deliver to the sender's
    ///                   \c ActorContext.
    /// \param[in] state  Current mailbox pressure state for metrics tagging.
    using LocalBackpressureEmitter =
        std::function<void(const BackpressureSignal& signal, MailboxPressureState state)>;

    /// \brief Callback to serialize and send a backpressure signal remotely.
    ///
    /// \param[in] signal The backpressure signal to serialize and wire-send.
    /// \param[in] state  Current mailbox pressure state for metrics tagging.
    using RemoteBackpressureEmitter =
        std::function<void(const BackpressureSignal& signal, MailboxPressureState state)>;

    /// \brief Injected configuration and dependencies.
    struct Config {
        /// \brief Dead-letter queue for rejected/expired messages.
        DeadLetterQueue* dlq = nullptr;

        /// \brief Metrics ring buffer for delivery telemetry.
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics = nullptr;

        /// \brief Dedup cache for at-least-once delivery deduplication.
        adt::DedupCache* dedup_cache = nullptr;

        /// \brief Local endpoint for source identification.
        EndPoint endpoint{};

        /// \brief System-wide default TTL applied when no explicit deadline
        ///        is set. Zero means no default TTL.
        std::chrono::milliseconds default_message_ttl_ms{0};

        /// \brief Lookup an actor instance by ID.
        std::function<std::shared_ptr<AbstractActor>(ActorId)> get_actor;

        /// \brief Lookup a mailbox by actor ID.
        std::function<MPSCActorMailbox<TypedMessage>*(ActorId)> get_mailbox;

        /// \brief Emit a backpressure signal to a local sender.
        LocalBackpressureEmitter emit_local_backpressure;

        /// \brief Emit a backpressure signal to a remote sender.
        RemoteBackpressureEmitter emit_remote_backpressure;
    };

    /// \brief Construct the delivery pipeline with injected dependencies.
    ///
    /// \param[in] config Injected configuration.  Pointed-to objects
    ///                   (DLQ, metrics ring buffer, dedup cache) must
    ///                   outlive the pipeline.  Callbacks are copied.
    explicit DeliveryPipeline(Config config);

    /// \brief Destroy the pipeline.  No-op — all resources are owned by
    ///        the injected components.
    ~DeliveryPipeline();

    DeliveryPipeline(const DeliveryPipeline&) = delete;
    DeliveryPipeline& operator=(const DeliveryPipeline&) = delete;
    DeliveryPipeline(DeliveryPipeline&&) = delete;
    DeliveryPipeline& operator=(DeliveryPipeline&&) = delete;

    /// \brief Attempt to deliver a message to a local actor with full
    ///        admission control.
    ///
    /// Runs the complete pipeline: actor lookup → circuit breaker gate →
    /// TTL default application → dedup check → deadline check →
    /// mailbox enqueue → rejection observability → backpressure signal.
    ///
    /// \param[in] target      Actor ID to deliver to.
    /// \param[in] msg         Message to deliver (moved into the pipeline).
    /// \param[in] priority    0–3 (0 = highest priority).
    /// \param[in] deadline_ns Absolute delivery deadline in nanoseconds
    ///                       (\c INT64_MAX = no deadline).
    /// \param[in] options     Delivery options (mode, message_id, flags).
    /// \return \c EnqueueResult describing acceptance or rejection.
    /// \retval Accepted              Message was enqueued.
    /// \retval ActorNotFound         Target actor does not exist.
    /// \retval CircuitOpen           Target actor's circuit breaker is open.
    /// \retval Rejected              Mailbox at hard capacity.
    EnqueueResult
    try_deliver(ActorId target, TypedMessage msg, uint8_t priority = 0,
                int64_t deadline_ns = INT64_MAX, DeliveryOptions options = {});

    /// \brief Deliver with a user-facing \c DeliveryResult.
    ///
    /// Wraps \c try_deliver() and converts the internal \c EnqueueResult
    /// to \c DeliveryResult.  Also records the delivery result on the
    /// mailbox and emits a \c kDeliveryResult metric event.
    ///
    /// \param[in] target      Actor ID to deliver to.
    /// \param[in] msg         Message to deliver (moved).
    /// \param[in] priority    0–3 (0 = highest).
    /// \param[in] deadline_ns Absolute delivery deadline.
    /// \param[in] options     Delivery options.
    /// \return \c DeliveryResult describing the delivery outcome.
    DeliveryResult
    deliver_with_result(ActorId target, TypedMessage msg, uint8_t priority = 0,
                        int64_t deadline_ns = INT64_MAX,
                        DeliveryOptions options = {});

    /// \brief Read-only access to the injected configuration.
    ///
    /// \return A const reference to the pipeline's \c Config.
    const Config& config() const noexcept {
        return config_;
    }

    /// \brief Update the metrics ring buffer pointer after construction.
    ///
    /// Used when the ring buffer is created after the pipeline (e.g.,
    /// during \c ActorSystem constructor ordering).
    ///
    /// \param[in] m Pointer to the metrics ring buffer, or \c nullptr.
    void set_metrics(metrics::MpscRingBuffer<metrics::MetricEvent>* m) {
        config_.metrics = m;
    }

  private:
    EnqueueResult
    reject_missing_actor(ActorId target, const TypedMessage& msg,
                         const DeliveryOptions& options, uint8_t priority,
                         int64_t deadline_ns) noexcept;

    std::optional<EnqueueResult>
    check_circuit_breaker(ActorId target, const TypedMessage& msg,
                          const DeliveryOptions& options, uint8_t priority,
                          int64_t deadline_ns);

    int64_t apply_default_ttl(int64_t deadline_ns) const;

    std::optional<EnqueueResult>
    check_duplicate(ActorId target, const TypedMessage& msg,
                    const DeliveryOptions& options);

    std::optional<EnqueueResult>
    check_expired(ActorId target, const TypedMessage& msg,
                  const DeliveryOptions& options, uint8_t priority,
                  int64_t deadline_ns);

    void
    emit_rejection_observability(ActorId target, const StreamBuffer& msg_payload,
                                 const TraceContext& msg_trace, bool msg_has_trace,
                                 const MailboxEnvelopeMeta& meta,
                                 const EnqueueResult& result,
                                 const DeliveryOptions& options,
                                 OverflowPolicy overflow_policy);

    static void
    set_dlq_trace_fields(DeadLetterRecord& dl, const TraceContext& tc) noexcept;

    static bool should_dead_letter(DeadLetterQueue* dlq, DeliveryMode mode) noexcept;

    void
    maybe_emit_backpressure(MPSCActorMailbox<TypedMessage>* mailbox,
                            const EnqueueResult& result,
                            const MailboxEnvelopeMeta& meta, bool emit_requested,
                            BackpressureMode backpressure_mode,
                            bool sender_is_remote);

    void push_circuit_open_dlq(ActorId target, const TypedMessage& msg,
                               const DeliveryOptions& options, uint8_t priority,
                               int64_t deadline_ns);

    Config config_;
};

} // namespace mailbox
} // namespace hpactor
