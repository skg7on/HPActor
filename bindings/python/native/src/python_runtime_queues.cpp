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

#include <hpactor/python/python_runtime_queues.hpp>

namespace hpactor::python {

PythonRuntimeQueues::PythonRuntimeQueues(const PythonRuntimeConfig& cfg)
    : dispatch_queue_(cfg.dispatch_queue_capacity),
      command_queue_(cfg.command_queue_capacity),
      completion_queue_(cfg.completion_queue_capacity) {}

bool PythonRuntimeQueues::try_push_dispatch(PythonDispatchPtr envelope) noexcept {
    if (dispatch_queue_.try_push(envelope)) {
        return true;
    }
    dispatch_rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool PythonRuntimeQueues::try_push_command(PythonCommandPtr cmd) noexcept {
    if (command_queue_.try_push(cmd)) {
        return true;
    }
    command_rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool PythonRuntimeQueues::try_push_completion(PythonCompletionPtr completion) noexcept {
    if (completion_queue_.try_push(completion)) {
        return true;
    }
    completion_rejected_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

PythonQueueSnapshot PythonRuntimeQueues::snapshot() const noexcept {
    PythonQueueSnapshot snap;
    snap.dispatch_depth = dispatch_queue_.size();
    snap.command_depth = command_queue_.size();
    snap.completion_depth = completion_queue_.size();
    snap.dispatch_rejected = dispatch_rejected_.load(std::memory_order_relaxed);
    snap.command_rejected = command_rejected_.load(std::memory_order_relaxed);
    snap.completion_rejected = completion_rejected_.load(std::memory_order_relaxed);
    return snap;
}

} // namespace hpactor::python
