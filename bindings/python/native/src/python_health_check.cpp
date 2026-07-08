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

#include <hpactor/python/python_health_check.hpp>
#include <hpactor/python/python_runtime.hpp>

namespace hpactor::python {

PythonRuntimeHealthCheck::PythonRuntimeHealthCheck(const PythonRuntime* runtime) noexcept
    : runtime_(runtime) {}

std::string PythonRuntimeHealthCheck::name() const noexcept {
    return "python-runtime";
}

bool PythonRuntimeHealthCheck::is_critical() const noexcept {
    return false; // Python binding is non-critical to core actor system.
}

result<void>
PythonRuntimeHealthCheck::check(uint32_t /*loop_lag_unready_ms*/) const noexcept {
    if (!runtime_) {
        // Binding disabled — always healthy.
        return result<void>::make();
    }

    auto snap = runtime_->snapshot();

    if (snap.state != PythonRuntimeState::Running) {
        return result<void>::make(
            error(errors::unknown, "python runtime not running"));
    }

    if (snap.last_heartbeat_ns == 0) {
        return result<void>::make(
            error(errors::unknown, "python runtime not started"));
    }

    // Heartbeat-based readiness: lag beyond threshold means unready.
    // Stub: loop_lag_ns is computed from heartbeat age in snapshot().
    // For now, use the ready flag computed by the snapshot.
    if (!snap.ready) {
        return result<void>::make(
            error(errors::unknown, "python event loop heartbeat stale"));
    }

    return result<void>::make();
}

} // namespace hpactor::python
