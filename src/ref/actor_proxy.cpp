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

#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/types/serialization.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/transport.hpp>

namespace hpactor {

ActorProxy::ActorProxy(ActorAddress address, net::Transport* transport)
    : address_(address), transport_(transport) {}

void ActorProxy::send(const ActorAddress& target, MessageVariant msg) {
    // Determine TypeTag using std::visit
    TypeTag tag = std::visit([](const auto& m) -> TypeTag {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, down_msg>) {
            return TypeTag::DownMsg;
        } else if constexpr (std::is_same_v<T, exit_msg>) {
            return TypeTag::ExitMsg;
        } else if constexpr (std::is_same_v<T, link_msg>) {
            return TypeTag::LinkMsg;
        } else if constexpr (std::is_same_v<T, unlink_msg>) {
            return TypeTag::UnlinkMsg;
        } else {
            return TypeTag::User;
        }
    }, msg);

    // Serialize message
    DefaultSerializer serializer;
    bytes payload = serializer.encode(tag, msg);

    // Create frame
    net::WireFrame frame;
    frame.sender = address_;       // This proxy's address (sender side)
    frame.receiver = target;      // Target actor address
    frame.message_id = MessageId::generate().value();
    frame.payload = std::move(payload);

    // Send via transport
    transport_->send(target, frame.encode());
}

} // namespace hpactor
