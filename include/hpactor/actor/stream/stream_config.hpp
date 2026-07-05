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

#include <cstdint>
#include <hpactor/types/duration.hpp>

namespace hpactor {

/// \brief Configuration for a streaming session.
struct StreamConfig {
    /// Initial credit window in bytes advertised by the receiver.
    uint32_t initial_window_bytes = 64 * 1024; // 64 KiB

    /// Maximum size of a single stream chunk payload (excluding framing).
    uint32_t max_chunk_bytes = 64 * 1024; // 64 KiB

    /// Maximum bytes the sender will buffer before write() returns false.
    uint32_t send_buffer_bytes = 256 * 1024; // 256 KiB

    /// Idle timeout — if no data or ack frame is received within this
    /// duration, the stream is errored with TIMEOUT.
    Duration idle_timeout = Duration::from_seconds(30);

    /// Maximum number of stream data frames that may be in flight before
    /// the sender pauses (additional cap beyond byte-window).
    uint32_t max_in_flight_frames = 256;
};

} // namespace hpactor
