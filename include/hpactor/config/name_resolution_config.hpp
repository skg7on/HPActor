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

#include <chrono>
#include <cstdint>

namespace hpactor::config {

/// \brief Configuration for the distributed name resolution subsystem.
///
/// Mirrors the TOML `[system.name_resolution]` section. All fields have
/// safe defaults suitable for a small-to-medium cluster.
struct NameResolutionConfig {
    /// Enable cross-node name resolution. When false, only local
    /// ActorDirectory lookups are performed (backward-compatible default).
    bool enabled{false};

    /// Timeout for remote name resolution queries (milliseconds).
    uint32_t resolve_timeout_ms{2000};

    /// Timeout for remote name registration requests (milliseconds).
    uint32_t register_timeout_ms{5000};

    /// TTL for locally cached remote name->address entries (seconds).
    uint32_t cache_ttl_seconds{30};

    /// Virtual nodes per physical node in the consistent hash ring.
    /// 100 replicas provides ~+/-1% imbalance.
    uint32_t virtual_nodes{100};

    /// Validate that all fields are within acceptable bounds.
    ///
    /// \retval true All fields are within their configured ranges.
    /// \note Does not modify any state — pure const predicate.
    [[nodiscard]] bool valid() const noexcept {
        return resolve_timeout_ms >= 100 &&
               resolve_timeout_ms <= 60'000 &&
               register_timeout_ms >= 100 &&
               register_timeout_ms <= 60'000 &&
               cache_ttl_seconds >= 1 &&
               cache_ttl_seconds <= 3600 &&
               virtual_nodes >= 1 &&
               virtual_nodes <= 1000;
    }
};

} // namespace hpactor::config
