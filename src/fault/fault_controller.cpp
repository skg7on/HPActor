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

#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/fault/fault_point.hpp>
#include <hpactor/platform.hpp>

#include <cassert>
#include <cstdlib>

namespace hpactor::fault {

FaultController* FaultController::instance_ = nullptr;

FaultController::FaultController()
    : enabled_(false)
    , active_scope_("*")
    , schedule_cursor_(0)
    , replay_seed_(0)
    , faults_fired_(0) {
    for (auto& tick : domain_ticks_) tick = 0;
}

void FaultController::load(const FaultSchedule& schedule) {
    schedule_ = schedule;
    schedule_cursor_ = 0;
    faults_fired_ = 0;
}

void FaultController::clear() {
    schedule_.clear();
    schedule_cursor_ = 0;
    faults_fired_ = 0;
}

void FaultController::enable(std::string_view scope_pattern) {
    enabled_ = true;
    active_scope_ = std::string(scope_pattern);
}

void FaultController::disable(std::string_view /*scope_pattern*/) {
    enabled_ = false;
}

void FaultController::advance_tick(FaultDomain domain) {
    auto idx = static_cast<size_t>(domain);
    assert(idx < 9);
    domain_ticks_[idx]++;
}

bool FaultController::check(std::string_view path,
                             std::optional<ActorId> target) {
    if (HPACTOR_UNLIKELY(!enabled_)) return false;

    auto& registry = FaultPointRegistry::instance();
    if (!registry.matches_prefix(path, active_scope_)) return false;

    const auto* fault_point = registry.lookup(path);
    FaultDomain domain = fault_point ? fault_point->domain : FaultDomain::kMailbox;

    auto idx = static_cast<size_t>(domain);
    assert(idx < 9);
    domain_ticks_[idx]++;

    uint64_t current_tick = domain_ticks_[idx];

    for (size_t i = schedule_cursor_; i < schedule_.entries().size(); ++i) {
        const auto& entry = schedule_.entries()[i];
        if (entry.domain != domain) continue;
        if (entry.at_tick > current_tick) break;

        if (entry.at_tick == current_tick && entry.path == path) {
            if (entry.target.has_value() && target.has_value() &&
                entry.target.value() != target.value()) {
                continue;
            }
            schedule_cursor_ = i + 1;
            faults_fired_++;

            if (entry.action == FaultAction::kPanic) {
                std::abort();
            }

            return true;
        }
    }

    return false;
}

void FaultController::stall(FaultDomain domain, uint64_t delay_ticks) {
    auto idx = static_cast<size_t>(domain);
    for (uint64_t i = 0; i < delay_ticks; ++i) {
        domain_ticks_[idx]++;
    }
}

FaultControllerSnapshot FaultController::snapshot() const {
    FaultControllerSnapshot snap{};
    snap.enabled = enabled_;
    snap.active_scope = active_scope_;
    snap.replay_seed = replay_seed_;
    snap.schedule_entry_count = schedule_.size();
    for (size_t i = 0; i < 9; ++i) {
        snap.domain_ticks[i] = domain_ticks_[i];
    }
    snap.faults_fired = faults_fired_;
    return snap;
}

void FaultController::install() {
    instance_ = this;
}

void FaultController::remove() {
    instance_ = nullptr;
}

} // namespace hpactor::fault
