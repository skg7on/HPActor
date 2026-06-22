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

#include <cstring>
#include <hpactor/net/reliable_ack.hpp>
#include <netinet/in.h>

namespace hpactor {
namespace net {

static constexpr size_t kAckWireSize = 14;

namespace {

// Host-to-big-endian for 64-bit values (uses builtin byte swap on
// little-endian).
inline uint64_t host_to_be64(uint64_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(val);
#else
    return val;
#endif
}

// Big-endian-to-host for 64-bit values.
inline uint64_t be64_to_host(uint64_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(val);
#else
    return val;
#endif
}

} // anonymous namespace

std::optional<StreamBuffer> encode_ack(const AckPayload& payload) {
    auto buf = StreamBuffer::with_capacity(kAckWireSize);
    buf.resize(kAckWireSize);
    auto* p = buf.data();
    uint64_t mid_be = host_to_be64(payload.message_id.value());
    uint32_t retry_be = htonl(static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(payload.retry_after)
            .count()));
    std::memcpy(p, &mid_be, 8);
    p[8] = static_cast<uint8_t>(payload.status);
    p[9] = 0;
    std::memcpy(p + 10, &retry_be, 4);
    return buf;
}

std::optional<AckPayload> decode_ack(const uint8_t* data, size_t len) {
    if (!data || len < kAckWireSize)
        return std::nullopt;
    AckPayload result;
    uint64_t mid_be;
    uint32_t retry_be;
    std::memcpy(&mid_be, data, 8);
    result.message_id = MessageId{be64_to_host(mid_be)};
    result.status = static_cast<AckStatus>(data[8]);
    std::memcpy(&retry_be, data + 10, 4);
    result.retry_after = std::chrono::milliseconds(ntohl(retry_be));
    return result;
}

} // namespace net
} // namespace hpactor
