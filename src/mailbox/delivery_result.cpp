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

#include <hpactor/mailbox/delivery_result.hpp>

namespace hpactor::mailbox {

DeliveryResult EnqueueResult::to_delivery_result(const ActorAddress& target_addr,
                                                 MessageId msg_id) const {
    return DeliveryResult::from_enqueue(*this, target_addr, msg_id);
}

const char* to_string(DeliveryStatus s) noexcept {
    switch (s) {
        case DeliveryStatus::Accepted:
            return "accepted";
        case DeliveryStatus::AcceptedWithPressure:
            return "accepted_with_pressure";
        case DeliveryStatus::NoRoute:
            return "no_route";
        case DeliveryStatus::ActorDead:
            return "actor_dead";
        case DeliveryStatus::MailboxFull:
            return "mailbox_full";
        case DeliveryStatus::Expired:
            return "expired";
        case DeliveryStatus::Duplicate:
            return "duplicate";
        case DeliveryStatus::RemoteUnavailable:
            return "remote_unavailable";
        case DeliveryStatus::RejectedByPolicy:
            return "rejected_by_policy";
        case DeliveryStatus::SerializationError:
            return "serialization_error";
        case DeliveryStatus::TransportError:
            return "transport_error";
        case DeliveryStatus::ShuttingDown:
            return "shutting_down";
    }
    return "accepted";
}

DeliveryResult
DeliveryResult::from_enqueue(const EnqueueResult& er,
                             const ActorAddress& target_addr, MessageId msg_id) {
    DeliveryResult dr;
    dr.target = target_addr;
    dr.message_id = msg_id;

    switch (er.code) {
        case EnqueueResultCode::Accepted:
            dr.status = DeliveryStatus::Accepted;
            break;
        case EnqueueResultCode::AcceptedWithSoftPressure:
            dr.status = DeliveryStatus::AcceptedWithPressure;
            break;
        case EnqueueResultCode::Rejected:
            dr.status = DeliveryStatus::MailboxFull;
            break;
        case EnqueueResultCode::DroppedNewest:
        case EnqueueResultCode::DroppedExisting:
            dr.status = DeliveryStatus::MailboxFull;
            break;
        case EnqueueResultCode::ReroutedToDeadLetter:
            dr.status = DeliveryStatus::RejectedByPolicy;
            break;
        case EnqueueResultCode::ReroutedToOverflow:
            dr.status = DeliveryStatus::AcceptedWithPressure;
            break;
        case EnqueueResultCode::MailboxClosed:
            dr.status = DeliveryStatus::ActorDead;
            break;
        case EnqueueResultCode::ActorNotFound:
            dr.status = DeliveryStatus::NoRoute;
            break;
        case EnqueueResultCode::EndpointBackpressure:
            dr.status = DeliveryStatus::RemoteUnavailable;
            break;
        case EnqueueResultCode::EndpointCircuitOpen:
            dr.status = DeliveryStatus::RemoteUnavailable;
            break;
    }

    dr.detail_code = static_cast<uint32_t>(er.code);
    return dr;
}

DeliveryResult DeliveryResult::from_transport(TransportSendResult tsr,
                                              const ActorAddress& target_addr,
                                              MessageId msg_id) {
    DeliveryResult dr;
    dr.target = target_addr;
    dr.message_id = msg_id;

    switch (tsr) {
        case TransportSendResult::Sent:
            dr.status = DeliveryStatus::Accepted;
            break;
        case TransportSendResult::NotConnected:
        case TransportSendResult::QueueFull:
        case TransportSendResult::CircuitOpen:
            dr.status = DeliveryStatus::RemoteUnavailable;
            break;
        case TransportSendResult::EncodeError:
            dr.status = DeliveryStatus::SerializationError;
            break;
        case TransportSendResult::ShuttingDown:
            dr.status = DeliveryStatus::ShuttingDown;
            break;
        case TransportSendResult::WriteError:
            dr.status = DeliveryStatus::TransportError;
            break;
    }

    dr.detail_code = static_cast<uint32_t>(tsr);
    return dr;
}

} // namespace hpactor::mailbox
