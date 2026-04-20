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
// Frame - network message frame format
// -----------------------------------------------------------------------------
// Every message sent over the network is wrapped in a Frame.
// Format:
//   [4 bytes: payload length]
//   [4 bytes: type tag]
//   [4 bytes: flags]
//   [8 bytes: message id]
//   [4 bytes: sender node_id]
//   [4 bytes: sender actor_id]
//   [4 bytes: sender incarnation]
//   [4 bytes: receiver node_id]
//   [4 bytes: receiver actor_id]
//   [4 bytes: receiver incarnation]
//   [payload...]
// -----------------------------------------------------------------------------
struct Frame {
    ActorAddress sender;
    ActorAddress receiver;
    bytes payload;
    uint32_t flags = 0;
    uint64_t message_id = 0;

    // Encode frame to bytes
    bytes encode() const;

    // Decode frame from bytes
    static Frame decode(const bytes& data);

    // Flag constants
    static constexpr uint32_t Important = 1 << 0;  // Requires delivery confirmation
    static constexpr uint32_t NoDrop = 1 << 1;       // Don't drop on congestion
    static constexpr uint32_t RpcRequest = 1 << 2;    // This frame is an RPC request
    static constexpr uint32_t RpcResponse = 1 << 3;  // This frame is an RPC response
    static constexpr uint32_t RpcIdempotent = 1 << 4; // Set by client on retries; server MUST
                                                      // deduplicate by MessageId before processing
};

// -----------------------------------------------------------------------------
// Frame header constants
// -----------------------------------------------------------------------------
constexpr size_t FrameHeaderSize = 60;  // Total size without payload
constexpr size_t PayloadLengthOffset = 0;
constexpr size_t TypeTagOffset = 4;
constexpr size_t FlagsOffset = 8;
constexpr size_t MessageIdOffset = 12;
constexpr size_t SenderNodeOffset = 20;
constexpr size_t SenderActorIdOffset = 24;
constexpr size_t SenderIncarnationOffset = 32;
constexpr size_t ReceiverNodeOffset = 40;
constexpr size_t ReceiverActorIdOffset = 44;
constexpr size_t ReceiverIncarnationOffset = 52;
constexpr size_t PayloadOffset = 60;

} // namespace net
} // namespace hpactor
