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
#include <hpactor/types/serialization.hpp>

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
    bytes payload;
    uint32_t flags = 0;
    uint64_t message_id = 0;

    // Encode frame to bytes
    bytes encode() const;

    // Decode frame from bytes
    static WireFrame decode(const bytes& data);

    // Calculate header size for given sender and receiver node_ids
    static size_t calculate_header_size(const ActorAddress& sender, const ActorAddress& receiver);

    // Endpoint serialization (network byte order)
    // Wire format: [protocol:1][addr:n][port:2]
    //   protocol: 0x04 = IPv4, 0x06 = IPv6
    //   IPv4: 7 bytes total (1 + 4 + 2)
    //   IPv6: 19 bytes total (1 + 16 + 2)
    static bytes encode_endpoint(const CommunicationEndpoint& ep);
    static CommunicationEndpoint decode_endpoint(bytes data);

    // Flag constants
    static constexpr uint32_t Important = 1 << 0;  // Requires delivery confirmation
    static constexpr uint32_t NoDrop = 1 << 1;       // Don't drop on congestion
    static constexpr uint32_t RpcRequest = 1 << 2;    // This frame is an RPC request
    static constexpr uint32_t RpcResponse = 1 << 3;  // This frame is an RPC response
    static constexpr uint32_t RpcIdempotent = 1 << 4; // Set by client on retries; server MUST
                                                      // deduplicate by MessageId before processing

    // Type tag for message payload (set during protobuf serialization)
    uint32_t type_tag = 0;
};

// Protobuf interop - convert between HPActor types and protobuf bytes
namespace hpactor { namespace net {
bytes frame_to_proto(const WireFrame& frame);
WireFrame frame_from_proto(const bytes& data);
}} // namespace hpactor::net

// -----------------------------------------------------------------------------
// Frame header constants (fixed-size portion only)
// -----------------------------------------------------------------------------
constexpr size_t PayloadLengthOffset = 0;
constexpr size_t TypeTagOffset = 4;
constexpr size_t FlagsOffset = 8;
constexpr size_t MessageIdOffset = 12;
constexpr size_t FixedHeaderSize = 20;  // Size of fixed portion (before node_id strings)

} // namespace net
} // namespace hpactor
