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

#include <hpactor/msg/frame.hpp>

#include <hpactor/log/logger.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cstring>

namespace hpactor {

namespace net {

StreamBuffer WireFrame::encode() const {
    std::string serialized = pb_envelope.SerializeAsString();

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

FrameDecodeResult
try_decode_wireframe(const StreamBuffer& data, FrameDecodeLimits limits) {
    FrameDecodeResult result{};

    // 1. Validate minimum header size
    if (data.size() < WireFrame::HeaderSize) {
        result.error = FrameDecodeError::HeaderTooShort;
        return result;
    }

    // 2. Validate magic "HPAC"
    const std::array<uint8_t, 4> expected_magic = {'H', 'P', 'A', 'C'};
    if (std::memcmp(data.data(), expected_magic.data(), 4) != 0) {
        result.error = FrameDecodeError::InvalidMagic;
        return result;
    }

    // 3. Read payload length (network byte order → host)
    uint32_t net_len = 0;
    std::memcpy(&net_len, data.data() + 4, 4);
    uint32_t payload_len = ntohl(net_len);
    result.declared_payload_bytes = payload_len;

    // 4. Enforce configured payload-size bound before allocating/copying
    if (limits.max_payload_bytes > 0 && payload_len > limits.max_payload_bytes) {
        result.error = FrameDecodeError::FrameTooLarge;
        HPACTOR_LOG_WARNING(
            log::LogCategory::kNetwork, ActorId{0},
            static_cast<uint32_t>(log::LogEventId::kNetworkFrameDecodeFailed),
            "frame too large",
            log::field("declared_bytes", static_cast<uint64_t>(payload_len)),
            log::field("max_bytes",
                       static_cast<uint64_t>(limits.max_payload_bytes)));
        return result;
    }

    // 5. Check length matches available data
    size_t expected_size = WireFrame::HeaderSize + payload_len;
    if (data.size() < expected_size) {
        result.error = FrameDecodeError::LengthMismatch;
        return result;
    }
    if (limits.reject_trailing_bytes && data.size() > expected_size) {
        result.error = FrameDecodeError::TrailingBytes;
        return result;
    }

    // 6. Parse protobuf from the bounded payload span
    WireFrame frame;
    std::string serialized(data.begin() + WireFrame::HeaderSize,
                           data.begin() + expected_size);
    if (!frame.pb_envelope.ParseFromString(serialized)) {
        result.error = FrameDecodeError::InvalidProtobuf;
        HPACTOR_LOG_ERROR(
            log::LogCategory::kNetwork, ActorId{0},
            static_cast<uint32_t>(log::LogEventId::kNetworkFrameDecodeFailed),
            "protobuf parse failure");
        return result;
    }

    result.frame = std::move(frame);
    result.error = FrameDecodeError::None;

    HPACTOR_LOG_TRACE(
        log::LogCategory::kNetwork, ActorId{0},
        static_cast<uint32_t>(log::LogEventId::kNetworkFrameReceived),
        "network frame received",
        log::field("bytes", static_cast<uint64_t>(data.size())),
        log::field("payload_type", static_cast<uint64_t>(static_cast<int>(
                                       result.frame.payload_type()))));
    return result;
}

WireFrame WireFrame::decode(const StreamBuffer& data) {
    auto decoded = try_decode_wireframe(data);
    return decoded.ok() ? std::move(decoded.frame) : WireFrame{};
}

WireFrame WireFrame::decode(std::span<const uint8_t> data) {
    return decode(StreamBuffer(data.begin(), data.end()));
}

// ── Handshake message encode/decode
// ────────────────────────────────────────────

namespace {

template <typename PbMessage>
StreamBuffer encode_handshake_message(const PbMessage& msg) {
    std::string serialized = msg.SerializeAsString();
    if (serialized.empty() && msg.ByteSizeLong() > 0) {
        return {}; // Serialization failure
    }

    StreamBuffer result;
    result.reserve(HandshakeHeaderSize + serialized.size());

    // Magic "HPAH"
    const std::array<uint8_t, 4> magic = {'H', 'P', 'A', 'H'};
    result.append(magic.data(), 4);

    // Remaining length in network byte order
    uint32_t payload_len = static_cast<uint32_t>(serialized.size());
    uint32_t net_len = htonl(payload_len);
    result.append(reinterpret_cast<const uint8_t*>(&net_len), 4);

    result.append(reinterpret_cast<const uint8_t*>(serialized.data()),
                  serialized.size());
    return result;
}

template <typename PbMessage>
std::optional<PbMessage> decode_handshake_message(const StreamBuffer& data) {
    if (data.size() < HandshakeHeaderSize) {
        return std::nullopt;
    }

    // Validate magic "HPAH"
    const std::array<uint8_t, 4> expected_magic = {'H', 'P', 'A', 'H'};
    if (std::memcmp(data.data(), expected_magic.data(), 4) != 0) {
        return std::nullopt;
    }

    // Read payload length
    uint32_t net_len = 0;
    std::memcpy(&net_len, data.data() + 4, 4);
    uint32_t payload_len = ntohl(net_len);

    size_t expected_size = HandshakeHeaderSize + payload_len;
    if (data.size() < expected_size) {
        return std::nullopt;
    }

    // Parse protobuf
    PbMessage msg;
    std::string serialized(data.begin() + HandshakeHeaderSize,
                           data.begin() + expected_size);
    if (!msg.ParseFromString(serialized)) {
        return std::nullopt;
    }

    return msg;
}

} // namespace

StreamBuffer encode_handshake_hello(const ::hpactor::net::HandshakeHello& hello) {
    return encode_handshake_message(hello);
}

std::optional<::hpactor::net::HandshakeHello>
decode_handshake_hello(const StreamBuffer& data) {
    return decode_handshake_message<::hpactor::net::HandshakeHello>(data);
}

StreamBuffer
encode_handshake_response(const ::hpactor::net::HandshakeResponse& resp) {
    return encode_handshake_message(resp);
}

std::optional<::hpactor::net::HandshakeResponse>
decode_handshake_response(const StreamBuffer& data) {
    return decode_handshake_message<::hpactor::net::HandshakeResponse>(data);
}

uint32_t negotiate_version(uint32_t client_min, uint32_t client_max,
                           uint32_t server_min, uint32_t server_max) {
    if (server_min > client_max || client_min > server_max) {
        return 0; // Disjoint ranges — incompatible
    }
    return std::min(server_max, client_max);
}

} // namespace net
} // namespace hpactor
