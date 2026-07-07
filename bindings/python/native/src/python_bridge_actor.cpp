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
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <memory>

namespace hpactor::python {

PythonBridgeActor::PythonBridgeActor(ActorContext* context, ActorSystem& system,
                                     PythonRuntime& runtime,
                                     PythonActorLease lease) noexcept
    : EventBasedActor(context, system), runtime_(runtime),
      lease_(std::move(lease)) {
    become(make_behavior());
}

void PythonBridgeActor::on_activate() {
    EventBasedActor::on_activate();
    [[maybe_unused]] bool bound = lease_.bind(id());
}

void PythonBridgeActor::on_deactivate() {
    lease_.reset();
    EventBasedActor::on_deactivate();
}

uint64_t PythonBridgeActor::generation() const noexcept {
    return lease_.generation();
}

void PythonBridgeActor::receive(TypedMessage& message) {
    // System messages (tag < TypeTag::User) are handled by the base class.
    if (static_cast<uint32_t>(message.type_id()) < 0x1000) {
        EventBasedActor::receive(message);
        return;
    }

    // User message: apply drain and lifecycle gates.
    if (!apply_drain_gate(message) || !apply_lifecycle_gate(message)) {
        return;
    }

    // Capture sender and ask correlation for reply routing.
    auto* ctx = context();
    if (ctx != nullptr) {
        ctx->set_current_sender(message.sender_address());
        ctx->set_current_ask_message_id(message.ask_message_id());
    }

    // Build the dispatch envelope.
    auto envelope = std::make_shared<PythonDispatchEnvelope>();
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

    // Reliable ACK/NACK based on transfer success.
    if (message.ack_requested()) {
        if (accepted) {
            // ACK status 0 = Accepted.
            system().send_reliable_ack(message.sender_address(), address(),
                                       message.message_id(),
                                       static_cast<uint8_t>(0), 0);
        } else {
            // NACK status 1 = Rejected, retry after 500 ms.
            system().send_reliable_ack(message.sender_address(), address(),
                                       message.message_id(),
                                       static_cast<uint8_t>(1), 500);
        }
    }

    try_drain_completion();
    check_mailbox_pressure();

    // Clear the ask correlation so it does not leak into the next message.
    if (ctx != nullptr) {
        ctx->set_current_ask_message_id(0);
    }
}

} // namespace hpactor::python
