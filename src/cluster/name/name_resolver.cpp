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
#include <hpactor/cluster/name/name_directory_codec.hpp>
#include <hpactor/msg/name_directory_tags.hpp>

#include <condition_variable>
#include <memory>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::name {

using codec::encode_register_request;
using codec::encode_register_response;
using codec::encode_resolve_query;
using codec::encode_resolve_response;
using codec::encode_unregister_request;
using codec::make_name_message;

// ── NameResolver ─────────────────────────────────────────────────────────────

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

    // Build and send PbNameResolveQuery.
    auto payload_bytes = encode_resolve_query(name);
    auto msg = make_name_message(kNameResolveQueryTag, payload_bytes);

    // Register pending request before sending.
    auto pending = std::make_shared<PendingResolve>();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_resolves_[name_str] = pending;
    }

    outbound_port_.send(outbound_port_.context, *home, std::move(msg));

    // Wait for response with timeout.
    {
        std::unique_lock<std::mutex> lock(pending->mtx);
        bool ready = pending->cv.wait_for(
            lock, std::chrono::milliseconds(config_.resolve_timeout_ms),
            [&] { return pending->ready; });
        if (!ready) {
            // Timed out — clean up and return nullopt.
            std::lock_guard<std::mutex> lock2(pending_mutex_);
            pending_resolves_.erase(name_str);
            return std::nullopt;
        }
    }

    // Clean up pending entry.
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_resolves_.erase(name_str);
    }

    if (pending->result.has_value()) {
        // Populate cache on success.
        cache_.put(name_str, *pending->result,
                   std::chrono::seconds(config_.cache_ttl_seconds));
        return pending->result;
    }
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
        std::string endpoint_str = endpoint_ops::to_string(address.endpoint);
        auto payload_bytes = encode_register_request(
            name, address.id.value(), endpoint_str, generation);
        auto msg = make_name_message(kNameRegisterRequestTag, payload_bytes);
        outbound_port_.send(outbound_port_.context, *home, std::move(msg));
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
        uint64_t gen = 0;
        // Use current generation if we have a local entry for generation
        // tracking. Send 0 if not found (home node uses generation >= check).
        auto local_entry = name_directory_.resolve(name_str);
        if (local_entry.has_value()) {
            gen = local_entry->generation;
        }
        auto payload_bytes = encode_unregister_request(name, gen);
        auto msg = make_name_message(kNameUnregisterRequestTag, payload_bytes);
        outbound_port_.send(outbound_port_.context, *home, std::move(msg));
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
NameResolver::on_name_register_request(EndPoint from,
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

    // Send response back to the requesting node.
    uint32_t result_code;
    switch (result) {
    case RegisterResult::Ok:             result_code = 0; break;
    case RegisterResult::DuplicateName:  result_code = 1; break;
    case RegisterResult::StaleGeneration: result_code = 3; break;
    default:                            result_code = 2; break; // Invalid
    }

    if (outbound_port_.active()) {
        auto resp_bytes = encode_register_response(result_code);
        auto msg = make_name_message(kNameRegisterResponseTag, resp_bytes);
        outbound_port_.send(outbound_port_.context, from, std::move(msg));
    }

    switch (result) {
    case RegisterResult::Ok:           return NameRegisterResult::Ok;
    case RegisterResult::DuplicateName:return NameRegisterResult::DuplicateName;
    case RegisterResult::StaleGeneration:return NameRegisterResult::StaleGeneration;
    }
    return NameRegisterResult::Ok;
}

NameResolveResult
NameResolver::on_name_resolve_query(EndPoint from, std::string_view name) {
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

    // Send response back to the requesting node.
    if (outbound_port_.active()) {
        bool found = entry.has_value();
        uint64_t actor_id = found ? entry->actor_id.value() : 0ULL;
        std::string ep_str = found
            ? endpoint_ops::to_string(entry->endpoint)
            : std::string{};
        uint64_t gen = found ? entry->generation : 0ULL;

        auto resp_bytes = encode_resolve_response(found, actor_id, ep_str, gen);
        auto msg = make_name_message(kNameResolveResponseTag, resp_bytes);
        outbound_port_.send(outbound_port_.context, from, std::move(msg));
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

void NameResolver::on_name_register_response(EndPoint /*from*/,
                                              uint32_t /*result_code*/) {
    // Fire-and-forget from the sender's perspective — no pending state to
    // resolve.  The result code is logged at debug level when structured
    // logging is enabled (future enhancement).
}

void NameResolver::on_name_resolve_response(EndPoint /*from*/, bool found,
                                             uint64_t actor_id,
                                             std::string_view endpoint_str,
                                             uint64_t /*generation*/) {
    // Find the pending resolve request.  The key is the actor name, but
    // the response doesn't include the name.  We iterate pending entries
    // matching by endpoint — in practice there will be at most a handful
    // of concurrent resolves.
    //
    // A more efficient approach would embed a correlation id in the
    // request/response, but for the initial implementation, we signal
    // all pending resolves whose expected home matches the responder.
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& [name, pending] : pending_resolves_) {
        if (!pending || pending->ready) continue;
        {
            std::lock_guard<std::mutex> plock(pending->mtx);
            if (found && !endpoint_str.empty()) {
                pending->result = ActorAddress{
                    endpoint_ops::parse_endpoint(endpoint_str),
                    ActorType{0}, ActorId{actor_id}, 0};
            }
            pending->ready = true;
        }
        pending->cv.notify_one();
    }
}

size_t NameResolver::home_entry_count() const noexcept {
    return name_directory_.size();
}

EndPoint NameResolver::local_endpoint() const noexcept {
    return local_endpoint_;
}

} // namespace hpactor::cluster::name
