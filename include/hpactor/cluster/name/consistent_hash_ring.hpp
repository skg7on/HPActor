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
#include <map>
#include <optional>
#include <set>
#include <string_view>

#include <hpactor/ref/actor_address.hpp>

namespace hpactor::cluster::name {

/// \brief Comparator for deterministic ordering of EndPoint in std::set.
///
/// EndPoint (\c std::variant<Ipv4Endpoint, Ipv6Endpoint>) has no
/// \c operator<. This comparator uses \c endpoint_ops::to_string() for
/// lexicographic ordering, enabling use in ordered containers.
///
/// \note Not a hot-path operation — called only during ring rebuilds on
///       membership change events.
struct EndPointCompare {
    /// \brief Lexicographic comparison of two endpoints via their string
    ///        representations.
    /// \param[in] a First endpoint.
    /// \param[in] b Second endpoint.
    /// \return \c true if \p a < \p b by string ordering.
    bool operator()(const EndPoint& a, const EndPoint& b) const noexcept;
};

/// \brief Hash token type used for ring position.
using HashToken = uint64_t;

/// \brief Deterministic consistent hash ring from IServiceDiscovery membership.
///
/// Builds a ring from the live node set with N virtual nodes per physical
/// node. Every node with the same membership view produces identical rings,
/// so all nodes agree on the home node for any name without coordination.
///
/// Hash function: FNV-1a 64-bit (reused from HPActor fingerprint code).
class ConsistentHashRing {
  public:
    ConsistentHashRing() = default;

    /// \brief Rebuild the ring from the live membership set.
    ///
    /// \param[in] live_members Set of alive node endpoints.
    /// \param[in] virtual_nodes Virtual nodes per physical node (default 100).
    void build(const std::set<EndPoint, EndPointCompare>& live_members,
               uint32_t virtual_nodes = 100);

    /// \brief Find the home node for a name.
    ///
    /// \param[in] name Actor name to resolve.
    /// \return The owning EndPoint, or std::nullopt if the ring is empty.
    std::optional<EndPoint> lookup(std::string_view name) const;

    /// \brief True when the ring has no nodes.
    [[nodiscard]] bool empty() const noexcept { return ring_.empty(); }

    /// \brief Number of physical nodes in the ring.
    [[nodiscard]] size_t size() const noexcept { return physical_nodes_.size(); }

  private:
    /// Compute FNV-1a 64-bit hash of a string_view.
    static HashToken hash(std::string_view s) noexcept;

    /// Compute hash of (physical_node, virtual_index) for virtual node
    /// placement.
    static HashToken virtual_node_hash(EndPoint node, uint32_t vn) noexcept;

    std::map<HashToken, EndPoint> ring_;              // token → node (sorted)
    std::set<EndPoint, EndPointCompare> physical_nodes_; // track count
};

} // namespace hpactor::cluster::name
