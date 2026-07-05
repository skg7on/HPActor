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

#include <hpactor/runtime/messaging_runtime.hpp>

#include <hpactor/actor/system/actor_directory.hpp>

namespace hpactor {

MessagingRuntime::MessagingRuntime(Dependencies deps, const Config& config)
    : network_emitters_(deps.network), // copy into stable member BEFORE taking
                                       // addresses
      dead_letters_(config.dead_letters), dedup_cache_(adt::DedupCache::Config{}),
      outbound_tracker_(), reliable_tracker_(mailbox::ReliableRetryPolicy{}),
      backpressure_(BackpressureCoordinator::Config{
          .metrics = deps.metrics,
          .wire_port = &network_emitters_.backpressure, // stable: points to
                                                        // member, not parameter
          .actor_directory = &deps.actors,
          .endpoint = deps.endpoint,
      }),
      delivery_pipeline_(mailbox::DeliveryPipeline::Config{
          .dlq = &dead_letters_,
          .metrics = deps.metrics,
          .dedup_cache = &dedup_cache_,
          .endpoint = deps.endpoint,
          .default_message_ttl_ms = config.default_message_ttl,
          .actors = &deps.actors,
          .backpressure = &backpressure_,
          .outbound_tracker = &outbound_tracker_,
          .reliable_ack = &network_emitters_.reliable_ack, // stable: points to
                                                           // member, not
                                                           // parameter
      }),
      local_delivery_engine_(deps.actors) {}

MessagingRuntime::~MessagingRuntime() = default;

mailbox::EnqueueResult
MessagingRuntime::try_deliver(ActorId target, TypedMessage msg,
                              uint8_t priority, int64_t deadline_ns,
                              mailbox::DeliveryOptions options) {
    return delivery_pipeline_.try_deliver(target, std::move(msg), priority,
                                          deadline_ns, options);
}

mailbox::DeliveryResult
MessagingRuntime::deliver_with_result(ActorId target, TypedMessage msg,
                                      uint8_t priority, int64_t deadline_ns,
                                      mailbox::DeliveryOptions options) {
    return delivery_pipeline_.deliver_with_result(target, std::move(msg),
                                                  priority, deadline_ns, options);
}

mailbox::EnqueueResult
MessagingRuntime::try_deliver_fast(ActorId target, TypedMessage msg,
                                   FastDeliveryReason reason) {
    (void)reason; // observed in debug/test instrumentation; enforced by
                  // architecture allowlist
    return local_delivery_engine_.try_deliver(target, std::move(msg));
}

void MessagingRuntime::on_reliable_ack(MessageId message_id,
                                       EndPoint endpoint) noexcept {
    outbound_tracker_.on_ack(message_id, endpoint);
}

void MessagingRuntime::on_reliable_nack(MessageId message_id, EndPoint endpoint,
                                        uint32_t reason_code,
                                        uint32_t retry_after_ms) noexcept {
    outbound_tracker_.on_nack(message_id, endpoint, reason_code, retry_after_ms);
}

void MessagingRuntime::reconfigure(const mailbox::DeadLetterConfig& dead_letters) noexcept {
    dead_letters_.reconfigure(dead_letters);
}

mailbox::DeadLetterQueue& MessagingRuntime::dead_letters() noexcept {
    return dead_letters_;
}
adt::DedupCache& MessagingRuntime::dedup_cache() noexcept {
    return dedup_cache_;
}
msg::OutboundDeliveryTracker& MessagingRuntime::delivery_receipt_tracker() noexcept {
    return outbound_tracker_;
}
mailbox::OutboundTracker& MessagingRuntime::mailbox_reliable_tracker() noexcept {
    return reliable_tracker_;
}
BackpressureCoordinator& MessagingRuntime::backpressure() noexcept {
    return backpressure_;
}
mailbox::DeliveryPipeline& MessagingRuntime::delivery_pipeline() noexcept {
    return delivery_pipeline_;
}
LocalDeliveryEngine& MessagingRuntime::local_delivery_engine() noexcept {
    return local_delivery_engine_;
}

} // namespace hpactor
