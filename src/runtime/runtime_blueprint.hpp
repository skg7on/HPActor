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

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {

// ── Component configs (immutable, value types) ──────────────────────────────

/// \brief Validated actor-runtime configuration (scheduler, identity, pool).
struct ActorRuntimeConfig final {
    size_t scheduler_threads{4};
    size_t max_queue_depth{1024};
    EndPoint endpoint{LocalEndpoint};
    bool use_coroutines{false};
    bool scheduler_start_paused{false};
};

/// \brief Validated messaging-runtime configuration (DLQ, TTL, mailbox).
struct MessagingRuntimeConfig final {
    size_t default_mailbox_capacity{1024};
    size_t default_mailbox_byte_capacity{64 * 1024};
    std::chrono::milliseconds default_message_ttl_ms{0};
};

/// \brief Validated stream-runtime configuration.
struct StreamRuntimeConfig final {
    size_t max_active_streams{4096};
};

/// \brief Validated network-runtime configuration (present when networking
///        is enabled).
struct BlueprintNetworkConfig final {
    bool enabled{false};
    uint16_t tcp_port{0};
    bool http_client_enabled{false};
    bool http_gateway_enabled{false};
    std::string http_bind_host{"0.0.0.0"};
    uint16_t http_port{8080};
};

// ── Topology actor spec ─────────────────────────────────────────────────────

/// \brief Validated specification for a configured actor from topology.
struct ConfiguredActorSpec {
    std::string id;       ///< Actor instance name in topology.
    std::string behavior; ///< Behavior type name (must be in factory registry).
};

// ── RuntimeBlueprint ────────────────────────────────────────────────────────

/// \brief Immutable validated startup configuration for the actor runtime.
///
/// Built by \c RuntimeBlueprintBuilder from \c Config and optional
/// TOML topology. Once constructed, the blueprint is immutable — component
/// code does not parse TOML or read the mutable legacy \c Config.
///
/// \note All accessors are const. There are no public setters.
class RuntimeBlueprint final {
  public:
    /// \brief Default-construct an empty blueprint (fingerprint == 0).
    RuntimeBlueprint() = default;

    // ── Component config accessors ──────────────────────────────────────────

    const ActorRuntimeConfig& actor() const noexcept {
        return actor_;
    }
    const MessagingRuntimeConfig& messaging() const noexcept {
        return messaging_;
    }
    const StreamRuntimeConfig& streams() const noexcept {
        return streams_;
    }
    const std::optional<BlueprintNetworkConfig>& network() const noexcept {
        return network_;
    }

    /// \brief Validated topology actor specs (empty if no topology loaded).
    const std::vector<ConfiguredActorSpec>& actors() const noexcept {
        return actors_;
    }

    /// \brief Deterministic fingerprint of the complete effective
    /// configuration.
    ///
    /// Two blueprints with identical effective values produce the same
    /// fingerprint. Used for reload diff detection.
    uint64_t fingerprint() const noexcept {
        return fingerprint_;
    }

  private:
    friend class RuntimeBlueprintBuilder;

    ActorRuntimeConfig actor_;
    MessagingRuntimeConfig messaging_;
    StreamRuntimeConfig streams_;
    std::optional<BlueprintNetworkConfig> network_;
    std::vector<ConfiguredActorSpec> actors_;
    uint64_t fingerprint_{0};
};

} // namespace hpactor
