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

namespace hpactor {
namespace net {

/// \brief Bounded, copyable snapshot of NetworkRuntime state.
///
/// All fields are atomic snapshots; no transport-internal objects
/// are exposed. Suitable for CLI, metrics, and admin inspection.
struct NetworkSnapshot {
    /// \brief Lifecycle state (0=Constructed, 1=Starting, 2=Running,
    ///        3=Stopping, 4=Stopped, 5=Failed).
    uint8_t state{0};

    /// \brief Whether networking was enabled at construction.
    bool enabled{false};

    /// \brief Whether the transport is currently listening.
    bool listening{false};

    /// \brief Discovery backend status (0=stopped, 1=starting, 2=running).
    uint8_t discovery_status{0};

    /// \brief Approximate member count from discovery.
    uint32_t member_count{0};

    /// \brief Active transport connections.
    uint32_t active_connections{0};

    /// \brief Idle (pooled) transport connections.
    uint32_t idle_connections{0};

    /// \brief Pending RPC operations.
    uint32_t pending_rpc{0};

    /// \brief Pending HTTP operations.
    uint32_t pending_http{0};

    /// \brief Location cache entry count.
    uint32_t cache_entries{0};

    /// \brief Callbacks rejected due to ingress gate closed.
    uint64_t callback_rejections{0};

    /// \brief Whether the network thread is running (1) or joined (0).
    uint8_t loop_thread_running{0};

    /// \brief Last startup/stop stage attempted.
    uint8_t last_stage{0};

    /// \brief Error code from last stage (0 for success).
    uint32_t last_error{0};
};

} // namespace net
} // namespace hpactor
