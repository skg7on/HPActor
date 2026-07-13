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
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/adt/stream_buffer.hpp>

#include <condition_variable>
#include <memory>
#include <string>
#include <unordered_map>

namespace hpactor::cluster::name {

// ── Protobuf wire-format encoding helpers ────────────────────────────────────
// Mirror the decode helpers in inbound_frame_router.cpp.  These produce the
// raw protobuf bytes for name_directory.proto messages without pulling in
// protobuf codegen (protobuf codegen for name_directory.proto is Task 12).

namespace {

// Wire types (3-bit, lower bits of each tag)
constexpr uint8_t kVarint          = 0;
constexpr uint8_t kLengthDelimited = 2;

// Tag encoding
constexpr uint8_t kVarintContinuationBit = 0x80;
constexpr uint8_t kVarintPayloadMask     = 0x7F;

void append_varint(std::string& out, uint64_t value) {
    while (value >= kVarintContinuationBit) {
        out.push_back(static_cast<uint8_t>((value & kVarintPayloadMask) |
                                            kVarintContinuationBit));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value & kVarintPayloadMask));
}

void append_tag(std::string& out, uint32_t field_number, uint8_t wire_type) {
    append_varint(out, (static_cast<uint64_t>(field_number) << 3) | wire_type);
}

void append_length_delimited(std::string& out, uint32_t field_number,
                             std::string_view data) {
    append_tag(out, field_number, kLengthDelimited);
    append_varint(out, data.size());
    out.append(data);
}

void append_varint_field(std::string& out, uint32_t field_number,
                         uint64_t value) {
    append_tag(out, field_number, kVarint);
    append_varint(out, value);
}

// ── Name-directory message encoders ──────────────────────────────────────

std::string encode_register_request(std::string_view name, uint64_t actor_id,
                                    std::string_view endpoint_str,
                                    uint64_t generation) {
    // PbNameRegisterRequest: name=1, actor_id=2, endpoint=3, generation=4
    std::string out;
    append_length_delimited(out, 1, name);       // field 1: name
    append_varint_field(out, 2, actor_id);       // field 2: actor_id
    append_length_delimited(out, 3, endpoint_str); // field 3: endpoint
    append_varint_field(out, 4, generation);    // field 4: generation
    return out;
}

std::string encode_resolve_query(std::string_view name) {
    // PbNameResolveQuery: name=1
    std::string out;
    append_length_delimited(out, 1, name);       // field 1: name
    return out;
}

std::string encode_unregister_request(std::string_view name,
                                      uint64_t generation) {
    // PbNameUnregisterRequest: name=1, generation=2
    std::string out;
    append_length_delimited(out, 1, name);       // field 1: name
    append_varint_field(out, 2, generation);    // field 2: generation
    return out;
}

std::string encode_register_response(uint32_t result_code) {
    // PbNameRegisterResponse: result=1 (varint enum)
    std::string out;
    append_varint_field(out, 1, result_code);
    return out;
}

std::string encode_resolve_response(bool found, uint64_t actor_id,
                                    std::string_view endpoint_str,
                                    uint64_t generation) {
    // PbNameResolveResponse: found=1, actor_id=2, endpoint=3, generation=4
    std::string out;
    append_varint_field(out, 1, found ? 1ULL : 0ULL); // field 1: found
    if (found) {
        append_varint_field(out, 2, actor_id);     // field 2: actor_id
        append_length_delimited(out, 3, endpoint_str); // field 3: endpoint
        append_varint_field(out, 4, generation);  // field 4: generation
    }
    return out;
}

} // namespace

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
    StreamBuffer payload = StreamBuffer::from_data(
        reinterpret_cast<const uint8_t*>(payload_bytes.data()),
        payload_bytes.size());
    TypedMessage msg(kNameResolveQueryTag, std::move(payload));

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
        StreamBuffer payload = StreamBuffer::from_data(
            reinterpret_cast<const uint8_t*>(payload_bytes.data()),
            payload_bytes.size());
        TypedMessage msg(kNameRegisterRequestTag, std::move(payload));
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
        StreamBuffer payload = StreamBuffer::from_data(
            reinterpret_cast<const uint8_t*>(payload_bytes.data()),
            payload_bytes.size());
        TypedMessage msg(kNameUnregisterRequestTag, std::move(payload));
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
        StreamBuffer payload = StreamBuffer::from_data(
            reinterpret_cast<const uint8_t*>(resp_bytes.data()),
            resp_bytes.size());
        TypedMessage msg(kNameRegisterResponseTag, std::move(payload));
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
        StreamBuffer payload = StreamBuffer::from_data(
            reinterpret_cast<const uint8_t*>(resp_bytes.data()),
            resp_bytes.size());
        TypedMessage msg(kNameResolveResponseTag, std::move(payload));
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
