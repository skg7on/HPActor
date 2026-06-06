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

namespace hpactor {

// -----------------------------------------------------------------------------
// TypeTag - type identifier for serialization (replaces RTTI)
//
// Range layout:
//   0x00000000 – 0x000000FF   System messages (256 slots)
//   0x00000100 – 0x00000FFF   Reserved for future system expansion
//   0x00001000 – 0x00FFFFFF   Application-defined messages (~16M slots)
//
// System sub-ranges:
//   0x00 – 0x0F   Core system (lifecycle: Down, Exit, Link, Unlink, Monitor,
//   Demonitor) 0x10 – 0x1F   Spawn protocol (SpawnRequest, SpawnResponse,
//   Error) 0x20 – 0x2F   HTTP protocol (HttpRequest, HttpResponse) 0x30 – 0x3F
//   TOML bootstrap (SystemInit) 0x40 – 0x4F   Metrics (MetricsRequest,
//   MetricsResponse) 0x50 – 0x5F   CLI interactive (Inspect, Kill, List, Stats)
//   0x60 – 0x6F   Async I/O (IoCompletion)
//   0x70 – 0xFF   Reserved for future system use
//
// Application sub-ranges (examples):
//   0x00001000 – 0x00001FFF   Auth subsystem
//   0x00002000 – 0x00002FFF   Chat subsystem
//   0x00003000 – 0x00003FFF   Database subsystem
//   ...
// -----------------------------------------------------------------------------
enum class TypeTag : uint32_t {
    // ---- System message range
    // ------------------------------------------------
    Invalid = 0x00000000,

    // Core system (0x00 – 0x0F)
    DownMsg = 0x01,
    ExitMsg = 0x02,
    LinkMsg = 0x03,
    UnlinkMsg = 0x04,
    MonitorMsg = 0x0A,
    DemonitorMsg = 0x0B,

    // Spawn protocol (0x10 – 0x1F)
    SpawnRequestTag = 0x10,
    SpawnResponseTag = 0x11,
    ErrorMsg = 0x12,

    // HTTP protocol (0x20 – 0x2F)
    HttpRequestTag = 0x20,
    HttpResponseTag = 0x21,

    // TOML config bootstrapping (0x30 – 0x3F)
    SystemInitTag = 0x30,

    // Metrics subsystem (0x40 – 0x4F)
    MetricsRequestTag = 0x40,
    MetricsResponseTag = 0x41,

    // CLI interactive subsystem (0x50 – 0x5F)
    InspectStateRequestTag = 0x50,
    InspectStateResponseTag = 0x51,
    KillRequestTag = 0x52,
    KillResponseTag = 0x53,
    ListActorsRequestTag = 0x54,
    ListActorsResponseTag = 0x55,
    SystemStatsRequestTag = 0x56,
    SystemStatsResponseTag = 0x57,
    MemoryStatsRequestTag = 0x58,
    MemoryStatsResponseTag = 0x59,
    TopologyShowRequestTag = 0x5A,
    TopologyShowResponseTag = 0x5B,
    TopologyRestartRequestTag = 0x5C,
    TopologyRestartResponseTag = 0x5D,
    QuarantineRequestTag = 0x5E,
    QuarantineResponseTag = 0x5F,

    // Async I/O (0x60 – 0x6F)
    IoCompletionTag = 0x60,

    // Backpressure control (0x70 – 0x7F)
    BackpressureSignalTag = 0x70,

    // ---- Application range
    // ---------------------------------------------------
    User = 0x00001000,
};

} // namespace hpactor
