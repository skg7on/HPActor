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
#include <hpactor/cluster/name/name_resolver.hpp>
#include <hpactor/cluster/name/consistent_hash_ring.hpp>
#include <hpactor/msg/name_directory_tags.hpp>

namespace hpactor::cluster::name {

NameResolver::NameResolver(NameDirectory& name_directory,
                           net::IServiceDiscovery& discovery,
                           NameResolveCache& cache,
                           const config::NameResolutionConfig& config,
                           EndPoint local_endpoint,
                           OutboundNameQueryPort outbound_port,
                           InboundNamePort inbound_port)
    : name_directory_(name_directory)
    , discovery_(discovery)
    , cache_(cache)
    , config_(config)
    , local_endpoint_(std::move(local_endpoint))
    , outbound_port_(outbound_port)
    , inbound_port_(inbound_port) {
    // Build initial ring from current membership.
    auto members = discovery_.discover_all();
    std::set<EndPoint, EndPointCompare> member_set;
    for (auto& m : members) member_set.insert(m.identity.endpoint);
    ring_.build(member_set, config_.virtual_nodes);
}

std::optional<ActorAddress> NameResolver::resolve(std::string_view name) {
    if (!config_.enabled) return std::nullopt;
    std::string name_str{name};

    // Tier 1: local NameDirectory (this node is the home node).
    // NameDirectory has its own internal mutex — no need to acquire
    // NameResolver::mutex_ (which only guards ring_).
    {
        auto entry = name_directory_.resolve(name_str);
        if (entry.has_value()) {
            return ActorAddress{entry->endpoint, ActorType{0},
                                entry->actor_id, 0};
        }
    }

    // Tier 2: cache hit.
    auto cached = cache_.get(name_str);
    if (cached.has_value()) return cached;

    // Tier 3: remote home-node query.
    std::optional<EndPoint> home;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        home = ring_.lookup(name);
    }
    if (!home.has_value()) return std::nullopt;

    // Cannot query remote without outbound port configured.
    if (!outbound_port_.active()) return std::nullopt;

    // TODO (Task 8): implement asynchronous query via OutboundNameQueryPort.
    // For now, return nullopt — the integration task will wire the
    // request/response cycle.
    return std::nullopt;
}

void NameResolver::on_local_register(std::string_view name,
                                     ActorAddress address,
                                     uint64_t generation) {
    if (!config_.enabled) return;

    std::optional<EndPoint> home;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        home = ring_.lookup(name);
    }
    if (!home.has_value()) return;

    // Check if this node is the home.
    if (*home == local_endpoint()) {
        NameEntry entry;
        entry.actor_id = address.id;
        entry.endpoint = address.endpoint;
        entry.generation = generation;
        entry.registered_at = std::chrono::steady_clock::now();
        name_directory_.register_entry(std::string{name}, entry);
        return;
    }

    // Home is remote — send NameRegisterRequest.
    if (outbound_port_.active()) {
        // Build TypedMessage and send (deferred to integration task).
        // For now: fire-and-forget stub.
    }
}

void NameResolver::on_local_unregister(std::string_view name) {
    if (!config_.enabled) return;
    std::string name_str{name};

    std::optional<EndPoint> home;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        home = ring_.lookup(name);
    }
    if (!home.has_value()) return;

    if (*home == local_endpoint()) {
        name_directory_.unregister(name_str);
    } else if (outbound_port_.active()) {
        // Send NameUnregisterRequest to home node (fire-and-forget).
    }

    // Evict from local cache regardless.
    cache_.evict(name_str);
}

void NameResolver::on_membership_change(
    [[maybe_unused]] const std::vector<EndPoint>& added,
    const std::vector<EndPoint>& removed) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Rebuild ring with current membership from discovery.
    auto members = discovery_.discover_all();
    std::set<EndPoint, EndPointCompare> member_set;
    for (auto& m : members) member_set.insert(m.identity.endpoint);
    ring_.build(member_set, config_.virtual_nodes);

    // Purge entries for departed nodes.
    for (auto& ep : removed) {
        name_directory_.purge_by_endpoint(ep);
        cache_.evict_node(ep);
    }
}

NameRegisterResult
NameResolver::on_name_register_request(EndPoint /*from*/,
                                        std::string_view name,
                                        ActorAddress address,
                                        uint64_t generation) {
    if (!config_.enabled) return NameRegisterResult::Disabled;

    // Verify this node is the authoritative home for this name.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto home = ring_.lookup(name);
        if (!home.has_value() || *home != local_endpoint_) {
            return NameRegisterResult::Disabled;
        }
    }

    NameEntry entry;
    entry.actor_id = address.id;
    entry.endpoint = address.endpoint;
    entry.generation = generation;
    entry.registered_at = std::chrono::steady_clock::now();

    auto result = name_directory_.register_entry(std::string{name}, entry);
    switch (result) {
    case RegisterResult::Ok:           return NameRegisterResult::Ok;
    case RegisterResult::DuplicateName:return NameRegisterResult::DuplicateName;
    case RegisterResult::StaleGeneration:return NameRegisterResult::StaleGeneration;
    }
    return NameRegisterResult::Ok;
}

NameResolveResult
NameResolver::on_name_resolve_query(EndPoint /*from*/, std::string_view name) {
    NameResolveResult result;
    if (!config_.enabled) return result;

    // Verify this node is the home for the queried name.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto home = ring_.lookup(name);
        if (!home.has_value() || *home != local_endpoint_) {
            return result;
        }
    }

    auto entry = name_directory_.resolve(std::string{name});
    if (entry.has_value()) {
        result.address = ActorAddress{entry->endpoint, ActorType{0},
                                       entry->actor_id, 0};
    }
    return result;
}

void NameResolver::on_name_unregister_request(EndPoint /*from*/,
                                               std::string_view name,
                                               uint64_t generation) {
    if (!config_.enabled) return;
    std::string name_str{name};

    // Verify this node is the home for the name being unregistered.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto home = ring_.lookup(name);
        if (!home.has_value() || *home != local_endpoint_) {
            return;
        }
    }

    auto existing = name_directory_.resolve(name_str);
    if (existing.has_value() && generation >= existing->generation) {
        name_directory_.unregister(name_str);
    }
}

size_t NameResolver::home_entry_count() const noexcept {
    return name_directory_.size();
}

EndPoint NameResolver::local_endpoint() const noexcept {
    return local_endpoint_;
}

} // namespace hpactor::cluster::name
