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
#include <string_view>

namespace hpactor::fault {

enum class FaultDomain : uint8_t {
    kMailbox = 0,
    kTransport = 1,
    kScheduler = 2,
    kAllocator = 3,
    kStorage = 4,
    kTimer = 5,
    kGossip = 6,
    kConfig = 7,
    kActor = 8,
    kRpc = 9,
    kSupervision = 10,
    kDiscovery = 11,
    kTracing = 12,
    kMetrics = 13,
};

constexpr std::string_view to_string(FaultDomain d) noexcept {
    switch (d) {
    case FaultDomain::kMailbox:     return "kMailbox";
    case FaultDomain::kTransport:   return "kTransport";
    case FaultDomain::kScheduler:   return "kScheduler";
    case FaultDomain::kAllocator:   return "kAllocator";
    case FaultDomain::kStorage:     return "kStorage";
    case FaultDomain::kTimer:       return "kTimer";
    case FaultDomain::kGossip:      return "kGossip";
    case FaultDomain::kConfig:      return "kConfig";
    case FaultDomain::kActor:       return "kActor";
    case FaultDomain::kRpc:         return "kRpc";
    case FaultDomain::kSupervision: return "kSupervision";
    case FaultDomain::kDiscovery:   return "kDiscovery";
    case FaultDomain::kTracing:     return "kTracing";
    case FaultDomain::kMetrics:     return "kMetrics";
    }
    return "kUnknown";
}

enum class FaultAction : uint8_t {
    kFail = 0,
    kDrop = 1,
    kDelay = 2,
    kCorrupt = 3,
    kPanic = 4,
};

constexpr std::string_view to_string(FaultAction a) noexcept {
    switch (a) {
    case FaultAction::kFail:    return "Fail";
    case FaultAction::kDrop:    return "Drop";
    case FaultAction::kDelay:   return "Delay";
    case FaultAction::kCorrupt: return "Corrupt";
    case FaultAction::kPanic:   return "Panic";
    }
    return "Unknown";
}

} // namespace hpactor::fault
