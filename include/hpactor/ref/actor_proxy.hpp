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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {

// Forward declarations
namespace net {
class Transport;
}

// -----------------------------------------------------------------------------
// ActorProxy - reference to a remote actor
// -----------------------------------------------------------------------------
// ActorProxy represents an actor on a remote node. It holds the actor's
// address and a reference to the transport used to communicate with it.
// Messages sent to a remote actor go through the transport layer.
// -----------------------------------------------------------------------------
class ActorProxy {
public:
    // Create an actor proxy for a remote actor
    // The transport pointer must outlive this proxy
    ActorProxy(ActorAddress address, net::Transport* transport);

    // Get the actor's address
    ActorAddress address() const { return address_; }

    // Get the endpoint where this actor resides
    CommunicationEndpoint endpoint() const { return address_.endpoint; }

    // Check if this is a local actor (always false for proxy)
    bool is_local() const { return false; }

    // Check if this actor is valid (has a valid address)
    explicit operator bool() const {
        return address_.operator bool();
    }

    // Send a message to this actor (fire-and-forget)
    // Serialization will be implemented in Phase 3
    void send(const ActorAddress& target, MessageVariant msg);

    // Access the underlying transport (for internal use)
    net::Transport* transport() const { return transport_; }

private:
    ActorAddress address_;
    net::Transport* transport_;  // Non-owning pointer to the transport
};

} // namespace hpactor
