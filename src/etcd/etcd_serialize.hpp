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

#include <hpactor/cluster/singleton/leadership_lease.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace hpactor::etcd {

/// rief Serialize a LeadershipLease to JSON for etcd key value storage.
std::string serialize_lease(const cluster::singleton::LeadershipLease& lease);

/// rief Deserialize a LeadershipLease from JSON.
/// Returns nullopt on parse errors or corrupt data.
std::optional<cluster::singleton::LeadershipLease>
deserialize_lease(std::string_view data);

/// rief Format the etcd key path for a singleton's owner record.
std::string owner_key(std::string_view key_prefix, std::string_view cluster_id,
                      std::string_view singleton_name);

} // namespace hpactor::etcd
