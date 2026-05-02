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

#include <arpa/inet.h>
#include <cstring>

namespace hpactor {

namespace net {

StreamBuffer WireFrame::encode() const {
    // Serialize to protobuf payload
    StreamBuffer proto_payload = frame_to_proto(*this);

    // Build framed message: magic + length + protobuf payload
    StreamBuffer result;
    result.reserve(HeaderSize + proto_payload.size());

    // Magic "HPAC"
    const uint8_t magic[4] = {'H', 'P', 'A', 'C'};
    result.append(magic, 4);

    // Remaining length in network byte order
    uint32_t payload_len = static_cast<uint32_t>(proto_payload.size());
    uint32_t net_len = htonl(payload_len);
    result.append(reinterpret_cast<const uint8_t*>(&net_len), 4);

    result.append(proto_payload.data(), proto_payload.size());
    return result;
}

WireFrame WireFrame::decode(const StreamBuffer& data) {
    if (data.size() < HeaderSize) {
        return WireFrame{};
    }

    // Validate magic header
    const uint8_t expected_magic[4] = {'H', 'P', 'A', 'C'};
    if (std::memcmp(data.data(), expected_magic, 4) != 0) {
        return WireFrame{};
    }

    // Read remaining length (network byte order)
    uint32_t net_len;
    std::memcpy(&net_len, data.data() + 4, 4);
    uint32_t payload_len = ntohl(net_len);

    if (data.size() < HeaderSize + payload_len) {
        return WireFrame{};
    }

    // Extract protobuf payload and decode
    StreamBuffer proto_payload(data.begin() + HeaderSize,
                               data.begin() + HeaderSize + payload_len);
    return frame_from_proto(proto_payload);
}

WireFrame WireFrame::decode(std::span<const uint8_t> data) {
    return decode(StreamBuffer(data.begin(), data.end()));
}

} // namespace net
} // namespace hpactor
