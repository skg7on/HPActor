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
#include <hpactor/log/logger.hpp>
#include <hpactor/platform.hpp>

#include <cassert>
#include <cstdlib>
#include <algorithm>
#include <mutex>
#include <vector>

namespace hpactor::fault {

thread_local FaultController* FaultController::tls_instance_ = nullptr;

FaultController::InstanceList& FaultController::instance_list() {
    static InstanceList list;
    return list;
}

FaultController::FaultController() {
    active_scope_ = "*";
    for (auto& tick : domain_ticks_) tick = 0;
}

void FaultController::load(const FaultSchedule& schedule) {
    FaultSchedule sorted = schedule;
    sorted.sort();
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        fc->load_impl(sorted);
    }
}

void FaultController::load_impl(const FaultSchedule& schedule) {
    schedule_ = std::make_shared<const FaultSchedule>(schedule);
    schedule_cursor_ = 0;
    faults_fired_.store(0, std::memory_order_release);
}

void FaultController::clear() {
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        fc->clear_impl();
    }
}

void FaultController::clear_impl() {
    schedule_.reset();
    schedule_cursor_ = 0;
    faults_fired_.store(0, std::memory_order_release);
}

void FaultController::enable(std::string_view scope_pattern) {
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        fc->enable_impl(scope_pattern);
    }
}

void FaultController::enable_impl(std::string_view scope_pattern) {
    enabled_.store(true, std::memory_order_release);
    active_scope_ = std::string(scope_pattern);
}

void FaultController::disable() {
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        fc->disable_impl();
    }
}

void FaultController::disable_impl() {
    enabled_.store(false, std::memory_order_release);
}

void FaultController::advance_tick(FaultDomain domain) {
    auto idx = static_cast<size_t>(domain);
    assert(idx < 14);
    domain_ticks_[idx]++;
}

bool FaultController::check(std::string_view path,
                             std::optional<ActorId> target) {
    if (HPACTOR_UNLIKELY(!enabled_.load(std::memory_order_acquire))) return false;

    auto& registry = FaultPointRegistry::instance();
    if (!registry.matches_prefix(path, active_scope_)) return false;

    const auto* fault_point = registry.lookup(path);
    FaultDomain domain = fault_point ? fault_point->domain : FaultDomain::kMailbox;

    auto idx = static_cast<size_t>(domain);
    assert(idx < 14);
    domain_ticks_[idx]++;

    uint64_t current_tick = domain_ticks_[idx];

    // Snapshot the schedule pointer — if load() swaps it out concurrently
    // on a broadcast, our local shared_ptr keeps the old vector alive.
    auto sched = schedule_;
    if (!sched) return false;

    for (size_t i = schedule_cursor_; i < sched->entries().size(); ++i) {
        const auto& entry = sched->entries()[i];
        if (entry.domain != domain) continue;
        if (entry.at_tick > current_tick) break;

        if (entry.at_tick == current_tick && entry.path == path) {
            if (entry.target.has_value() && target.has_value() &&
                entry.target.value() != target.value()) {
                continue;
            }
            schedule_cursor_ = i + 1;
            faults_fired_.fetch_add(1, std::memory_order_relaxed);

            // Emit fault timeline log entry
            if (log_manager_) {
                log::LogField fields[] = {
                    log::field_lit("domain",
                                   to_string(entry.domain).data()),
                    log::field("tick", current_tick),
                    log::field_lit("path", entry.path.c_str()),
                    log::field_lit("action",
                                   to_string(entry.action).data()),
                };
                log_manager_->logger().emit(
                    log::LogLevel::kInfo, log::LogCategory::kFault,
                    target.value_or(ActorId{0}), 0, "fault_inject",
                    fields,
                    static_cast<uint8_t>(
                        sizeof(fields) / sizeof(fields[0])),
                    __FILE__, __LINE__);
            }

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
    assert(idx < 14);
    for (uint64_t i = 0; i < delay_ticks; ++i) {
        domain_ticks_[idx]++;
    }
}

FaultControllerSnapshot FaultController::snapshot() const {
    FaultControllerSnapshot snap{};
    snap.enabled = enabled_.load(std::memory_order_acquire);
    snap.active_scope = active_scope_;
    snap.replay_seed = replay_seed_;
    snap.schedule_entry_count = schedule_ ? schedule_->size() : 0;
    for (size_t i = 0; i < 14; ++i) {
        snap.domain_ticks[i] = domain_ticks_[i];
    }
    snap.faults_fired = faults_fired_.load(std::memory_order_acquire);
    return snap;
}

FaultControllerSnapshot FaultController::aggregate_snapshot() {
    FaultControllerSnapshot snap{};
    snap.enabled = false;
    snap.faults_fired = 0;
    for (size_t i = 0; i < 14; ++i) snap.domain_ticks[i] = 0;

    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    for (auto* fc : list.instances) {
        auto s = fc->snapshot();
        if (s.enabled) snap.enabled = true;
        snap.faults_fired += s.faults_fired;
        snap.schedule_entry_count = std::max(snap.schedule_entry_count, s.schedule_entry_count);
        for (size_t i = 0; i < 14; ++i) {
            snap.domain_ticks[i] += s.domain_ticks[i];
        }
        if (!s.active_scope.empty() && s.active_scope != "*") {
            snap.active_scope = s.active_scope;
        }
    }
    if (snap.active_scope.empty()) snap.active_scope = "*";

    return snap;
}

void FaultController::install() {
    tls_instance_ = this;
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    // Guard against double-registration
    if (std::find(list.instances.begin(), list.instances.end(), this) ==
        list.instances.end()) {
        list.instances.push_back(this);
    }
}

void FaultController::remove() {
    auto& list = instance_list();
    std::lock_guard<std::mutex> lock(list.mutex);
    auto it = std::find(list.instances.begin(), list.instances.end(), this);
    if (it != list.instances.end()) {
        list.instances.erase(it);
    }
    tls_instance_ = nullptr;
}

FaultController* FaultController::instance() {
    return tls_instance_;
}

} // namespace hpactor::fault
