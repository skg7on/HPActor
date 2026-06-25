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

// ActorProxy implementation - see actor_proxy.hpp

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_proxy.hpp>

namespace hpactor {

ActorProxy::ActorProxy(ActorAddress address, net::Transport* transport)
    : address_(address), transport_(transport) {}

ActorProxy::ActorProxy(const ActorAddress& addr, ActorSystem* system)
    : address_(addr),
      transport_(system != nullptr ? system->get_transport_for(addr.endpoint)
                                   : nullptr),
      system_(system) {}

void ActorProxy::send(const ActorAddress& target, TypedMessage msg) {
    (void)try_send(target, std::move(msg));
}

mailbox::DeliveryResult
ActorProxy::try_send(const ActorAddress& target, TypedMessage msg,
                     mailbox::DeliveryOptions options) {
    if (transport_ == nullptr) {
        // Capture dead letter: no transport available
        if (system_) {
            mailbox::DeadLetterRecord dl;
            dl.reason = mailbox::DeadLetterReason::RemoteNodeUnreachable;
            dl.source = mailbox::DeadLetterSource::ActorProxy;
            dl.sender = msg.sender_address().id != ActorId{0}
                            ? msg.sender_address()
                            : address_;
            dl.target = target;
            dl.type_tag = msg.type_id();
            dl.payload_sample = msg.payload();
            (void)system_->dead_letter(std::move(dl));
        }
        return {mailbox::DeliveryStatus::NoRoute, target,
                MessageId{options.message_id}, 0};
    }

    // Resolve via location cache or discovery
    ActorAddress resolved_target = target;
    if (location_cache_) {
        auto cached = location_cache_->get(target.id);
        if (cached) {
            resolved_target.endpoint = *cached;
        }
    }
    if (discovery_) {
        auto* member = discovery_->discover(resolved_target.endpoint);
        if (!member) {
            // Capture dead letter: no route to target
            if (system_) {
                mailbox::DeadLetterRecord dl;
                dl.reason = mailbox::DeadLetterReason::MissingRoute;
                dl.source = mailbox::DeadLetterSource::ServiceDiscovery;
                dl.sender = msg.sender_address().id != ActorId{0}
                                ? msg.sender_address()
                                : address_;
                dl.target = target;
                dl.type_tag = msg.type_id();
                dl.payload_sample = msg.payload();
                (void)system_->dead_letter(std::move(dl));
            }
            return {mailbox::DeliveryStatus::NoRoute, target,
                    MessageId{options.message_id}, 0};
        }
        resolved_target.endpoint = member->identity.endpoint;
        if (location_cache_) {
            location_cache_->put(target.id, resolved_target.endpoint);
        }
    }

    net::WireFrame frame;
    // Use msg.sender_address() if present, fall back to the proxy address
    const auto& sender_addr =
        msg.sender_address().id != ActorId{0} ? msg.sender_address() : address_;
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_sender(),
                  sender_addr);
    net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_receiver(),
                  resolved_target);
    auto msg_id = generate_message_id();
    frame.pb_envelope.mutable_data_frame()->set_message_id(msg_id.value());
    frame.pb_envelope.mutable_data_frame()->set_type_tag(
        static_cast<uint32_t>(msg.type_id()));
    frame.pb_envelope.mutable_data_frame()->set_payload(
        reinterpret_cast<const char*>(msg.payload().data()), msg.payload().size());

    if (msg.has_trace_context()) {
        net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_trace_context(),
                      msg.trace_context());
    }

    auto tsr = transport_->try_send(resolved_target, frame.encode());
    if (tsr != TransportSendResult::Sent) {
        // Capture dead letter: transport refused the message
        if (system_) {
            mailbox::DeadLetterRecord dl;
            dl.reason = mailbox::DeadLetterReason::TransportSendFailed;
            dl.source = mailbox::DeadLetterSource::Transport;
            dl.sender = msg.sender_address().id != ActorId{0}
                            ? msg.sender_address()
                            : address_;
            dl.target = target;
            dl.type_tag = msg.type_id();
            dl.payload_sample = msg.payload();
            (void)system_->dead_letter(std::move(dl));
        }
        return mailbox::DeliveryResult::from_transport(tsr, target, msg_id);
    }
    return mailbox::DeliveryResult::from_transport(tsr, target, msg_id);
}

mailbox::DeliveryResult
ActorProxy::try_send_batch(const ActorAddress& target,
                           std::vector<TypedMessage> msgs,
                           mailbox::DeliveryOptions options) {
    if (msgs.empty()) {
        return {mailbox::DeliveryStatus::Accepted, target, MessageId{0}, 0};
    }

    if (transport_ == nullptr) {
        if (system_) {
            for (auto& msg : msgs) {
                mailbox::DeadLetterRecord dl;
                dl.reason = mailbox::DeadLetterReason::RemoteNodeUnreachable;
                dl.source = mailbox::DeadLetterSource::ActorProxy;
                dl.sender = msg.sender_address().id != ActorId{0}
                                ? msg.sender_address()
                                : address_;
                dl.target = target;
                dl.type_tag = msg.type_id();
                dl.payload_sample = msg.payload();
                (void)system_->dead_letter(std::move(dl));
            }
        }
        return {mailbox::DeliveryStatus::NoRoute, target,
                MessageId{options.message_id}, 0};
    }

    // Resolve via location cache or discovery
    ActorAddress resolved_target = target;
    if (location_cache_) {
        auto cached = location_cache_->get(target.id);
        if (cached) {
            resolved_target.endpoint = *cached;
        }
    }
    if (discovery_) {
        auto* member = discovery_->discover(resolved_target.endpoint);
        if (!member) {
            if (system_) {
                for (auto& msg : msgs) {
                    mailbox::DeadLetterRecord dl;
                    dl.reason = mailbox::DeadLetterReason::MissingRoute;
                    dl.source = mailbox::DeadLetterSource::ServiceDiscovery;
                    dl.sender = msg.sender_address().id != ActorId{0}
                                    ? msg.sender_address()
                                    : address_;
                    dl.target = target;
                    dl.type_tag = msg.type_id();
                    dl.payload_sample = msg.payload();
                    (void)system_->dead_letter(std::move(dl));
                }
            }
            return {mailbox::DeliveryStatus::NoRoute, target,
                    MessageId{options.message_id}, 0};
        }
        resolved_target.endpoint = member->identity.endpoint;
        if (location_cache_) {
            location_cache_->put(target.id, resolved_target.endpoint);
        }
    }

    // Build BatchMsgFrame
    ::hpactor::net::BatchMsgFrame batch;
    const auto& sender_addr =
        !msgs.empty() && msgs[0].sender_address().id != ActorId{0}
            ? msgs[0].sender_address()
            : address_;
    net::to_proto(batch.mutable_sender(), sender_addr);
    net::to_proto(batch.mutable_receiver(), resolved_target);

    for (auto& msg : msgs) {
        auto* entry = batch.add_entries();
        entry->set_type_tag(static_cast<uint32_t>(msg.type_id()));
        auto msg_id = generate_message_id();
        entry->set_message_id(msg_id.value());
        entry->set_payload(reinterpret_cast<const char*>(msg.payload().data()),
                           msg.payload().size());
        if (msg.has_trace_context()) {
            net::to_proto(entry->mutable_trace_context(), msg.trace_context());
        }
    }

    // Wrap in WireEnvelope and encode
    auto frame = net::WireFrame::from_batch(std::move(batch));
    auto tsr = transport_->try_send_batch(resolved_target, frame.encode());
    if (tsr != TransportSendResult::Sent) {
        if (system_) {
            for (auto& msg : msgs) {
                mailbox::DeadLetterRecord dl;
                dl.reason = mailbox::DeadLetterReason::TransportSendFailed;
                dl.source = mailbox::DeadLetterSource::Transport;
                dl.sender = msg.sender_address().id != ActorId{0}
                                ? msg.sender_address()
                                : address_;
                dl.target = target;
                dl.type_tag = msg.type_id();
                dl.payload_sample = msg.payload();
                (void)system_->dead_letter(std::move(dl));
            }
        }
        return mailbox::DeliveryResult::from_transport(tsr, target, MessageId{0});
    }
    return mailbox::DeliveryResult::from_transport(tsr, target, MessageId{0});
}

} // namespace hpactor
