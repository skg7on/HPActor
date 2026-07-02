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

#include <hpactor/types/types.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace hpactor {

namespace net {
struct Member;
} // namespace net

/// \brief Immutable snapshot of cluster runtime state.
struct ClusterSnapshot final {
    bool enabled{false};
    std::string node_id;
    uint32_t member_count{0};
    bool singleton_active{false};
    uint64_t epoch{0};
};

/// \brief Typed legacy views for backward-compatible cluster access.
/// Deprecated in Phase 8 — new code must use the IClusterRuntime interface.
struct ClusterLegacyViews {
    void* failure_model{nullptr};
    void* singleton_manager{nullptr};
    void* route_invalidation{nullptr};
};

/// \brief Request parameters for cluster stop.
struct ClusterStopRequest {
    bool drain_callbacks{true};
    std::chrono::milliseconds drain_timeout{5000};
};

/// \brief Dependencies injected into a cluster runtime implementation.
struct ClusterRuntimeDependencies {
    std::string node_id;
};

/// \brief Typed control-plane boundary for the cluster subsystem.
///
/// Virtual dispatch is acceptable here — this is a low-frequency
/// cross-library control plane, not a message hot path.
class IClusterRuntime {
  public:
    virtual ~IClusterRuntime() = default;

    virtual result<void> start() noexcept = 0;
    virtual result<void> stop(const ClusterStopRequest& req) noexcept = 0;

    /// \brief Called by NetworkRuntime when a member joins or leaves.
    virtual void
    on_member_changed(const net::Member& member, bool joined) noexcept = 0;

    virtual ClusterSnapshot snapshot() const noexcept = 0;

    /// \brief Typed legacy views for Phase 7 compatibility accessors.
    /// Phase 8 will remove this.
    virtual ClusterLegacyViews legacy_views() noexcept = 0;
};

/// \brief Factory function signature for creating a cluster runtime.
///
/// \param[in] deps     Cluster runtime dependencies.
/// \param[in] reserved  Reserved for future use (pass \c nullptr).
/// \return A new cluster runtime instance, or \c nullptr when cluster is
///         disabled.
using ClusterRuntimeFactory = std::unique_ptr<IClusterRuntime> (*)(
    const ClusterRuntimeDependencies& deps, void* reserved) noexcept;

} // namespace hpactor
