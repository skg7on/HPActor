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

namespace hpactor {

/// \brief Aggregate snapshot of all runtime components for operations
/// inspection (CLI, admin API, health endpoints).
///
/// Each component snapshot is a bounded copy of its state at collection time.
/// Per-component epochs may differ — this is a coordinated best-effort
/// snapshot, NOT an instantaneously atomic view.
struct OperationsSnapshot final {
    /// \brief Wall-clock time when collection started (ns since epoch).
    uint64_t collection_start_ns{0};
    /// \brief Wall-clock time when collection completed (ns since epoch).
    uint64_t collection_end_ns{0};

    /// \brief Lifecycle state description.
    std::string lifecycle_phase;

    /// \brief Actor count.
    uint64_t actor_count{0};

    /// \brief Observability state.
    bool metrics_enabled{false};
    bool logging_enabled{false};
    bool tracing_enabled{false};
    uint64_t metrics_drops{0};
    uint64_t log_drops{0};
    uint64_t trace_drops{0};

    /// \brief Network state.
    bool network_enabled{false};
    uint16_t tcp_port{0};

    /// \brief Cluster state.
    bool cluster_enabled{false};
    uint32_t cluster_member_count{0};

    /// \brief Messaging state.
    uint64_t dlq_record_count{0};
};

} // namespace hpactor
