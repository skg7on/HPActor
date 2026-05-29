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

#include <hpactor/fault/fault_types.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hpactor::fault {

struct DelayPayload { uint64_t ticks; };
struct CorruptPayload { uint64_t byte_offset; uint8_t byte_mask; };
struct FailPayload { int32_t error_code; };

using FaultPayload = std::variant<std::monostate, FailPayload,
                                   DelayPayload, CorruptPayload>;

struct FaultScheduleEntry {
    FaultDomain domain;
    uint64_t at_tick;
    std::string path;
    FaultAction action;
    std::optional<ActorId> target;
    FaultPayload payload;
};

class FaultSchedule {
  public:
    class Builder;

    FaultSchedule() = default;

    void add_entry(FaultScheduleEntry entry);
    void clear();

    const std::vector<FaultScheduleEntry>& entries() const noexcept {
        return entries_;
    }
    bool empty() const noexcept { return entries_.empty(); }
    size_t size() const noexcept { return entries_.size(); }

  private:
    std::vector<FaultScheduleEntry> entries_;
};

class FaultSchedule::Builder {
  public:
    explicit Builder(FaultSchedule& schedule, FaultDomain domain,
                     uint64_t at_tick)
        : schedule_(&schedule), domain_(domain), at_tick_(at_tick) {}

    Builder& fail(std::string_view path, int32_t error_code);
    Builder& drop(std::string_view path);
    Builder& delay(std::string_view path, uint64_t ticks);
    Builder& corrupt(std::string_view path, uint64_t byte_offset,
                     uint8_t byte_mask);
    Builder& panic(std::string_view path);
    Builder& target(ActorId actor);

  private:
    FaultSchedule* schedule_;
    FaultDomain domain_;
    uint64_t at_tick_;
    std::optional<ActorId> target_;
};

inline FaultSchedule::Builder add_entry_to(FaultSchedule& schedule,
                                            FaultDomain domain,
                                            uint64_t at_tick) {
    return FaultSchedule::Builder(schedule, domain, at_tick);
}

} // namespace hpactor::fault
