// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string_view>

#include <hpactor/cluster/name/consistent_hash_ring.hpp>
#include <hpactor/cluster/name/inbound_name_port.hpp>
#include <hpactor/cluster/name/name_directory.hpp>
#include <hpactor/cluster/name/name_registration_port.hpp>
#include <hpactor/cluster/name/name_resolve_cache.hpp>
#include <hpactor/cluster/name/outbound_name_query_port.hpp>
#include <hpactor/config/name_resolution_config.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/ref/actor_address.hpp>

namespace hpactor::cluster::name {

/// \brief Result of a cross-node name registration attempt.
enum class NameRegisterResult : uint8_t {
    Ok,
    DuplicateName,
    StaleGeneration,
    Timeout,
    Disabled,
};

/// \brief Result of a cross-node name resolution.
struct NameResolveResult {
    std::optional<ActorAddress> address;
};

/// \brief Cluster glue bridging ActorDirectory, IServiceDiscovery, and the
///        home-node name protocol.
///
/// Owned by the runtime (ActorSystem::Impl). Not an actor — all public
/// methods are thread-safe. Uses function-pointer ports for outbound
/// messaging and inbound dispatch; no std::function, no ActorSystem*
/// capture.
class NameResolver {
  public:
    /// \brief All dependencies fixed at construction.
    NameResolver(NameDirectory& name_directory,
                 net::IServiceDiscovery& discovery,
                 NameResolveCache& cache,
                 const config::NameResolutionConfig& config,
                 OutboundNameQueryPort outbound_port,
                 InboundNamePort inbound_port);

    /// \brief Resolve a name to an ActorAddress.
    ///
    /// Three-tier cascade: local NameDirectory → cache → remote query.
    /// \param[in] name Actor name.
    /// \return ActorAddress if resolved, std::nullopt otherwise.
    std::optional<ActorAddress> resolve(std::string_view name);

    // ── ActorDirectory callbacks (via NameRegistrationPort) ────────────────

    /// \brief Called by ActorDirectory when a name is registered locally.
    void on_local_register(std::string_view name, ActorAddress address,
                           uint64_t generation);

    /// \brief Called by ActorDirectory when a name is unregistered locally.
    void on_local_unregister(std::string_view name);

    // ── Membership callback ────────────────────────────────────────────────

    /// \brief Called when IServiceDiscovery reports membership changes.
    ///
    /// Rebuilds the hash ring and evicts cache entries for departed nodes.
    void on_membership_change(const std::vector<EndPoint>& added,
                              const std::vector<EndPoint>& removed);

    // ── Inbound protocol handlers (via InboundNamePort) ────────────────────

    NameRegisterResult
    on_name_register_request(EndPoint from, std::string_view name,
                             ActorAddress address, uint64_t generation);

    NameResolveResult
    on_name_resolve_query(EndPoint from, std::string_view name);

    void on_name_unregister_request(EndPoint from,
                                    std::string_view name,
                                    uint64_t generation);

    // ── Observability ──────────────────────────────────────────────────────

    /// \brief Current number of names homed on this node.
    size_t home_entry_count() const noexcept;

    /// \brief Local node's endpoint (derived from discovery).
    EndPoint local_endpoint() const noexcept;

  private:
    NameDirectory& name_directory_;
    net::IServiceDiscovery& discovery_;
    NameResolveCache& cache_;
    const config::NameResolutionConfig& config_;
    OutboundNameQueryPort outbound_port_;
    InboundNamePort inbound_port_;
    ConsistentHashRing ring_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::cluster::name
