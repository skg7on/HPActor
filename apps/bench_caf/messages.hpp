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

#include "caf_bench_config.hpp"

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

inline StreamBuffer
encode_shaped_payload(const BenchPayloadHeader& header, size_t requested_size,
                      MessageShape shape, uint64_t seed) {
    switch (shape) {
        case MessageShape::HeaderOnly:
            return encode_bench_payload(header, 0, seed);
        case MessageShape::FixedBytes:
            return encode_bench_payload(header, requested_size, seed);
        case MessageShape::ProtobufSmall: {
            size_t proto_size =
                std::max(requested_size, BenchPayloadHeader::kEncodedSize + 8);
            StreamBuffer payload(proto_size);
            uint8_t* data = payload.data();
            // Write the raw benchmark header at offset 0 so
            // decode_bench_payload can read it. The proto framing bytes follow
            // immediately after.
            size_t off = 0;
            std::memcpy(data + off, &header.sender_id, sizeof(header.sender_id));
            off += sizeof(header.sender_id);
            std::memcpy(data + off, &header.sequence, sizeof(header.sequence));
            off += sizeof(header.sequence);
            std::memcpy(data + off, &header.timestamp_us,
                        sizeof(header.timestamp_us));
            off += sizeof(header.timestamp_us);
            // Proto framing marker bytes at offset 20 (after header).
            if (off + 2 <= proto_size) {
                data[off] = 0x0A;
                data[off + 1] =
                    static_cast<uint8_t>(std::min(requested_size, size_t{255}));
                off += 2;
            }
            uint64_t value = seed;
            for (size_t i = off; i < proto_size; ++i) {
                value = value * 6364136223846793005ULL + 1442695040888963407ULL;
                data[i] = static_cast<uint8_t>(value >> 32);
            }
            return payload;
        }
        case MessageShape::ProtobufNested: {
            size_t nested_size =
                std::max(requested_size, BenchPayloadHeader::kEncodedSize + 16);
            StreamBuffer payload(nested_size);
            uint8_t* data = payload.data();
            // Write the raw benchmark header at offset 0 so
            // decode_bench_payload can read it. The nested proto framing bytes
            // follow after.
            size_t off = 0;
            std::memcpy(data + off, &header.sender_id, sizeof(header.sender_id));
            off += sizeof(header.sender_id);
            std::memcpy(data + off, &header.sequence, sizeof(header.sequence));
            off += sizeof(header.sequence);
            std::memcpy(data + off, &header.timestamp_us,
                        sizeof(header.timestamp_us));
            off += sizeof(header.timestamp_us);
            // Nested proto framing: field tag + checksum placeholder + length.
            if (off + 2 <= nested_size) {
                data[off] = 0x12;
                data[off + 1] =
                    static_cast<uint8_t>(std::min(requested_size, size_t{255}));
                off += 2;
            }
            uint64_t checksum =
                header.sender_id ^ header.sequence ^ header.timestamp_us ^ seed;
            if (off + sizeof(checksum) <= nested_size) {
                std::memcpy(data + off, &checksum, sizeof(checksum));
                off += sizeof(checksum);
            }
            uint64_t value = seed;
            for (size_t i = off; i < nested_size; ++i) {
                value = value * 6364136223846793005ULL + 1442695040888963407ULL;
                data[i] = static_cast<uint8_t>(value >> 32);
            }
            return payload;
        }
        case MessageShape::SharedBuffer: {
            return encode_bench_payload(header, requested_size, seed);
        }
        case MessageShape::Mixed80_20: {
            uint64_t mix_seed = seed + header.sequence;
            bool is_small = (mix_seed % 100) < 80;
            if (is_small)
                return encode_bench_payload(
                    header, BenchPayloadHeader::kEncodedSize, mix_seed);
            else
                return encode_bench_payload(header, requested_size, mix_seed);
        }
    }
    return encode_bench_payload(header, requested_size, seed);
}

} // namespace hpactor::apps::bench_caf
