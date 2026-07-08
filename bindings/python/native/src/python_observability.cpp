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

#include <hpactor/python/python_observability.hpp>

#include <mutex>
#include <unordered_map>

namespace hpactor::python {

PythonObservability::PythonObservability(Dependencies deps) noexcept
    : metrics_(deps.metrics), logger_(deps.logger), tracer_(deps.tracer) {
    // Metric families are registered lazily on first use by the metrics
    // subsystem; explicit registration is a no-op for missing families.
}

void PythonObservability::record_dispatch(const std::string& /*actor_type*/,
                                          bool accepted) noexcept {
    (void)accepted;
    // Stub: metric increment deferred to metrics subsystem integration.
}

void PythonObservability::record_dispatch_rejected(
    const std::string& /*actor_type*/) noexcept {
    // Stub.
}

void PythonObservability::record_command(bool /*accepted*/) noexcept {
    // Stub.
}

void PythonObservability::record_command_rejected() noexcept {
    // Stub.
}

void PythonObservability::record_handler(const std::string& /*actor_type*/,
                                         std::chrono::microseconds /*duration*/,
                                         bool /*exception*/) noexcept {
    // Stub.
}

void PythonObservability::record_handler_exception(
    const std::string& /*actor_type*/) noexcept {
    // Stub.
}

void PythonObservability::record_handler_cancelled(
    const std::string& /*actor_type*/) noexcept {
    // Stub.
}

void PythonObservability::record_loop_lag(std::chrono::microseconds /*lag*/) noexcept {
    // Stub.
}

void PythonObservability::record_stale_completion() noexcept {
    // Stub.
}

void PythonObservability::log_handler_failure(
    const std::string& /*actor_type*/, const std::string& /*exception_type*/,
    const std::string& /*detail*/, const std::string& /*traceback*/) noexcept {
    // Stub: structured log emission deferred.
}

uint64_t
PythonObservability::begin_handler_span(const PythonHandlerSpanStart& /*start*/) noexcept {
    if (!tracer_)
        return 0;
    // Stub: span creation deferred.
    return 0;
}

void PythonObservability::finish_handler_span(uint64_t token,
                                              PythonHandlerSpanStatus /*status*/) noexcept {
    if (!tracer_ || token == 0)
        return;
    // Stub: span finishing deferred.
}

} // namespace hpactor::python
