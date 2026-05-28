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

#include <hpactor/fault/fault_schedule.hpp>

namespace hpactor::fault {

void FaultSchedule::add_entry(FaultScheduleEntry entry) {
    entries_.push_back(std::move(entry));
}

void FaultSchedule::clear() {
    entries_.clear();
}

FaultSchedule::Builder& FaultSchedule::Builder::fail(std::string_view path,
                                                       int32_t error_code) {
    FaultScheduleEntry entry{domain_, at_tick_, std::string(path),
                              FaultAction::kFail, target_,
                              FailPayload{error_code}};
    schedule_->add_entry(std::move(entry));
    return *this;
}

FaultSchedule::Builder& FaultSchedule::Builder::drop(std::string_view path) {
    FaultScheduleEntry entry{domain_, at_tick_, std::string(path),
                              FaultAction::kDrop, target_,
                              std::monostate{}};
    schedule_->add_entry(std::move(entry));
    return *this;
}

FaultSchedule::Builder& FaultSchedule::Builder::delay(std::string_view path,
                                                        uint64_t ticks) {
    FaultScheduleEntry entry{domain_, at_tick_, std::string(path),
                              FaultAction::kDelay, target_,
                              DelayPayload{ticks}};
    schedule_->add_entry(std::move(entry));
    return *this;
}

FaultSchedule::Builder& FaultSchedule::Builder::corrupt(
    std::string_view path, uint64_t byte_offset, uint8_t byte_mask) {
    FaultScheduleEntry entry{domain_, at_tick_, std::string(path),
                              FaultAction::kCorrupt, target_,
                              CorruptPayload{byte_offset, byte_mask}};
    schedule_->add_entry(std::move(entry));
    return *this;
}

FaultSchedule::Builder& FaultSchedule::Builder::panic(std::string_view path) {
    FaultScheduleEntry entry{domain_, at_tick_, std::string(path),
                              FaultAction::kPanic, target_,
                              std::monostate{}};
    schedule_->add_entry(std::move(entry));
    return *this;
}

FaultSchedule::Builder& FaultSchedule::Builder::target(ActorId actor) {
    target_ = actor;
    return *this;
}

} // namespace hpactor::fault
