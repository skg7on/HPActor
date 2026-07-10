// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/cluster/singleton/leadership_lease.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace hpactor::etcd {

/// \brief Serialize a LeadershipLease to JSON for etcd key value storage.
std::string serialize_lease(const cluster::singleton::LeadershipLease& lease);

/// \brief Deserialize a LeadershipLease from JSON.
/// Returns nullopt on parse errors or corrupt data.
std::optional<cluster::singleton::LeadershipLease>
deserialize_lease(std::string_view data);

/// \brief Format the etcd key path for a singleton's owner record.
std::string owner_key(std::string_view key_prefix, std::string_view cluster_id,
                      std::string_view singleton_name);

} // namespace hpactor::etcd
