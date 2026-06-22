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

#include <hpactor/adt/node_identity.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::net {

/// \brief Lifecycle status of a cluster member.
enum class MemberStatus : uint8_t {
    Alive = 0,      ///< Member is healthy and reachable.
    Suspicious = 1, ///< Member has missed heartbeats; failure detection in
                    ///< progress.
    Dead = 2,       ///< Member is confirmed unreachable.
    Left = 3,       ///< Member gracefully left the cluster.
};

/// \brief Cluster member descriptor used by all discovery backends.
///
/// Holds identity, advertised actor types, SWIM-style incarnation counter,
/// and a last-seen timestamp that is set locally on message receipt.
struct Member {
    /// \brief Node identity (endpoint, host, acceptors).
    NodeIdentity identity;
    /// \brief Actor types this member can host.
    std::vector<std::string> actor_types;
    /// \brief Current lifecycle status.
    MemberStatus status = MemberStatus::Alive;
    /// \brief SWIM incarnation number; higher values win in conflict
    /// resolution.
    uint64_t incarnation = 0;
    /// \brief Local timestamp of last message from this member.
    ///
    /// Not transmitted on wire — receivers set to \c steady_clock::now() on
    /// receipt.
    std::chrono::steady_clock::time_point last_seen;
};

/// \brief Callback invoked when cluster membership changes.
///
/// \param[in] member The member whose status changed.
/// \param[in] joined \c true if the member joined, \c false if it left or was
/// removed.
using MemberChangeCallback = std::function<void(const Member&, bool joined)>;

/// \brief Pluggable service-discovery interface.
///
/// Implementations provide cluster membership management for the actor system.
/// Four backends are available:
/// - \c UdpRegistrar — same-host UDP-based discovery.
/// - \c GossipMembership — cross-server SWIM protocol.
/// - \c HybridDiscovery — composes \c UdpRegistrar + \c GossipMembership.
/// - \c StaticDiscovery — fixed topology from configuration.
///
/// \note Thread safety: \c discover_all() and \c discover() may be called
///       from any thread. Implementations must synchronize member access.
class IServiceDiscovery {
  public:
    virtual ~IServiceDiscovery() = default;

    /// \brief Start membership protocol and begin discovery.
    ///
    /// Must be called before any other method except configuration setters.
    /// \note Must be safe to call once; repeat calls should be no-ops.
    virtual void start() = 0;

    /// \brief Stop membership protocol and release resources.
    ///
    /// After \c stop() returns, \c discover_all() and \c discover() may
    /// return empty or stale results.
    virtual void stop() = 0;

    /// \brief Return a snapshot of all known cluster members.
    ///
    /// \return Copy of the current member list.
    /// \note Thread safety: Safe to call from any thread.
    virtual std::vector<Member> discover_all() const = 0;

    /// \brief Look up a member by endpoint.
    ///
    /// \param[in] ep Endpoint to search for.
    /// \return Pointer to the member, or \c nullptr if not found.
    /// \note The returned pointer may be invalidated by the next membership
    ///       change. Copy the \c Member if you need stability.
    virtual const Member* discover(EndPoint ep) const = 0;

    /// \brief Announce this node's presence to the cluster.
    ///
    /// \param[in] local_state This node's identity, actor types, and status.
    /// \note For gossip backends, this bumps the local incarnation number.
    virtual void announce(Member local_state) = 0;

    /// \brief Register a callback for membership change events.
    ///
    /// \param[in] cb Callback invoked on member join/leave.
    /// \note Only one callback is stored; subsequent calls replace the
    ///       previous registration.
    virtual void on_member_change(MemberChangeCallback cb) = 0;

    /// \brief Return a human-readable backend name for diagnostics.
    ///
    /// \return Backend identifier (e.g., \c "gossip", \c "udp-registrar",
    ///         \c "static", \c "hybrid").
    virtual std::string backend_name() const = 0;

    /// \brief Access the raw member map for observability.
    ///
    /// \return Pointer to the internal member map, or \c nullptr if not
    ///         exposed by this backend.
    /// \note The returned pointer reflects a snapshot-in-time; do not hold
    ///       across calls that modify membership.
    virtual const std::unordered_map<EndPoint, Member>* raw_members() const {
        return nullptr;
    }
};

} // namespace hpactor::net
