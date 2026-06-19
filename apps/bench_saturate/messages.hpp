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
#include <chrono>
#include <cstring>

namespace hpactor::apps::bench_saturate {

// =============================================================================
// Message Type Tags — application range (0x00010200 – 0x000102FF)
// =============================================================================

inline constexpr TypeTag SaturateStartTag{0x00010200};
inline constexpr TypeTag SaturateStopTag{0x00010201};
inline constexpr TypeTag RateChangeTag{0x00010202};
inline constexpr TypeTag StatsPollTag{0x00010203};
inline constexpr TypeTag StatsReplyTag{0x00010204};
inline constexpr TypeTag ThroughputSampleTag{0x00010205};
inline constexpr TypeTag DropReportTag{0x00010206};
inline constexpr TypeTag LatencySampleTag{0x00010207};
inline constexpr TypeTag LoadMessageTag{0x00010208};

// =============================================================================
// Payload mode enum
// =============================================================================

enum class PayloadMode : uint8_t {
    Small = 0, // header-only (20 bytes)
    Junk = 1,  // header + random fill to [min, max] size
    Mixed = 2  // 80% small, 20% junk
};

// =============================================================================
// SaturateStart payload encoding
// =============================================================================

struct SaturateStartPayload {
    uint32_t num_senders = 100;
    uint32_t num_receivers = 10;
    PayloadMode payload_mode = PayloadMode::Small;
    uint16_t payload_size_min = 16;
    uint16_t payload_size_max = 16;
    uint32_t initial_rate_msgps = 100;
    uint16_t step_interval_ms = 1000;
    float drop_threshold_pct = 1.0f;
    uint8_t refine_iterations = 5;
    uint32_t mailbox_capacity = 4096;
    uint32_t stable_duration_ms = 5000;
    uint32_t duration_max_ms = 120000;

    StreamBuffer encode() const {
        uint8_t buf[40];
        std::memset(buf, 0, sizeof(buf));
        size_t off = 0;
        std::memcpy(buf + off, &num_senders, 4);
        off += 4;
        std::memcpy(buf + off, &num_receivers, 4);
        off += 4;
        uint8_t mode = static_cast<uint8_t>(payload_mode);
        std::memcpy(buf + off, &mode, 1);
        off += 1;
        std::memcpy(buf + off, &payload_size_min, 2);
        off += 2;
        std::memcpy(buf + off, &payload_size_max, 2);
        off += 2;
        std::memcpy(buf + off, &initial_rate_msgps, 4);
        off += 4;
        std::memcpy(buf + off, &step_interval_ms, 2);
        off += 2;
        std::memcpy(buf + off, &drop_threshold_pct, 4);
        off += 4;
        std::memcpy(buf + off, &refine_iterations, 1);
        off += 1;
        std::memcpy(buf + off, &mailbox_capacity, 4);
        off += 4;
        std::memcpy(buf + off, &stable_duration_ms, 4);
        off += 4;
        std::memcpy(buf + off, &duration_max_ms, 4);
        off += 4;
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    static SaturateStartPayload decode(const StreamBuffer& buf) {
        SaturateStartPayload p;
        if (buf.size() < 40)
            return p;
        const uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(&p.num_senders, d + off, 4);
        off += 4;
        std::memcpy(&p.num_receivers, d + off, 4);
        off += 4;
        uint8_t mode = 0;
        std::memcpy(&mode, d + off, 1);
        off += 1;
        p.payload_mode = static_cast<PayloadMode>(mode);
        std::memcpy(&p.payload_size_min, d + off, 2);
        off += 2;
        std::memcpy(&p.payload_size_max, d + off, 2);
        off += 2;
        std::memcpy(&p.initial_rate_msgps, d + off, 4);
        off += 4;
        std::memcpy(&p.step_interval_ms, d + off, 2);
        off += 2;
        std::memcpy(&p.drop_threshold_pct, d + off, 4);
        off += 4;
        std::memcpy(&p.refine_iterations, d + off, 1);
        off += 1;
        std::memcpy(&p.mailbox_capacity, d + off, 4);
        off += 4;
        std::memcpy(&p.stable_duration_ms, d + off, 4);
        off += 4;
        std::memcpy(&p.duration_max_ms, d + off, 4);
        off += 4;
        return p;
    }
};

// =============================================================================
// RateChange payload encoding
// =============================================================================

struct RateChangePayload {
    uint32_t target_rate_msgps = 100;
    PayloadMode payload_mode = PayloadMode::Small;
    uint16_t payload_size_min = 16;
    uint16_t payload_size_max = 16;
    uint16_t step_interval_ms = 1000;

    StreamBuffer encode() const {
        uint8_t buf[12];
        std::memset(buf, 0, sizeof(buf));
        size_t off = 0;
        std::memcpy(buf + off, &target_rate_msgps, 4);
        off += 4;
        uint8_t mode = static_cast<uint8_t>(payload_mode);
        std::memcpy(buf + off, &mode, 1);
        off += 1;
        std::memcpy(buf + off, &payload_size_min, 2);
        off += 2;
        std::memcpy(buf + off, &payload_size_max, 2);
        off += 2;
        std::memcpy(buf + off, &step_interval_ms, 2);
        off += 2;
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    static RateChangePayload decode(const StreamBuffer& buf) {
        RateChangePayload p;
        if (buf.size() < 12)
            return p;
        const uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(&p.target_rate_msgps, d + off, 4);
        off += 4;
        uint8_t mode = 0;
        std::memcpy(&mode, d + off, 1);
        off += 1;
        p.payload_mode = static_cast<PayloadMode>(mode);
        std::memcpy(&p.payload_size_min, d + off, 2);
        off += 2;
        std::memcpy(&p.payload_size_max, d + off, 2);
        off += 2;
        std::memcpy(&p.step_interval_ms, d + off, 2);
        off += 2;
        return p;
    }
};

// =============================================================================
// LoadMessage payload encoding (sent from sender to receiver)
// =============================================================================

struct LoadMessagePayload {
    static constexpr size_t kHeaderSize = 20; // sender_id(4) + seq_no(8) +
                                              // timestamp(8)

    uint32_t sender_id = 0;
    uint64_t seq_no = 0;
    uint64_t send_timestamp_us = 0;

    /// \brief Encode header-only (small mode, 20 bytes).
    StreamBuffer encode_header() const {
        uint8_t buf[kHeaderSize];
        size_t off = 0;
        std::memcpy(buf + off, &sender_id, 4);
        off += 4;
        std::memcpy(buf + off, &seq_no, 8);
        off += 8;
        std::memcpy(buf + off, &send_timestamp_us, 8);
        off += 8;
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    /// \brief Encode header + junk fill to \p total_size bytes.
    StreamBuffer encode_with_junk(size_t total_size, uint64_t random_seed) const {
        total_size = std::max(total_size, kHeaderSize);
        StreamBuffer buf(total_size);
        uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(d + off, &sender_id, 4);
        off += 4;
        std::memcpy(d + off, &seq_no, 8);
        off += 8;
        std::memcpy(d + off, &send_timestamp_us, 8);
        off += 8;
        // Fill junk with pseudo-random bytes derived from seed + offset
        uint64_t seed = random_seed;
        for (size_t i = kHeaderSize; i < total_size; ++i) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            d[i] = static_cast<uint8_t>(seed >> 32);
        }
        return buf;
    }

    static LoadMessagePayload decode(const StreamBuffer& buf) {
        LoadMessagePayload p;
        if (buf.size() < kHeaderSize)
            return p;
        const uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(&p.sender_id, d + off, 4);
        off += 4;
        std::memcpy(&p.seq_no, d + off, 8);
        off += 8;
        std::memcpy(&p.send_timestamp_us, d + off, 8);
        off += 8;
        return p;
    }
};

// =============================================================================
// LatencySample payload encoding
// =============================================================================

struct LatencySamplePayload {
    uint32_t sender_id = 0;
    uint64_t seq_no = 0;
    uint32_t latency_us = 0;

    StreamBuffer encode() const {
        uint8_t buf[16];
        size_t off = 0;
        std::memcpy(buf + off, &sender_id, 4);
        off += 4;
        std::memcpy(buf + off, &seq_no, 8);
        off += 8;
        std::memcpy(buf + off, &latency_us, 4);
        off += 4;
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    static LatencySamplePayload decode(const StreamBuffer& buf) {
        LatencySamplePayload p;
        if (buf.size() < 16)
            return p;
        const uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(&p.sender_id, d + off, 4);
        off += 4;
        std::memcpy(&p.seq_no, d + off, 8);
        off += 8;
        std::memcpy(&p.latency_us, d + off, 4);
        off += 4;
        return p;
    }
};

// =============================================================================
// DropReport payload encoding
// =============================================================================

struct DropReportPayload {
    uint64_t receiver_id = 0;
    uint64_t total_received = 0;
    uint64_t total_dropped = 0;

    StreamBuffer encode() const {
        uint8_t buf[24];
        size_t off = 0;
        std::memcpy(buf + off, &receiver_id, 8);
        off += 8;
        std::memcpy(buf + off, &total_received, 8);
        off += 8;
        std::memcpy(buf + off, &total_dropped, 8);
        off += 8;
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    static DropReportPayload decode(const StreamBuffer& buf) {
        DropReportPayload p;
        if (buf.size() < 24)
            return p;
        const uint8_t* d = buf.data();
        size_t off = 0;
        std::memcpy(&p.receiver_id, d + off, 8);
        off += 8;
        std::memcpy(&p.total_received, d + off, 8);
        off += 8;
        std::memcpy(&p.total_dropped, d + off, 8);
        off += 8;
        return p;
    }
};

// =============================================================================
// ThroughputSample payload encoding
// =============================================================================

struct ThroughputSamplePayload {
    uint32_t sender_id = 0;
    uint64_t total_sent = 0;

    StreamBuffer encode() const {
        uint8_t buf[12];
        std::memcpy(buf, &sender_id, 4);
        std::memcpy(buf + 4, &total_sent, 8);
        return StreamBuffer(buf, buf + sizeof(buf));
    }

    static ThroughputSamplePayload decode(const StreamBuffer& buf) {
        ThroughputSamplePayload p;
        if (buf.size() < 12)
            return p;
        const uint8_t* d = buf.data();
        std::memcpy(&p.sender_id, d, 4);
        std::memcpy(&p.total_sent, d + 4, 8);
        return p;
    }
};

// =============================================================================
// Convenience: wrap a StreamBuffer in a TypedMessage
// =============================================================================

inline TypedMessage make_msg(TypeTag tag, StreamBuffer payload = {}) {
    return TypedMessage(tag, std::move(payload));
}

// =============================================================================
// CPU Burn helper (portable, cooperative)
// =============================================================================

/// \brief Burn CPU for approximately \p us microseconds.
///
/// Polls \c steady_clock in a tight loop. No syscalls — cooperatively yields
/// to the scheduler. Used by senders to simulate processing cost if needed.
inline void burn_cpu_us(uint64_t us) {
    if (us == 0)
        return;
    auto target = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < target) {
        // spin
    }
}

// =============================================================================
// Payload size generator for junk/mixed modes
// =============================================================================

/// \brief Generate a random payload size in [min, max] using a simple LCG.
inline size_t
random_payload_size(uint16_t min_size, uint16_t max_size, uint64_t& seed) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    uint16_t range = static_cast<uint16_t>(max_size - min_size + 1);
    return min_size + static_cast<size_t>((seed >> 32) % range);
}

} // namespace hpactor::apps::bench_saturate
