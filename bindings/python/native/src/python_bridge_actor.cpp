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

#include <hpactor/python/python_bridge_actor.hpp>

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/lifecycle/quarantine_reason.hpp>
#include <hpactor/msg/failure_envelope.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <memory>

namespace hpactor::python {

PythonBridgeActor::PythonBridgeActor(ActorContext* context, ActorSystem& system,
                                     PythonRuntime& runtime, PythonActorLease lease,
                                     PythonReliabilityController& reliability,
                                     PythonSupervisionConfig supervision) noexcept
    : EventBasedActor(context, system), LifecycleActor(), runtime_(runtime),
      lease_(std::move(lease)), reliability_(reliability),
      supervision_(supervision) {
    become(make_behavior());
}

void PythonBridgeActor::on_activate() {
    EventBasedActor::on_activate();
    [[maybe_unused]] bool bound = lease_.bind(id());
    reliability_.register_actor(address(), lease_.generation(), supervision_);
}

void PythonBridgeActor::on_deactivate() {
    lease_.reset();
    reliability_.unregister_actor(address());
    EventBasedActor::on_deactivate();
}

void PythonBridgeActor::on_drain() {
    // Reject new user dispatches; system messages still accepted.
}

void PythonBridgeActor::on_stop() {
    // Enqueue an ActorStopped completion to the Python runtime.
    auto completion = std::make_shared<PythonCompletion>();
    completion->kind = PythonCompletionKind::ActorStopped;
    completion->actor = address();
    completion->generation = lease_.generation();
    (void)runtime_.try_push_completion(completion);
}

void PythonBridgeActor::on_fail(error err) {
    // Build bounded failure metadata.
    PythonFailureMetadata fm;
    fm.reason = static_cast<FailureReason>(err.code());
    fm.source = FailureSource::LanguageBinding;
    fm.error_code = err.code();
    fm.detail = err.message();

    // Enqueue an ActorFailed completion.
    auto completion = std::make_shared<PythonCompletion>();
    completion->kind = PythonCompletionKind::ActorFailed;
    completion->actor = address();
    completion->generation = lease_.generation();
    completion->source = FailureSource::LanguageBinding;
    completion->error_code = err.code();
    (void)runtime_.try_push_completion(completion);

    // Notify the reliability controller.
    // Note: the reliability port is called from the controller.
}

void PythonBridgeActor::on_restart() {
    // Allocate a replacement generation.
    // The old lease is released; a new one is reserved.
    lease_.reset();

    auto new_lease = runtime_.reserve_actor();
    if (new_lease.has_value()) {
        lease_ = std::move(new_lease.value());
        [[maybe_unused]] bool bound = lease_.bind(id());
        reliability_.advance_generation(address(), lease_.generation());
    }

    // Enqueue a Restart dispatch to the Python runtime.
    auto envelope = std::make_shared<PythonDispatchEnvelope>();
    envelope->kind = PythonDispatchKind::Restart;
    envelope->actor = address();
    envelope->generation = lease_.generation();
    envelope->sequence = next_dispatch_sequence_++;
    envelope->failure.reason = FailureReason::Unknown;
    envelope->failure.source = FailureSource::LanguageBinding;

    (void)runtime_.try_push_dispatch(envelope);
}

void PythonBridgeActor::on_quarantined(QuarantineReason /*reason*/) {
    // Enqueue a stop completion with quarantined status.
    auto completion = std::make_shared<PythonCompletion>();
    completion->kind = PythonCompletionKind::ActorStopped;
    completion->actor = address();
    completion->generation = lease_.generation();
    (void)runtime_.try_push_completion(completion);
}

uint64_t PythonBridgeActor::generation() const noexcept {
    return lease_.generation();
}

void PythonBridgeActor::receive(TypedMessage& message) {
    // System messages (tag < 0x1000) are handled by the base class.
    const auto tag_val = static_cast<uint32_t>(message.type_id());

    if (tag_val < 0x1000) {
        EventBasedActor::receive(message);
        return;
    }

    // Phase 1C: check lifecycle gate.
    if (!apply_drain_gate(message) || !apply_lifecycle_gate(message)) {
        return;
    }

    auto* ctx = context();
    if (ctx != nullptr) {
        ctx->set_current_sender(message.sender_address());
        ctx->set_current_ask_message_id(message.ask_message_id());
    }

    auto envelope = std::make_shared<PythonDispatchEnvelope>();
    envelope->kind = PythonDispatchKind::Message;
    envelope->actor = address();
    envelope->generation = lease_.generation();
    envelope->type_tag = message.type_id();
    envelope->payload = message.payload();
    envelope->sender = message.sender_address();
    envelope->message_id = MessageId(message.message_id());
    envelope->ask_message_id = message.ask_message_id();
    envelope->priority = message.delivery_priority();
    envelope->deadline_ns = message.deadline_ns();
    envelope->flags = message.delivery_flags();
    envelope->ack_requested = message.ack_requested();
    envelope->sequence = next_dispatch_sequence_++;

    if (message.has_trace_context()) {
        envelope->trace = message.trace_context();
        envelope->has_trace = true;
    }

    PythonDispatchPtr dispatch = std::move(envelope);
    const bool accepted = runtime_.try_push_dispatch(dispatch);

    if (message.ack_requested()) {
        if (accepted) {
            system().send_reliable_ack(message.sender_address(), address(),
                                       message.message_id(),
                                       static_cast<uint8_t>(0), 0);
        } else {
            system().send_reliable_ack(message.sender_address(), address(),
                                       message.message_id(),
                                       static_cast<uint8_t>(1), 500);
        }
    }

    try_drain_completion();
    check_mailbox_pressure();

    if (ctx != nullptr) {
        ctx->set_current_ask_message_id(0);
    }
}

} // namespace hpactor::python
