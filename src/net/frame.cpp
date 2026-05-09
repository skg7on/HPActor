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

#include <hpactor/log/logger.hpp>

#include <arpa/inet.h>
#include <cstring>

namespace hpactor {

namespace net {

StreamBuffer WireFrame::encode() const {
    std::string serialized = pb_frame.SerializeAsString();

    StreamBuffer result;
    result.reserve(HeaderSize + serialized.size());

    // Magic "HPAC"
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'C'};
    result.append(magic.data(), 4);

    // Remaining length in network byte order
    uint32_t payload_len = static_cast<uint32_t>(serialized.size());
    uint32_t net_len = htonl(payload_len);
    result.append(reinterpret_cast<const uint8_t*>(&net_len), 4);

    result.append(reinterpret_cast<const uint8_t*>(serialized.data()),
                  serialized.size());
    return result;
}

WireFrame WireFrame::decode(const StreamBuffer& data) {
    if (data.size() < HeaderSize) {
        return WireFrame{};
    }

    // Validate magic header
    const std::array<uint8_t, 4> expected_magic = {'H', 'P', 'A', 'C'};
    if (std::memcmp(data.data(), expected_magic.data(), 4) != 0) {
        HPACTOR_LOG_ERROR(
            log::LogCategory::kNetwork, ActorId{0},
            static_cast<uint32_t>(log::LogEventId::kNetworkFrameDecodeFailed),
            "network frame decode failed");
        return WireFrame{};
    }

    // Read remaining length (network byte order)
    uint32_t net_len = 0;
    std::memcpy(&net_len, data.data() + 4, 4);
    uint32_t payload_len = ntohl(net_len);

    if (data.size() < HeaderSize + payload_len) {
        return WireFrame{};
    }

    // Parse protobuf payload directly into pb_frame
    WireFrame frame;
    std::string serialized(data.begin() + HeaderSize,
                           data.begin() + HeaderSize + payload_len);
    if (!frame.pb_frame.ParseFromString(serialized)) {
        HPACTOR_LOG_ERROR(log::LogCategory::kNetwork, ActorId{0}, 0,
                          "protobuf parse failure");
        return WireFrame{};
    }

    HPACTOR_LOG_TRACE(
        log::LogCategory::kNetwork, ActorId{0},
        static_cast<uint32_t>(log::LogEventId::kNetworkFrameReceived),
        "network frame received",
        log::field("bytes", static_cast<uint64_t>(data.size())),
        log::field("tag", static_cast<uint64_t>(frame.pb_frame.type_tag())));
    return frame;
}

WireFrame WireFrame::decode(std::span<const uint8_t> data) {
    return decode(StreamBuffer(data.begin(), data.end()));
}

} // namespace net
} // namespace hpactor
