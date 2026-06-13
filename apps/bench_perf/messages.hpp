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

#include <chrono>
#include <cstring>

namespace hpactor::apps::bench_perf {

// =============================================================================
// Message Type Tags — application range (0x00010100 – 0x000101FF)
// =============================================================================

inline constexpr TypeTag LatencySampleTag{0x00010100};
inline constexpr TypeTag ThroughputTickTag{0x00010101};
inline constexpr TypeTag BenchStartTag{0x00010102};
inline constexpr TypeTag BenchStopTag{0x00010103};
inline constexpr TypeTag StatsPollTag{0x00010104};
inline constexpr TypeTag StatsReplyTag{0x00010105};

// =============================================================================
// Payload helpers
// =============================================================================

inline StreamBuffer encode_u64(uint64_t v) {
    StreamBuffer buf(sizeof(v));
    std::memcpy(buf.data(), &v, sizeof(v));
    return buf;
}

inline uint64_t decode_u64(const StreamBuffer& buf) {
    if (buf.size() < sizeof(uint64_t))
        return 0;
    uint64_t v = 0;
    std::memcpy(&v, buf.data(), sizeof(v));
    return v;
}

inline TypedMessage make_msg(TypeTag tag, StreamBuffer payload = {}) {
    return TypedMessage(tag, std::move(payload));
}

// =============================================================================
// CPU Burn helper (portable, cooperative)
// =============================================================================

/// \brief Burn CPU for approximately \p us microseconds.
///
/// Polls \c steady_clock in a tight loop. Cooperatively yields no syscalls.
/// Used by worker/hot actors to simulate processing cost.
inline void burn_cpu_us(uint64_t us) {
    if (us == 0)
        return;
    auto target = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < target) {
        // spin
    }
}

} // namespace hpactor::apps::bench_perf
