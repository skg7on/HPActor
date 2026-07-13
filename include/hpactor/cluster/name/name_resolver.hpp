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
    Ok,              ///< Registration succeeded on the home node.
    DuplicateName,   ///< Another actor already holds this name with the same
                     ///< generation.
    StaleGeneration, ///< The caller's generation is behind the existing entry.
    Timeout,         ///< The home node did not respond within the configured
                     ///< timeout.
    Disabled,        ///< Name resolution is disabled in the local config.
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
    /// \brief Construct with all dependencies fixed.
    ///
    /// All dependencies are set at construction — no late setters. The
    /// initial hash ring is built from \c discovery_'s current membership.
    ///
    /// \param[in,out] name_directory Home-node store served by this node.
    /// \param[in,out] discovery Membership source for ring construction.
    /// \param[in,out] cache TTL cache shared across all resolutions.
    /// \param[in] config Timeouts, TTL, and virtual-node count.
    /// \param[in] local_endpoint This node's authoritative endpoint.
    /// \param[in] outbound_port Sends name-protocol messages to peers.
    /// \param[in] inbound_port Receives name-protocol frames from peers.
    /// \note The referenced objects must outlive the NameResolver.
    NameResolver(NameDirectory& name_directory,
                 net::IServiceDiscovery& discovery,
                 NameResolveCache& cache,
                 const config::NameResolutionConfig& config,
                 EndPoint local_endpoint,
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
    ///
    /// Hashes \p name to find the home node. If this node is the home,
    /// commits to the local \c NameDirectory; otherwise sends a
    /// \c NameRegisterRequest to the remote home node via
    /// \c outbound_port_.
    ///
    /// \param[in] name Actor name being registered.
    /// \param[in] address Full ActorAddress of the registered actor.
    /// \param[in] generation Monotonic counter for stale detection.
    void on_local_register(std::string_view name, ActorAddress address,
                           uint64_t generation);

    /// \brief Called by ActorDirectory when a name is unregistered locally.
    ///
    /// Hashes \p name to find the home node. If remote, sends a
    /// fire-and-forget \c NameUnregisterRequest via \c outbound_port_.
    /// Always evicts the name from the local \c cache_.
    ///
    /// \param[in] name Actor name being unregistered.
    void on_local_unregister(std::string_view name);

    // ── Membership callback ────────────────────────────────────────────────

    /// \brief Called when IServiceDiscovery reports membership changes.
    ///
    /// Rebuilds the hash ring from the current membership set (reported via
    /// \c discovery_.discover_all()) and evicts cache entries for every
    /// endpoint in \p removed.
    ///
    /// \param[in] added Endpoints that joined the cluster (unused — ring is
    ///                  rebuilt from the full membership set).
    /// \param[in] removed Endpoints that left the cluster — names homed on
    ///                    or hosted on these nodes are purged.
    void on_membership_change(const std::vector<EndPoint>& added,
                              const std::vector<EndPoint>& removed);

    // ── Inbound protocol handlers (via InboundNamePort) ────────────────────

    /// \brief Handle an incoming name registration request from a peer.
    ///
    /// Commits the registration to the local \c NameDirectory
    /// (this node must be the home node for \p name).
    ///
    /// \param[in] from Peer endpoint that sent the request
    ///                 (informational — not validated).
    /// \param[in] name Actor name to register.
    /// \param[in] address ActorAddress of the actor being registered.
    /// \param[in] generation Monotonic generation counter.
    /// \return \c NameRegisterResult reflecting the registration outcome.
    NameRegisterResult
    on_name_register_request(EndPoint from, std::string_view name,
                             ActorAddress address, uint64_t generation);

    /// \brief Handle an incoming name resolution query from a peer.
    ///
    /// Looks up \p name in the local \c NameDirectory and returns
    /// the entry if found.
    ///
    /// \param[in] from Peer endpoint that sent the query (informational).
    /// \param[in] name Actor name to resolve.
    /// \return \c NameResolveResult with the \c ActorAddress if found.
    NameResolveResult
    on_name_resolve_query(EndPoint from, std::string_view name);

    /// \brief Handle an incoming name unregistration request from a peer.
    ///
    /// Removes \p name from the local \c NameDirectory if the
    /// \p generation is greater than or equal to the existing entry's
    /// generation (stale guard).
    ///
    /// \param[in] from Peer endpoint that sent the request (informational).
    /// \param[in] name Actor name to unregister.
    /// \param[in] generation Sender's generation counter.
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
    config::NameResolutionConfig config_;
    EndPoint local_endpoint_;
    OutboundNameQueryPort outbound_port_;
    InboundNamePort inbound_port_;
    ConsistentHashRing ring_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::cluster::name
