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

#include <hpactor/net/frame.hpp>

#include <cstring>

namespace hpactor {

namespace net {

size_t Frame::calculate_header_size(const ActorAddress& sender, const ActorAddress& receiver) {
    // Fixed header + sender node_id (4 bytes length + string) + sender actor_id + sender incarnation
    // + receiver node_id (4 bytes length + string) + receiver actor_id + receiver incarnation
    return FixedHeaderSize +
           sizeof(uint32_t) + sender.node_id.size() +  // sender node_id length + string
           sizeof(uint64_t) + sizeof(uint64_t) +        // sender actor_id + incarnation
           sizeof(uint32_t) + receiver.node_id.size() + // receiver node_id length + string
           sizeof(uint64_t) + sizeof(uint64_t);        // receiver actor_id + incarnation
}

bytes Frame::encode() const {
    size_t header_size = calculate_header_size(sender, receiver);
    bytes result;
    result.resize(header_size + payload.size());

    uint32_t payload_len = static_cast<uint32_t>(payload.size());
    uint32_t type_tag = 0;  // TODO: extract type tag from payload
    uint64_t msg_id = message_id;

    size_t offset = 0;

    // Payload length
    std::memcpy(result.data() + offset, &payload_len, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Type tag
    std::memcpy(result.data() + offset, &type_tag, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Flags
    std::memcpy(result.data() + offset, &flags, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Message ID
    std::memcpy(result.data() + offset, &msg_id, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Sender node_id (length-prefixed string)
    uint32_t sender_node_len = static_cast<uint32_t>(sender.node_id.size());
    std::memcpy(result.data() + offset, &sender_node_len, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (sender_node_len > 0) {
        std::memcpy(result.data() + offset, sender.node_id.data(), sender_node_len);
        offset += sender_node_len;
    }

    // Sender actor_id
    uint64_t sender_id_val = sender.id.value();
    std::memcpy(result.data() + offset, &sender_id_val, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Sender incarnation
    std::memcpy(result.data() + offset, &sender.incarnation, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Receiver node_id (length-prefixed string)
    uint32_t receiver_node_len = static_cast<uint32_t>(receiver.node_id.size());
    std::memcpy(result.data() + offset, &receiver_node_len, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (receiver_node_len > 0) {
        std::memcpy(result.data() + offset, receiver.node_id.data(), receiver_node_len);
        offset += receiver_node_len;
    }

    // Receiver actor_id
    uint64_t receiver_id_val = receiver.id.value();
    std::memcpy(result.data() + offset, &receiver_id_val, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Receiver incarnation
    std::memcpy(result.data() + offset, &receiver.incarnation, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Payload
    if (!payload.empty()) {
        std::memcpy(result.data() + offset, payload.data(), payload.size());
    }

    return result;
}

Frame Frame::decode(const bytes& data) {
    Frame frame;
    size_t offset = 0;

    // Skip payload length (not needed for parsing)
    offset += sizeof(uint32_t);

    // Type tag (placeholder - would be used for dispatch)
    offset += sizeof(uint32_t);

    // Flags
    std::memcpy(&frame.flags, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // Message ID
    std::memcpy(&frame.message_id, data.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Sender node_id (length-prefixed string)
    uint32_t sender_node_len;
    std::memcpy(&sender_node_len, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (sender_node_len > 0) {
        frame.sender.node_id.resize(sender_node_len);
        std::memcpy(frame.sender.node_id.data(), data.data() + offset, sender_node_len);
        offset += sender_node_len;
    }

    uint64_t sender_id_val;
    std::memcpy(&sender_id_val, data.data() + offset, sizeof(uint64_t));
    frame.sender.id = ActorId(sender_id_val);
    offset += sizeof(uint64_t);

    std::memcpy(&frame.sender.incarnation, data.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Receiver node_id (length-prefixed string)
    uint32_t receiver_node_len;
    std::memcpy(&receiver_node_len, data.data() + offset, sizeof(uint32_t));
    offset += sizeof(uint32_t);
    if (receiver_node_len > 0) {
        frame.receiver.node_id.resize(receiver_node_len);
        std::memcpy(frame.receiver.node_id.data(), data.data() + offset, receiver_node_len);
        offset += receiver_node_len;
    }

    uint64_t receiver_id_val;
    std::memcpy(&receiver_id_val, data.data() + offset, sizeof(uint64_t));
    frame.receiver.id = ActorId(receiver_id_val);
    offset += sizeof(uint64_t);

    std::memcpy(&frame.receiver.incarnation, data.data() + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    // Payload - remaining data
    if (offset < data.size()) {
        auto it = data.begin() + static_cast<long>(offset);
        frame.payload.insert(frame.payload.end(), it, data.end());
    }

    return frame;
}

} // namespace net
} // namespace hpactor
