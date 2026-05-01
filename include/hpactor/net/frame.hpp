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

#include <span>

namespace hpactor {

namespace net {

// -----------------------------------------------------------------------------
// WireFrame - network message frame format (wire format for serialization)
// -----------------------------------------------------------------------------
// Every message sent over the network is wrapped in a WireFrame.
// Format (NodeId is now "host:port" string):
//   [4 bytes: payload length]
//   [4 bytes: type tag]
//   [4 bytes: flags]
//   [8 bytes: message id]
//   [4 bytes: sender node_id length]
//   [N bytes: sender node_id string]
//   [8 bytes: sender actor_id]
//   [8 bytes: sender incarnation]
//   [4 bytes: receiver node_id length]
//   [M bytes: receiver node_id string]
//   [8 bytes: receiver actor_id]
//   [8 bytes: receiver incarnation]
//   [payload...]
// -----------------------------------------------------------------------------
struct WireFrame {
    ActorAddress sender;
    ActorAddress receiver;
    StreamBuffer payload;
    uint32_t flags = 0;
    uint64_t message_id = 0;

    // Encode frame to bytes
    StreamBuffer encode() const;

    // Decode frame from bytes
    static WireFrame decode(const StreamBuffer& data);

    // Decode frame from span — ownership-boundary copy into payload
    static WireFrame decode(std::span<const uint8_t> data);

    // Flag constants
    static constexpr uint32_t Important = 1 << 0;   // Requires delivery
                                                    // confirmation
    static constexpr uint32_t NoDrop = 1 << 1;      // Don't drop on congestion
    static constexpr uint32_t RpcRequest = 1 << 2;  // This frame is an RPC
                                                    // request
    static constexpr uint32_t RpcResponse = 1 << 3; // This frame is an RPC
                                                    // response
    static constexpr uint32_t RpcIdempotent = 1 << 4; // Set by client on
                                                      // retries; server MUST
                                                      // deduplicate by
                                                      // MessageId before
                                                      // processing

    // Type tag for message payload (set during protobuf serialization)
    uint32_t type_tag = 0;
};

// Protobuf interop - convert between HPActor types and protobuf bytes
StreamBuffer frame_to_proto(const WireFrame& frame);
WireFrame frame_from_proto(const StreamBuffer& data);

} // namespace net
} // namespace hpactor
