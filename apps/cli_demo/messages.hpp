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

#include <cstring>

namespace hpactor::apps::cli_demo {

// =============================================================================
// Message Type Tags — application range (0x00010000 – 0x000100FF)
// =============================================================================

inline constexpr TypeTag WorkerResultTag{0x00010000};
inline constexpr TypeTag HealthPingTag{0x00010001};
inline constexpr TypeTag HealthPongTag{0x00010002};
inline constexpr TypeTag BroadcastConfigTag{0x00010003};
inline constexpr TypeTag TimeQueryTag{0x00010004};
inline constexpr TypeTag TimeReplyTag{0x00010005};
inline constexpr TypeTag LogEntryTag{0x00010006};
inline constexpr TypeTag MonitorQueryTag{0x00010007};
inline constexpr TypeTag MonitorReplyTag{0x00010008};
inline constexpr TypeTag PeriodicTickTag{0x00010009};
inline constexpr TypeTag StartTag{0x0001000A};
inline constexpr TypeTag DlqGenerateTag{0x0001000B};
inline constexpr TypeTag QueryTriggerTag{0x0001000C};

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

} // namespace hpactor::apps::cli_demo
