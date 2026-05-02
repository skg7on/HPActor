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

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_proxy.hpp>

namespace hpactor {

ActorProxy::ActorProxy(ActorAddress address, net::Transport* transport)
    : address_(address), transport_(transport) {}

ActorProxy::ActorProxy(const ActorAddress& addr, ActorSystem* system)
    : address_(addr),
      transport_(system ? system->get_transport_for(addr.endpoint) : nullptr) {}

void ActorProxy::send(const ActorAddress& target, TypedMessage msg) {
    if (!transport_) {
        return;  // Silently drop; matches fire-and-forget semantics
    }
    net::WireFrame frame;
    // Use msg.sender_address() if present, fall back to the proxy address
    auto& sender_addr = msg.sender_address().id != ActorId{0}
                            ? msg.sender_address()
                            : address_;
    net::to_proto(frame.pb_frame.mutable_sender(), sender_addr);
    net::to_proto(frame.pb_frame.mutable_receiver(), target);
    frame.pb_frame.set_message_id(MessageId::generate().value());
    frame.pb_frame.set_type_tag(static_cast<uint32_t>(msg.type_id()));
    frame.pb_frame.set_payload(
        reinterpret_cast<const char*>(msg.payload().data()),
        msg.payload().size());

    transport_->send(target, frame.encode());
}

} // namespace hpactor
