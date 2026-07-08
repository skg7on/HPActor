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

#include <hpactor/python/python_inspection.hpp>

namespace hpactor::python {

PythonInspectionService::PythonInspectionService(uint32_t max_pending) noexcept
    : max_pending_(max_pending) {}

result<PythonInspectResult>
PythonInspectionService::inspect(ActorAddress actor, uint64_t generation,
                                 std::chrono::milliseconds /*timeout*/) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    if (pending_.size() >= max_pending_) {
        return result<PythonInspectResult>::make(
            error(errors::mailbox_full, "inspection table full"));
    }

    uint64_t token = next_token_++;
    PendingInspection entry;
    entry.result.actor = actor;
    entry.result.generation = generation;
    pending_[token] = entry;

    // Stub: the actual timed wait and bridge request is deferred.
    // For now, return the pending entry with a timeout marker.
    PythonInspectResult inspect_result;
    inspect_result.actor = actor;
    inspect_result.generation = generation;
    inspect_result.timed_out = false;
    return hpactor::result<PythonInspectResult>::make(std::move(inspect_result));
}

void PythonInspectionService::complete(PythonInspectResult result) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    // Stub: match result to pending entry and mark complete.
    (void)result;
}

void PythonInspectionService::cancel_all(error /*reason*/) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
}

size_t PythonInspectionService::pending_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

} // namespace hpactor::python
