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
#include <string>

namespace hpactor::metrics {

/// \brief Configuration for the metrics subsystem.
///
/// Maps to TOML keys under \c [system.metrics].
struct MetricsConfig {
    /// \brief Enable metrics collection. Default true.
    bool enabled = true;
    /// \brief Capacity of the metric event ring buffer (must be a power of
    /// two).
    uint32_t ring_buffer_capacity = 65536;
    /// \brief HTTP path for the Prometheus /metrics endpoint.
    std::string metrics_path = "/metrics";
    /// \brief Include per-actor labels in metric output.
    bool per_actor_labels = true;
    /// \brief Emit scheduler dispatch/steal metric events.
    bool scheduler_metrics = true;
    /// \brief Emit memory alloc/free metric events.
    bool memory_metrics = true;
};

} // namespace hpactor::metrics
