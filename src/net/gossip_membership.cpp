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

#include <hpactor/net/gossip_membership.hpp>

#include <hpactor/net/udp_transport.hpp>

#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/gossip.pb.h>
#include <hpactor/log/logger.hpp>
#include <hpactor/net/registrar.hpp> // for endpoint_ops, HostResolver

#include <algorithm>
#include <random>

namespace hpactor::net {

// =============================================================================
// Internal helpers (anonymous namespace)
// =============================================================================
namespace {

// ---- Endpoint conversion to/from PbEndpoint ----

void ep_to_pb_endpoint(PbEndpoint* pb, const EndPoint& ep) {
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
        auto* pb4 = pb->mutable_ipv4();
        pb4->set_addr(ipv4->addr);
        pb4->set_port(ipv4->port());
    } else if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&ep)) {
        auto* pb6 = pb->mutable_ipv6();
        pb6->set_addr(
            std::string(reinterpret_cast<const char*>(ipv6->addr.data()), 16));
        pb6->set_port(ipv6->port());
    }
}

EndPoint pb_endpoint_to_ep(const PbEndpoint& pb) {
    if (pb.has_ipv4()) {
        const auto& pb4 = pb.ipv4();
        return Ipv4Endpoint{pb4.addr(), htons(static_cast<uint16_t>(pb4.port()))};
    }
    if (pb.has_ipv6()) {
        const auto& pb6 = pb.ipv6();
        std::array<uint8_t, 16> addr{};
        if (pb6.addr().size() == 16) {
            std::memcpy(addr.data(), pb6.addr().data(), 16);
        }
        return Ipv6Endpoint{addr, htons(static_cast<uint16_t>(pb6.port()))};
    }
    return Ipv4Endpoint{0, 0};
}

// ---- Piggyback entry conversion to/from protobuf ----

void to_pb_piggyback(PbPiggybackEntry* pb, const PiggybackEntry& entry) {
    pb->set_type(static_cast<PbPiggybackType>(entry.type));
    ep_to_pb_endpoint(pb->mutable_endpoint(), entry.identity.endpoint);
    pb->set_incarnation(entry.incarnation);
    for (const auto& s : entry.actor_types)
        pb->add_actor_types(s);
    pb->set_load(entry.load);
    for (const auto& acc : entry.identity.acceptors) {
        auto* a = pb->add_acceptors();
        a->set_port(acc.port);
        a->set_handshake_version(acc.handshake_version);
        a->set_protocol_version(acc.protocol_version);
        a->set_tls_required(acc.tls_required);
    }
}

PiggybackEntry from_pb_piggyback(const PbPiggybackEntry& pb) {
    PiggybackEntry entry;
    entry.type = static_cast<PiggybackType>(pb.type());
    entry.identity.endpoint = pb_endpoint_to_ep(pb.endpoint());
    entry.incarnation = pb.incarnation();
    for (const auto& s : pb.actor_types())
        entry.actor_types.push_back(s);
    entry.load = pb.load();
    for (const auto& a : pb.acceptors()) {
        AcceptorInfo acc;
        acc.port = static_cast<uint16_t>(a.port());
        acc.handshake_version = static_cast<uint8_t>(a.handshake_version());
        acc.protocol_version = static_cast<uint8_t>(a.protocol_version());
        acc.tls_required = a.tls_required();
        entry.identity.acceptors.push_back(acc);
    }
    return entry;
}

// ---- Member conversion to/from protobuf ----

void to_pb_member(PbGossipMember* pb, const Member& m) {
    ep_to_pb_endpoint(pb->mutable_endpoint(), m.identity.endpoint);
    pb->set_host(m.identity.host);
    pb->set_uds_path(m.identity.uds_path);
    pb->set_incarnation(m.incarnation);
    pb->set_status(static_cast<uint32_t>(m.status));
    for (const auto& s : m.actor_types)
        pb->add_actor_types(s);
    for (const auto& acc : m.identity.acceptors) {
        auto* a = pb->add_acceptors();
        a->set_port(acc.port);
        a->set_handshake_version(acc.handshake_version);
        a->set_protocol_version(acc.protocol_version);
        a->set_tls_required(acc.tls_required);
    }
}

Member from_pb_member(const PbGossipMember& pb) {
    Member m;
    m.identity.endpoint = pb_endpoint_to_ep(pb.endpoint());
    m.identity.host = pb.host();
    m.identity.uds_path = pb.uds_path();
    m.incarnation = pb.incarnation();
    m.status = static_cast<MemberStatus>(pb.status());
    for (const auto& s : pb.actor_types())
        m.actor_types.push_back(s);
    for (const auto& a : pb.acceptors()) {
        AcceptorInfo acc;
        acc.port = static_cast<uint16_t>(a.port());
        acc.handshake_version = static_cast<uint8_t>(a.handshake_version());
        acc.protocol_version = static_cast<uint8_t>(a.protocol_version());
        acc.tls_required = a.tls_required();
        m.identity.acceptors.push_back(acc);
    }
    return m;
}

} // anonymous namespace

// =============================================================================
// GossipMembership — Constructor / Destructor
// =============================================================================

GossipMembership::GossipMembership(const GossipConfig& cfg, EventLoop* loop)
    : config_(cfg), loop_(loop),
      transport_(loop ? std::make_unique<RealUdpTransport>(loop)
                      : std::unique_ptr<IUdpTransport>()),
      incarnation_(1) {}

GossipMembership::GossipMembership(const GossipConfig& cfg,
                                   std::unique_ptr<IUdpTransport> transport)
    : config_(cfg), loop_(nullptr), transport_(std::move(transport)),
      incarnation_(1) {}

GossipMembership::~GossipMembership() {
    stop();
}

// =============================================================================
// IServiceDiscovery — lifecycle
// =============================================================================

void GossipMembership::start() {
    // Incarnation is a wall-clock timestamp so that restarted nodes
    // automatically have a higher incarnation than any previous run.
    incarnation_ = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());

    if (transport_) {
        if (!transport_->bind(config_.gossip_port)) {
            HPACTOR_LOG_ERROR(
                log::LogCategory::kDiscovery, ActorId{0},
                static_cast<uint32_t>(log::LogEventId::kDiscoveryNodeDead),
                "gossip bind failed");
            return;
        }
        transport_->set_receive_callback(
            [this](const StreamBuffer& data, const std::string& host,
                   uint16_t port) { handle_packet(data, host, port); });
    }

    // Add self to the membership table.
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        Member self = config_.local_state;
        self.incarnation = incarnation_;
        self.status = MemberStatus::Alive;
        self.last_seen = std::chrono::steady_clock::now();
        members_[self.identity.endpoint] = std::move(self);
    }

    // Bootstrap: join an existing cluster via a seed, or start a solo cluster.
    if (!config_.seeds.empty()) {
        join_seed_index_ = 0;
        send_join(config_.seeds[0]);

        // Recursive retry chain: try the next seed after 1 s if no SyncRsp
        // arrived.
        auto retry_fn = std::make_shared<std::function<void()>>();
        *retry_fn = [this, retry_fn]() {
            join_seed_index_++;
            if (join_seed_index_ >= config_.seeds.size()) {
                return; // No more seeds to try
            }
            {
                std::shared_lock<std::shared_mutex> lock(members_mutex_);
                if (members_.size() > 1) {
                    return; // Already received SyncRsp — joined successfully
                }
            }
            send_join(config_.seeds[join_seed_index_]);
            join_retry_timer_ = loop_->run_after(*retry_fn, 1000);
        };
        join_retry_timer_ = loop_->run_after(*retry_fn, 1000);
    }

    // Seed the per-instance RNG.
    if (!rng_seeded_) {
        std::random_device rd;
        rng_.seed(rd());
        rng_seeded_ = true;
    }

    // Schedule the periodic protocol round.
    protocol_timer_ =
        loop_->run_every([this] { protocol_round(); },
                         static_cast<int>(config_.protocol_period.count()));
}

void GossipMembership::stop() {
    // Cancel the periodic protocol timer.
    if (protocol_timer_ != 0 && loop_) {
        loop_->cancel_timer(protocol_timer_);
        protocol_timer_ = 0;
    }

    // Cancel the join retry timer if still active.
    if (join_retry_timer_ != 0 && loop_) {
        loop_->cancel_timer(join_retry_timer_);
        join_retry_timer_ = 0;
    }

    // Send Leave to all known Alive + Suspicious members (best-effort).
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);
        for (const auto& [ep, member] : members_) {
            if (member.status == MemberStatus::Alive ||
                member.status == MemberStatus::Suspicious) {
                send_leave(ep);
            }
        }
    }

    // Tear down the UDP transport.
    if (transport_) {
        transport_->close();
    }

    // Clear local state.
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        members_.clear();
        pending_pings_.clear();
        forwarded_pings_.clear();
    }
}

// =============================================================================
// IServiceDiscovery — query
// =============================================================================

std::vector<Member> GossipMembership::discover_all() const {
    std::shared_lock<std::shared_mutex> lock(members_mutex_);
    std::vector<Member> result;
    result.reserve(members_.size());
    for (const auto& [ep, member] : members_) {
        result.push_back(member);
    }
    return result;
}

const Member* GossipMembership::discover(EndPoint ep) const {
    std::shared_lock<std::shared_mutex> lock(members_mutex_);
    auto it = members_.find(ep);
    if (it != members_.end()) {
        return &it->second;
    }
    return nullptr;
}

// =============================================================================
// IServiceDiscovery — announce / callback
// =============================================================================

void GossipMembership::announce(Member local_state) {
    std::unique_lock<std::shared_mutex> lock(members_mutex_);
    // Bump incarnation on re-announce
    ++incarnation_;
    local_state.incarnation = incarnation_;
    local_state.last_seen = std::chrono::steady_clock::now();
    config_.local_state = std::move(local_state);
    needs_dissemination_ = true;
}

void GossipMembership::on_member_change(MemberChangeCallback cb) {
    member_change_cb_ = std::move(cb);
}

// =============================================================================
// Wire protocol — encode_message
// =============================================================================
//
// Binary format:
//   Magic (4B "HPGC") | Version (1B) | Type (1B) | Flags (2B) |
//   Sender Endpoint (len-prefixed, var) |
//   Incarnation (8B BE) | Sequence Number (4B BE) |
//   [Ping Target Endpoint (len-prefixed, var) — PingReq only] |
//   Piggyback Count (2B BE) | Piggyback entries...

StreamBuffer
GossipMembership::encode_message(GossipMessageType type, uint64_t inc,
                                 uint32_t seq, EndPoint ping_target,
                                 const std::vector<PiggybackEntry>& pb) const {
    PbGossipEnvelope env;
    env.set_magic(GossipMagic);
    env.set_version(GossipVersion);
    env.set_type(static_cast<PbGossipMessageType>(type));
    env.set_flags(0);

    // Build per-type payload
    switch (type) {
        case GossipMessageType::Ping: {
            auto* ping = env.mutable_ping();
            ep_to_pb_endpoint(ping->mutable_sender_endpoint(),
                              config_.local_state.identity.endpoint);
            ping->set_incarnation(inc);
            ping->set_seq_no(seq);
            for (const auto& e : pb)
                to_pb_piggyback(ping->add_piggyback(), e);
            break;
        }
        case GossipMessageType::Ack: {
            auto* ack = env.mutable_ack();
            ep_to_pb_endpoint(ack->mutable_sender_endpoint(),
                              config_.local_state.identity.endpoint);
            ack->set_incarnation(inc);
            for (const auto& e : pb)
                to_pb_piggyback(ack->add_piggyback(), e);
            break;
        }
        case GossipMessageType::PingReq: {
            auto* pr = env.mutable_ping_req();
            ep_to_pb_endpoint(pr->mutable_sender_endpoint(),
                              config_.local_state.identity.endpoint);
            ep_to_pb_endpoint(pr->mutable_target_endpoint(), ping_target);
            pr->set_incarnation(inc);
            break;
        }
        case GossipMessageType::IndirectAck: {
            auto* ia = env.mutable_indirect_ack();
            ep_to_pb_endpoint(ia->mutable_sender_endpoint(),
                              config_.local_state.identity.endpoint);
            ep_to_pb_endpoint(ia->mutable_original_requester(), ping_target);
            ia->set_incarnation(inc);
            break;
        }
        case GossipMessageType::Join: {
            auto* join = env.mutable_join();
            ep_to_pb_endpoint(join->mutable_sender_endpoint(),
                              config_.local_state.identity.endpoint);
            join->set_incarnation(inc);
            join->set_host(config_.local_state.identity.host);
            join->set_uds_path(config_.local_state.identity.uds_path);
            for (const auto& s : config_.local_state.actor_types)
                join->add_actor_types(s);
            for (const auto& acc : config_.local_state.identity.acceptors) {
                auto* a = join->add_acceptors();
                a->set_port(acc.port);
                a->set_handshake_version(acc.handshake_version);
                a->set_protocol_version(acc.protocol_version);
                a->set_tls_required(acc.tls_required);
            }
            break;
        }
        case GossipMessageType::Leave: {
            auto* leave = env.mutable_leave();
            ep_to_pb_endpoint(leave->mutable_sender_endpoint(),
                              config_.local_state.identity.endpoint);
            leave->set_incarnation(inc);
            break;
        }
        default:
            break;
    }

    std::string serialized = env.SerializeAsString();
    return StreamBuffer(serialized.begin(), serialized.end());
}

// =============================================================================
// Wire protocol — decode_message
// =============================================================================

bool GossipMembership::decode_message(const StreamBuffer& data,
                                      GossipMessageType& type, EndPoint& sender,
                                      uint64_t& inc, uint32_t& seq,
                                      EndPoint& ping_target,
                                      std::vector<PiggybackEntry>& pb) const {
    PbGossipEnvelope env;
    if (!env.ParseFromArray(data.data(), static_cast<int>(data.size())))
        return false;
    if (env.magic() != GossipMagic || env.version() != GossipVersion)
        return false;

    type = static_cast<GossipMessageType>(env.type());
    pb.clear();

    switch (type) {
        case GossipMessageType::Ping:
            if (!env.has_ping())
                return false;
            sender = pb_endpoint_to_ep(env.ping().sender_endpoint());
            inc = env.ping().incarnation();
            seq = env.ping().seq_no();
            for (const auto& e : env.ping().piggyback())
                pb.push_back(from_pb_piggyback(e));
            break;
        case GossipMessageType::Ack:
            if (!env.has_ack())
                return false;
            sender = pb_endpoint_to_ep(env.ack().sender_endpoint());
            inc = env.ack().incarnation();
            seq = 0;
            for (const auto& e : env.ack().piggyback())
                pb.push_back(from_pb_piggyback(e));
            break;
        case GossipMessageType::PingReq:
            if (!env.has_ping_req())
                return false;
            sender = pb_endpoint_to_ep(env.ping_req().sender_endpoint());
            ping_target = pb_endpoint_to_ep(env.ping_req().target_endpoint());
            inc = env.ping_req().incarnation();
            seq = 0;
            break;
        case GossipMessageType::IndirectAck:
            if (!env.has_indirect_ack())
                return false;
            sender = pb_endpoint_to_ep(env.indirect_ack().sender_endpoint());
            ping_target =
                pb_endpoint_to_ep(env.indirect_ack().original_requester());
            inc = env.indirect_ack().incarnation();
            seq = 0;
            break;
        case GossipMessageType::Join:
            if (!env.has_join())
                return false;
            sender = pb_endpoint_to_ep(env.join().sender_endpoint());
            inc = env.join().incarnation();
            seq = 0;
            break;
        case GossipMessageType::SyncRsp:
            // SyncRsp is handled by decode_sync_rsp(), not this path
            return false;
        case GossipMessageType::Leave:
            if (!env.has_leave())
                return false;
            sender = pb_endpoint_to_ep(env.leave().sender_endpoint());
            inc = env.leave().incarnation();
            seq = 0;
            break;
    }

    return true;
}

// =============================================================================
// Wire protocol — encode_sync_rsp (protobuf)
// =============================================================================

StreamBuffer
GossipMembership::encode_sync_rsp(const std::vector<Member>& members) const {
    PbGossipSyncRsp rsp;
    ep_to_pb_endpoint(rsp.mutable_sender_endpoint(),
                      config_.local_state.identity.endpoint);
    rsp.set_incarnation(incarnation_);
    for (const auto& m : members) {
        to_pb_member(rsp.add_members(), m);
    }
    std::string serialized = rsp.SerializeAsString();
    return StreamBuffer(serialized.begin(), serialized.end());
}

// =============================================================================
// Wire protocol — decode_sync_rsp (protobuf)
// =============================================================================

bool GossipMembership::decode_sync_rsp(const StreamBuffer& data,
                                       std::vector<Member>& members) const {
    PbGossipSyncRsp rsp;
    if (!rsp.ParseFromArray(data.data(), static_cast<int>(data.size())))
        return false;

    members.clear();
    members.reserve(static_cast<size_t>(rsp.members_size()));
    for (const auto& pb_m : rsp.members()) {
        Member m = from_pb_member(pb_m);
        m.last_seen = std::chrono::steady_clock::now();
        members.push_back(std::move(m));
    }
    return true;
}

// =============================================================================
// Internal piggyback builder (called from member functions)
// =============================================================================

// This helper is used by protocol_round(), handle_ping(), and send methods to
// assemble outgoing piggyback entries.  It reads members_, config_, and
// needs_dissemination_ directly.  The caller is responsible for clearing
// needs_dissemination_ after the piggyback is serialised.

static std::vector<PiggybackEntry>
build_piggyback_impl(const GossipConfig& config, uint64_t incarnation,
                     bool& needs_dissemination,
                     const std::unordered_map<EndPoint, Member>& members) {
    std::vector<PiggybackEntry> entries;

    // Self metadata — disseminate when announce() has been called.
    if (needs_dissemination) {
        PiggybackEntry meta;
        meta.type = PiggybackType::Metadata;
        meta.identity.endpoint = config.local_state.identity.endpoint;
        meta.incarnation = incarnation;
        meta.actor_types = config.local_state.actor_types;
        meta.load = 0;
        meta.identity.acceptors = config.local_state.identity.acceptors;
        entries.push_back(std::move(meta));
    }

    // Piggyback all Suspicious and Dead members so the cluster converges
    // quickly.
    for (const auto& [ep, m] : members) {
        if (m.status == MemberStatus::Suspicious) {
            PiggybackEntry e;
            e.type = PiggybackType::Suspicious;
            e.identity.endpoint = ep;
            e.incarnation = m.incarnation;
            entries.push_back(std::move(e));
        } else if (m.status == MemberStatus::Dead) {
            PiggybackEntry e;
            e.type = PiggybackType::Dead;
            e.identity.endpoint = ep;
            e.incarnation = m.incarnation;
            entries.push_back(std::move(e));
        }
    }

    return entries;
}

// =============================================================================
// Protocol round
// =============================================================================

void GossipMembership::protocol_round() {
    FAULT_INJECT("hpactor.gossip.protocol_round.delay") {
        _fc->stall(hpactor::fault::FaultDomain::kGossip, 1);
    }
    auto now = std::chrono::steady_clock::now();

    // ── 1. Check pending pings ─────────────────────────────────────────────
    // Must run before sending new pings so that expired pings are not
    // overwritten by fresh ones in step 2.
    // Collect expired pings under lock, then release before calling
    // send_ping_req() and mark_suspicious() which acquire their own
    // members_mutex_ lock.
    std::vector<EndPoint> expired_targets_suspicious;
    std::vector<std::pair<EndPoint, EndPoint>> ping_reqs; // (proxy, target)
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);

        std::vector<EndPoint> expired_pings;
        for (auto& [ep, pp] : pending_pings_) {
            if (now >= pp.expires_at) {
                expired_pings.push_back(ep);
            }
        }

        for (const auto& target : expired_pings) {
            auto it = pending_pings_.find(target);
            if (it == pending_pings_.end())
                continue;
            auto& pp = it->second;

            if (!pp.indirect_requested) {
                // First expiry — request indirect probes.
                pp.indirect_requested = true;
                pp.indirect_expires_at = now + config_.ping_timeout;

                // Pick indirect probe peers: Alive, not self, not the target.
                std::unordered_set<EndPoint> exclude{
                    target, config_.local_state.identity.endpoint};
                auto probes = pick_random_peers(config_.indirect_probes, exclude);

                if (probes.empty()) {
                    // 2-node cluster or no other peers available — skip
                    // indirect probes and immediately mark suspicious.
                    pending_pings_.erase(target);
                    expired_targets_suspicious.push_back(target);
                } else {
                    for (const auto& proxy : probes) {
                        ping_reqs.emplace_back(proxy, target);
                    }
                }
            } else {
                // Second expiry — indirect probes did not succeed.
                pending_pings_.erase(it);
                expired_targets_suspicious.push_back(target);
            }
        }
    } // Release lock before calling send_ping_req() / mark_suspicious()

    for (const auto& [proxy, target] : ping_reqs) {
        send_ping_req(proxy, target);
    }

    for (const auto& target : expired_targets_suspicious) {
        mark_suspicious(target);
    }

    // ── 2. Pick peers and send Pings ──────────────────────────────────────
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);

        // Collect Alive members excluding self.
        std::vector<EndPoint> alive;
        for (const auto& [ep, m] : members_) {
            if (m.status == MemberStatus::Alive &&
                !(ep == config_.local_state.identity.endpoint)) {
                alive.push_back(ep);
            }
        }

        if (!alive.empty()) {
            auto targets = pick_random_peers(config_.fanout, {});
            // pick_random_peers already handles the alive filtering; but it
            // uses the shared_lock we already hold.  To avoid double-locking
            // we build the targets list above and pass it directly.

            // Build piggyback entries for outgoing Pings.
            auto pb = build_piggyback_impl(config_, incarnation_,
                                           needs_dissemination_, members_);
            // Lock must be held while reading members_ for piggyback.

            for (const auto& target : targets) {
                StreamBuffer msg =
                    encode_message(GossipMessageType::Ping, incarnation_, seq_no_++,
                                   config_.local_state.identity.endpoint, pb);
                if (transport_)
                    transport_->send(msg, target);

                // Record or refresh pending ping.  Preserve indirect probe
                // state if one was already in flight (set by step 1 above).
                // pending_pings_ is protected by members_mutex_ in our design.
                auto it = pending_pings_.find(target);
                if (it != pending_pings_.end()) {
                    it->second.expires_at = now + config_.ping_timeout;
                } else {
                    PendingPing pp;
                    pp.expires_at = now + config_.ping_timeout;
                    pp.indirect_requested = false;
                    pending_pings_[target] = pp;
                }
            }

            // Clear dissemination flag now that we've piggybacked our metadata.
            if (needs_dissemination_ && !pb.empty()) {
                needs_dissemination_ = false;
            }
        }
    } // shared_lock released

    // ── 3. Check suspicious members for timeout ────────────────────────────
    // Collect expired suspicious members under lock, then release before
    // calling mark_dead() which acquires its own members_mutex_ lock.
    std::vector<EndPoint> to_mark_dead;
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        for (auto& [ep, m] : members_) {
            if (m.status == MemberStatus::Suspicious) {
                if (now - m.last_seen > config_.suspicion_timeout) {
                    to_mark_dead.push_back(ep);
                }
            }
        }
    } // Release lock before calling mark_dead()

    for (const auto& ep : to_mark_dead) {
        mark_dead(ep);
    }

    // ── 4. Purge old tombstones ────────────────────────────────────────────
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        purge_dead_tombstones();
    }
}

// =============================================================================
// Packet handler — non-blocking recvfrom loop
// =============================================================================

void GossipMembership::handle_packet(const StreamBuffer& data,
                                     const std::string& from_host,
                                     uint16_t from_port) {
    GossipMessageType type;
    EndPoint sender;
    uint64_t inc;
    uint32_t seq;
    EndPoint ping_target;
    std::vector<PiggybackEntry> pb;

    if (!decode_message(data, type, sender, inc, seq, ping_target, pb)) {
        // Try SyncRsp format (uses a separate wire format from standard
        // messages)
        std::vector<Member> members;
        if (decode_sync_rsp(data, members)) {
            handle_sync_rsp(std::move(members));
        }
        return;
    }

    switch (type) {
        case GossipMessageType::Ping:
            handle_ping(sender, inc, seq, std::move(pb), from_host, from_port);
            break;
        case GossipMessageType::Ack:
            handle_ack(sender, inc, std::move(pb));
            break;
        case GossipMessageType::PingReq:
            handle_ping_req(sender, ping_target);
            break;
        case GossipMessageType::IndirectAck:
            handle_indirect_ack(sender, ping_target);
            break;
        case GossipMessageType::Join:
            handle_join(sender, inc, std::move(pb));
            break;
        case GossipMessageType::Leave:
            handle_leave(sender, inc);
            break;
        default:
            break;
    }
}

// =============================================================================
// Message handlers
// =============================================================================

void GossipMembership::handle_ping(EndPoint sender, uint64_t inc, uint32_t /*seq*/,
                                   std::vector<PiggybackEntry> pb,
                                   const std::string& /*host*/, uint16_t /*port*/) {
    // Merge the sender — a Ping proves they are alive.
    {
        Member m;
        m.identity.endpoint = sender;
        m.incarnation = inc;
        m.status = MemberStatus::Alive;
        m.last_seen = std::chrono::steady_clock::now();
        merge_member(m);
    }

    // Apply piggyback entries from the ping.
    apply_piggyback(pb);

    // If we had a pending ping for the sender, clear it — they just proved
    // they are alive by pinging us.
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        pending_pings_.erase(sender);
    }

    // Build piggyback for the Ack response.
    std::vector<PiggybackEntry> ack_pb;
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);
        ack_pb = build_piggyback_impl(config_, incarnation_,
                                      needs_dissemination_, members_);
    }
    if (!ack_pb.empty()) {
        needs_dissemination_ = false;
    }

    // Send Ack with piggyback.
    StreamBuffer ack_msg =
        encode_message(GossipMessageType::Ack, incarnation_, seq_no_++,
                       config_.local_state.identity.endpoint, ack_pb);
    if (transport_)
        transport_->send(ack_msg, sender);
}

void GossipMembership::handle_ack(EndPoint sender, uint64_t inc,
                                  std::vector<PiggybackEntry> pb) {
    // Merge the sender — an Ack proves they are alive.
    {
        Member m;
        m.identity.endpoint = sender;
        m.incarnation = inc;
        m.status = MemberStatus::Alive;
        m.last_seen = std::chrono::steady_clock::now();
        merge_member(m);
    }

    // Check if this ack is for a forwarded PingReq.
    auto fwd_it = forwarded_pings_.find(sender);
    if (fwd_it != forwarded_pings_.end()) {
        // We forwarded a Ping on behalf of fwd_it->second.
        // The target (sender) responded, so send IndirectAck to the original
        // requester.
        send_indirect_ack(fwd_it->second, sender);
        forwarded_pings_.erase(fwd_it);
    }

    // Clear any pending ping for this sender — the probe succeeded.
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        pending_pings_.erase(sender);
    }

    // Apply piggyback entries.
    apply_piggyback(pb);
}

void GossipMembership::handle_ping_req(EndPoint sender, EndPoint target) {
    // Merge the requester.
    {
        Member m;
        m.identity.endpoint = sender;
        m.status = MemberStatus::Alive;
        m.last_seen = std::chrono::steady_clock::now();
        merge_member(m);
    }

    // Forward the ping on behalf of the requester.
    forwarded_pings_[target] = sender;

    // Build piggyback and send Ping to target.
    std::vector<PiggybackEntry> pb;
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);
        pb = build_piggyback_impl(config_, incarnation_, needs_dissemination_,
                                  members_);
    }

    StreamBuffer msg =
        encode_message(GossipMessageType::Ping, incarnation_, seq_no_++,
                       config_.local_state.identity.endpoint, pb);
    if (transport_)
        transport_->send(msg, target);
}

void GossipMembership::handle_indirect_ack(EndPoint sender, EndPoint target) {
    // Merge the indirect ack sender.
    {
        Member m;
        m.identity.endpoint = sender;
        m.status = MemberStatus::Alive;
        m.last_seen = std::chrono::steady_clock::now();
        merge_member(m);
    }

    // If we have a pending ping for target with indirect_requested=true,
    // the indirect probe succeeded — clear the pending ping.
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        auto it = pending_pings_.find(target);
        if (it != pending_pings_.end() && it->second.indirect_requested) {
            pending_pings_.erase(it);
        }
    }
}

void GossipMembership::handle_join(EndPoint sender, uint64_t inc,
                                   std::vector<PiggybackEntry> pb) {
    // Merge the joining node.
    {
        Member m;
        m.identity.endpoint = sender;
        m.incarnation = inc;
        m.status = MemberStatus::Alive;
        m.last_seen = std::chrono::steady_clock::now();
        merge_member(m);
        HPACTOR_LOG_INFO(log::LogCategory::kDiscovery, ActorId{0},
                         static_cast<uint32_t>(log::LogEventId::kDiscoveryNodeJoined),
                         "discovery node joined");
    }

    apply_piggyback(pb);

    // Send full membership table (excluding Dead and Left) as SyncRsp.
    send_sync_rsp(sender);
}

void GossipMembership::handle_sync_rsp(std::vector<Member> members) {
    FAULT_INJECT("hpactor.gossip.sync_rsp.corrupt") {
        if (!members.empty()) {
            members[0].status = MemberStatus::Dead;
        }
    }
    // Merge all received members into our table.
    for (auto& m : members) {
        merge_member(m);
    }

    // Cancel any pending join retry — we have successfully joined.
    if (join_retry_timer_ != 0 && loop_) {
        loop_->cancel_timer(join_retry_timer_);
        join_retry_timer_ = 0;
    }

    // Ensure self is still in the table (should always be, but be defensive).
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        if (members_.find(config_.local_state.identity.endpoint) == members_.end()) {
            Member self = config_.local_state;
            self.incarnation = incarnation_;
            self.status = MemberStatus::Alive;
            self.last_seen = std::chrono::steady_clock::now();
            members_[self.identity.endpoint] = std::move(self);
        }
    }
}

void GossipMembership::handle_leave(EndPoint sender, uint64_t inc) {
    Member member_to_fire;
    bool should_fire = false;

    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        auto it = members_.find(sender);
        if (it != members_.end()) {
            // Only accept if incarnation is >= what we have.
            if (inc >= it->second.incarnation) {
                it->second.status = MemberStatus::Left;
                it->second.last_seen = std::chrono::steady_clock::now();
                if (member_change_cb_) {
                    member_to_fire = it->second;
                    should_fire = true;
                }
            }
        }
    }

    if (should_fire) {
        member_change_cb_(member_to_fire, false);
    }
}

// =============================================================================
// Message sending
// =============================================================================

void GossipMembership::send_ping(EndPoint target) {
    FAULT_INJECT("hpactor.gossip.ping.drop") {
        return;
    }
    std::vector<PiggybackEntry> pb;
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);
        pb = build_piggyback_impl(config_, incarnation_, needs_dissemination_,
                                  members_);
    }
    StreamBuffer msg =
        encode_message(GossipMessageType::Ping, incarnation_, seq_no_++,
                       config_.local_state.identity.endpoint, pb);
    if (transport_)
        transport_->send(msg, target);
}

void GossipMembership::send_ack(EndPoint target, std::vector<PiggybackEntry> pb) {
    FAULT_INJECT("hpactor.gossip.ack.drop") {
        return;
    }
    StreamBuffer msg =
        encode_message(GossipMessageType::Ack, incarnation_, seq_no_++,
                       config_.local_state.identity.endpoint, pb);
    if (transport_)
        transport_->send(msg, target);
}

void GossipMembership::send_ping_req(EndPoint proxy, EndPoint target) {
    // Build minimal piggyback for PingReq.
    std::vector<PiggybackEntry> pb;
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);
        pb = build_piggyback_impl(config_, incarnation_, needs_dissemination_,
                                  members_);
    }
    StreamBuffer msg = encode_message(GossipMessageType::PingReq, incarnation_,
                                      seq_no_++, target, pb);
    if (transport_)
        transport_->send(msg, proxy);
}

void GossipMembership::send_indirect_ack(EndPoint target, EndPoint orig_target) {
    // IndirectAck carries the original target in the ping_target field.
    std::vector<PiggybackEntry> pb; // empty piggyback for indirect ack
    StreamBuffer msg = encode_message(GossipMessageType::IndirectAck,
                                      incarnation_, seq_no_++, orig_target, pb);
    if (transport_)
        transport_->send(msg, target);
}

void GossipMembership::send_join(EndPoint seed) {
    FAULT_INJECT("hpactor.gossip.join.drop") {
        return;
    }
    // Join includes self metadata as piggyback.
    std::vector<PiggybackEntry> pb;
    {
        PiggybackEntry meta;
        meta.type = PiggybackType::Metadata;
        meta.identity.endpoint = config_.local_state.identity.endpoint;
        meta.incarnation = incarnation_;
        meta.actor_types = config_.local_state.actor_types;
        meta.identity.acceptors = config_.local_state.identity.acceptors;
        pb.push_back(std::move(meta));
    }
    StreamBuffer msg =
        encode_message(GossipMessageType::Join, incarnation_, seq_no_++,
                       config_.local_state.identity.endpoint, pb);
    if (transport_)
        transport_->send(msg, seed);
}

void GossipMembership::send_sync_rsp(EndPoint target) {
    // Collect non-Dead, non-Left members for the sync response.
    std::vector<Member> table;
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);
        for (const auto& [ep, m] : members_) {
            if (m.status != MemberStatus::Dead && m.status != MemberStatus::Left) {
                table.push_back(m);
            }
        }
    }
    StreamBuffer data = encode_sync_rsp(table);
    if (transport_)
        transport_->send(data, target);
}

void GossipMembership::send_leave(EndPoint target) {
    FAULT_INJECT("hpactor.gossip.leave.drop") {
        return;
    }
    std::vector<PiggybackEntry> pb; // empty piggyback for leave
    StreamBuffer msg =
        encode_message(GossipMessageType::Leave, incarnation_, seq_no_++,
                       config_.local_state.identity.endpoint, pb);
    if (transport_)
        transport_->send(msg, target);
}

// =============================================================================
// State mutation
// =============================================================================

void GossipMembership::mark_suspicious(EndPoint ep) {
    FAULT_INJECT("hpactor.gossip.mark_suspicious.drop") {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(members_mutex_);
    auto it = members_.find(ep);
    if (it != members_.end()) {
        it->second.status = MemberStatus::Suspicious;
        it->second.last_seen = std::chrono::steady_clock::now();
        HPACTOR_LOG_WARNING(log::LogCategory::kDiscovery, ActorId{0}, 0,
                            "discovery node suspected");
    }
}

void GossipMembership::mark_dead(EndPoint ep) {
    FAULT_INJECT("hpactor.gossip.mark_dead.drop") {
        return;
    }
    Member member_to_fire;
    bool should_fire = false;

    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        auto it = members_.find(ep);
        if (it != members_.end()) {
            it->second.status = MemberStatus::Dead;
            it->second.last_seen = std::chrono::steady_clock::now();
            HPACTOR_LOG_ERROR(
                log::LogCategory::kDiscovery, ActorId{0},
                static_cast<uint32_t>(log::LogEventId::kDiscoveryNodeDead),
                "discovery node dead");
            if (member_change_cb_) {
                member_to_fire = it->second;
                should_fire = true;
            }
        }
    }

    if (should_fire) {
        member_change_cb_(member_to_fire, false);
    }
}

void GossipMembership::merge_member(const Member& remote) {
    FAULT_INJECT("hpactor.gossip.merge_member.corrupt") {
        const_cast<Member&>(remote).incarnation += 100;
    }
    std::unique_lock<std::shared_mutex> lock(members_mutex_);

    auto it = members_.find(remote.identity.endpoint);
    if (it == members_.end()) {
        // First time seeing this endpoint — insert.
        members_[remote.identity.endpoint] = remote;
        members_[remote.identity.endpoint].last_seen =
            std::chrono::steady_clock::now();
        return;
    }

    auto& existing = it->second;

    // Only accept updates with a strictly higher incarnation.
    if (remote.incarnation <= existing.incarnation) {
        return; // Stale information
    }

    // Higher incarnation: accept the update.
    // Special case: Dead member with higher incarnation → reactivate.
    if (existing.status == MemberStatus::Dead &&
        remote.incarnation > existing.incarnation) {
        // The node restarted with a higher incarnation — reactivate to Alive.
        existing.status = MemberStatus::Alive;
    }

    // Update fields from remote that carry meaningful information.
    existing.incarnation = remote.incarnation;
    if (remote.status != MemberStatus::Alive ||
        existing.status == MemberStatus::Dead) {
        // Accept status from remote unless we already have it as Dead and it's
        // not a reactivation.
    }
    if (remote.status == MemberStatus::Suspicious ||
        remote.status == MemberStatus::Dead || remote.status == MemberStatus::Left) {
        existing.status = remote.status;
    }

    // Accept metadata if present.
    if (!remote.actor_types.empty()) {
        existing.actor_types = remote.actor_types;
    }
    if (!remote.identity.host.empty()) {
        existing.identity.host = remote.identity.host;
    }
    if (!remote.identity.acceptors.empty()) {
        existing.identity.acceptors = remote.identity.acceptors;
    }

    existing.last_seen = std::chrono::steady_clock::now();
}

void GossipMembership::apply_piggyback(const std::vector<PiggybackEntry>& entries) {
    std::unique_lock<std::shared_mutex> lock(members_mutex_);

    for (const auto& entry : entries) {
        auto it = members_.find(entry.identity.endpoint);
        if (it == members_.end()) {
            // We don't know this endpoint — insert it with the piggyback info.
            Member m;
            m.identity.endpoint = entry.identity.endpoint;
            m.incarnation = entry.incarnation;
            m.last_seen = std::chrono::steady_clock::now();

            switch (entry.type) {
                case PiggybackType::Alive:
                    m.status = MemberStatus::Alive;
                    break;
                case PiggybackType::Suspicious:
                    m.status = MemberStatus::Suspicious;
                    break;
                case PiggybackType::Dead:
                    m.status = MemberStatus::Dead;
                    break;
                case PiggybackType::Metadata:
                    m.status = MemberStatus::Alive;
                    m.actor_types = entry.actor_types;
                    m.identity.acceptors = entry.identity.acceptors;
                    break;
            }
            members_[entry.identity.endpoint] = std::move(m);
            continue;
        }

        auto& existing = it->second;

        // Only apply if the piggyback incarnation is >= what we have.
        if (entry.incarnation < existing.incarnation) {
            continue; // Stale
        }

        existing.incarnation = entry.incarnation;
        existing.last_seen = std::chrono::steady_clock::now();

        switch (entry.type) {
            case PiggybackType::Alive:
                if (existing.status == MemberStatus::Suspicious ||
                    existing.status == MemberStatus::Dead) {
                    existing.status = MemberStatus::Alive;
                }
                break;
            case PiggybackType::Suspicious:
                existing.status = MemberStatus::Suspicious;
                break;
            case PiggybackType::Dead:
                existing.status = MemberStatus::Dead;
                break;
            case PiggybackType::Metadata:
                if (!entry.actor_types.empty()) {
                    existing.actor_types = entry.actor_types;
                }
                if (!entry.identity.acceptors.empty()) {
                    existing.identity.acceptors = entry.identity.acceptors;
                }
                break;
        }
    }
}

void GossipMembership::purge_dead_tombstones() {
    auto now = std::chrono::steady_clock::now();

    for (auto it = members_.begin(); it != members_.end();) {
        const auto& m = it->second;

        if ((m.status == MemberStatus::Dead || m.status == MemberStatus::Left) &&
            now - m.last_seen > config_.dead_timeout) {
            it = members_.erase(it);
            HPACTOR_LOG_DEBUG(log::LogCategory::kDiscovery, ActorId{0}, 0,
                              "discovery cache purged");
        } else {
            ++it;
        }
    }
}

std::vector<EndPoint>
GossipMembership::pick_random_peers(size_t count,
                                    std::unordered_set<EndPoint> exclude) {
    FAULT_INJECT("hpactor.gossip.pick_random_peers.fail") {
        return {};
    }

    // Collect Alive peers excluding self and explicitly excluded endpoints.
    // Caller holds members_mutex_ — do not re-acquire.
    std::vector<EndPoint> candidates;
    for (const auto& [ep, m] : members_) {
        if (m.status != MemberStatus::Alive)
            continue;
        if (ep == config_.local_state.identity.endpoint)
            continue; // exclude self
        if (exclude.find(ep) != exclude.end())
            continue;
        candidates.push_back(ep);
    }

    if (candidates.empty())
        return {};
    if (candidates.size() <= count)
        return candidates;

    // Shuffle and pick the first 'count' elements.
    std::shuffle(candidates.begin(), candidates.end(), rng_);
    candidates.resize(count);
    return candidates;
}

} // namespace hpactor::net
