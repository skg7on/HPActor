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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/types/types.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace hpactor::apps::bench_caf {

inline constexpr TypeTag ActorCreationStartTag{0x00010300};
inline constexpr TypeTag ActorCreationDoneTag{0x00010301};
inline constexpr TypeTag MailboxLoadTag{0x00010302};
inline constexpr TypeTag MailboxDoneTag{0x00010303};
inline constexpr TypeTag MixedTokenTag{0x00010304};
inline constexpr TypeTag MixedDoneTag{0x00010305};
inline constexpr TypeTag MixedCpuTaskTag{0x00010306};
inline constexpr TypeTag MixedCpuDoneTag{0x00010307};

struct BenchPayloadHeader {
    static constexpr size_t kEncodedSize = 20;

    uint32_t sender_id = 0;
    uint64_t sequence = 0;
    uint64_t timestamp_us = 0;
};

inline StreamBuffer encode_bench_payload(const BenchPayloadHeader& header,
                                         size_t requested_size, uint64_t seed) {
    size_t size = std::max(requested_size, BenchPayloadHeader::kEncodedSize);
    StreamBuffer payload(size);
    uint8_t* data = payload.data();
    size_t off = 0;
    std::memcpy(data + off, &header.sender_id, sizeof(header.sender_id));
    off += sizeof(header.sender_id);
    std::memcpy(data + off, &header.sequence, sizeof(header.sequence));
    off += sizeof(header.sequence);
    std::memcpy(data + off, &header.timestamp_us, sizeof(header.timestamp_us));
    off += sizeof(header.timestamp_us);

    uint64_t value = seed;
    for (size_t i = off; i < size; ++i) {
        value = value * 6364136223846793005ULL + 1442695040888963407ULL;
        data[i] = static_cast<uint8_t>(value >> 32);
    }
    return payload;
}

inline BenchPayloadHeader decode_bench_payload(const StreamBuffer& payload) {
    BenchPayloadHeader header;
    if (payload.size() < BenchPayloadHeader::kEncodedSize)
        return header;
    const uint8_t* data = payload.data();
    size_t off = 0;
    std::memcpy(&header.sender_id, data + off, sizeof(header.sender_id));
    off += sizeof(header.sender_id);
    std::memcpy(&header.sequence, data + off, sizeof(header.sequence));
    off += sizeof(header.sequence);
    std::memcpy(&header.timestamp_us, data + off, sizeof(header.timestamp_us));
    return header;
}

inline TypedMessage make_bench_msg(TypeTag tag, StreamBuffer payload = {}) {
    return TypedMessage(tag, std::move(payload));
}

} // namespace hpactor::apps::bench_caf
