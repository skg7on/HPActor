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

#include <hpactor/runtime/messaging_network_emitters.hpp>

#include <hpactor/adt/dedup_cache.hpp>
#include <hpactor/mailbox/backpressure_coordinator.hpp>
#include <hpactor/mailbox/delivery_pipeline.hpp>
#include <hpactor/mailbox/local_delivery_engine.hpp>
#include <hpactor/mailbox/outbound_tracker.hpp>
#include <hpactor/mailbox/reliable_retry_policy.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/delivery_result.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/outbound_delivery_tracker.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>

namespace hpactor {

class ActorDirectory;

/// \brief Fast-delivery reason classification.
///
/// Every call to \c try_deliver_fast() must supply a reason.
/// Ordinary delivery must use the full \c DeliveryPipeline.
enum class FastDeliveryReason : uint8_t {
    /// Stream protocol handler (Phase 3 transitional; moves in Phase 4).
    StreamProtocol,

    /// Public compatibility API (\c try_deliver_local_fast).
    CompatibilityExplicit,
};

/// \brief Cohesive owner of all ActorSystem messaging state and delivery
///        policy.
///
/// Owns the dead-letter queue, receiver dedup cache, reliable and
/// compatibility outbound trackers, backpressure coordinator, full
/// delivery pipeline, and fast local delivery engine.
///
/// All dependencies are fixed at construction — no late setters, no
/// facade-capturing callbacks, no \c std::function on hot paths.
///
/// \note Thread safety: Individual components own their synchronization.
///       \c MessagingRuntime itself adds no component-wide lock.
class MessagingRuntime final {
  public:
    /// \brief Non-owning dependencies that must outlive this component.
    struct Dependencies {
        ActorDirectory& actors;
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics;
        MessagingNetworkEmitters network;
        EndPoint endpoint;
    };

    /// \brief Validated configuration consumed at construction.
    struct Config {
        mailbox::DeadLetterConfig dead_letters;
        std::chrono::milliseconds default_message_ttl{0};
    };

    /// \brief Construct all messaging components in dependency order.
    ///
    /// \param[in] deps   Stable non-owning references/ports.
    /// \param[in] config Validated effective configuration.
    MessagingRuntime(Dependencies deps, const Config& config);

    ~MessagingRuntime();

    MessagingRuntime(const MessagingRuntime&) = delete;
    MessagingRuntime& operator=(const MessagingRuntime&) = delete;
    MessagingRuntime(MessagingRuntime&&) = delete;
    MessagingRuntime& operator=(MessagingRuntime&&) = delete;

    // ── Delivery ──────────────────────────────────────────────────────

    mailbox::EnqueueResult
    try_deliver(ActorId target, TypedMessage msg, uint8_t priority,
                int64_t deadline_ns, mailbox::DeliveryOptions options);

    mailbox::DeliveryResult
    deliver_with_result(ActorId target, TypedMessage msg, uint8_t priority,
                        int64_t deadline_ns, mailbox::DeliveryOptions options);

    mailbox::EnqueueResult
    try_deliver_fast(ActorId target, TypedMessage msg, FastDeliveryReason reason);

    // ── Reliable control ──────────────────────────────────────────────

    void on_reliable_ack(MessageId message_id, EndPoint endpoint) noexcept;
    void on_reliable_nack(MessageId message_id, EndPoint endpoint,
                          uint32_t reason_code, uint32_t retry_after_ms) noexcept;

    /// \brief Process pending retries for at-least-once delivery.
    ///
    /// Invokes the current tracker's retry logic with a caller-supplied
    /// resend port.  Transport resend remains a characterized gap.
    template <typename ResendFn>
    void process_retries(uint64_t now_ns, ResendFn&& resend) noexcept {
        outbound_tracker_.process_retries(now_ns, std::forward<ResendFn>(resend));
    }

    // ── Reconfiguration ────────────────────────────────────────────────

    /// \brief Apply live-reloadable configuration changes.
    ///
    /// Preserves object identity for all owned components (DLQ, dedup,
    /// trackers, coordinator, pipeline, engine).  Only fields classified
    /// as live-reloadable by the current implementation are applied.
    ///
    /// \param[in] dead_letters New dead-letter queue configuration.
    void reconfigure(const mailbox::DeadLetterConfig& dead_letters) noexcept;

    // ── Accessors (stable addresses) ──────────────────────────────────

    mailbox::DeadLetterQueue& dead_letters() noexcept;
    adt::DedupCache& dedup_cache() noexcept;
    msg::OutboundDeliveryTracker& delivery_receipt_tracker() noexcept;
    mailbox::OutboundTracker& mailbox_reliable_tracker() noexcept;
    BackpressureCoordinator& backpressure() noexcept;
    mailbox::DeliveryPipeline& delivery_pipeline() noexcept;
    LocalDeliveryEngine& local_delivery_engine() noexcept;

  private:
    // Members in dependency order (destructed in reverse).
    // network_emitters_ MUST precede backpressure_ and delivery_pipeline_
    // because their Config structs store pointers into network_emitters_.
    MessagingNetworkEmitters network_emitters_;
    mailbox::DeadLetterQueue dead_letters_;
    adt::DedupCache dedup_cache_;
    msg::OutboundDeliveryTracker outbound_tracker_;
    mailbox::OutboundTracker reliable_tracker_;
    BackpressureCoordinator backpressure_;
    mailbox::DeliveryPipeline delivery_pipeline_;
    LocalDeliveryEngine local_delivery_engine_;
};

} // namespace hpactor
