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

#include <hpactor/gossip.pb.h>
#include <hpactor/log/logger.hpp>
#include <hpactor/net/registrar.hpp> // for endpoint_ops, HostResolver

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
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
    ep_to_pb_endpoint(pb->mutable_endpoint(), entry.endpoint);
    pb->set_incarnation(entry.incarnation);
    for (const auto& s : entry.actor_types)
        pb->add_actor_types(s);
    pb->set_load(entry.load);
    for (const auto& acc : entry.acceptors) {
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
    entry.endpoint = pb_endpoint_to_ep(pb.endpoint());
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
        entry.acceptors.push_back(acc);
    }
    return entry;
}

// ---- Member conversion to/from protobuf ----

void to_pb_member(PbGossipMember* pb, const Member& m) {
    ep_to_pb_endpoint(pb->mutable_endpoint(), m.endpoint);
    pb->set_host(m.host);
    pb->set_uds_path(m.uds_path);
    pb->set_incarnation(m.incarnation);
    pb->set_status(static_cast<uint32_t>(m.status));
    for (const auto& s : m.actor_types)
        pb->add_actor_types(s);
    for (const auto& acc : m.acceptors) {
        auto* a = pb->add_acceptors();
        a->set_port(acc.port);
        a->set_handshake_version(acc.handshake_version);
        a->set_protocol_version(acc.protocol_version);
        a->set_tls_required(acc.tls_required);
    }
}

Member from_pb_member(const PbGossipMember& pb) {
    Member m;
    m.endpoint = pb_endpoint_to_ep(pb.endpoint());
    m.host = pb.host();
    m.uds_path = pb.uds_path();
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
        m.acceptors.push_back(acc);
    }
    return m;
}

// ---- Async UDP send via EventLoop ----
// Uses the EventLoop's async_sendto (non-blocking, completion-driven).
// Falls back to blocking sendto when no EventLoop is available.

void async_udp_send(EventLoop* loop, int sock, const StreamBuffer& data,
                    const EndPoint& dest) {
    if (sock < 0 || data.empty())
        return;

    if (loop) {
        struct iovec iov;
        iov.iov_base = const_cast<uint8_t*>(data.data());
        iov.iov_len = data.size();

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&dest)) {
            addr.sin_addr.s_addr = ipv4->addr;
            addr.sin_port = ipv4->port_nw;
        }

        loop->backend()->async_sendto(
            sock, &iov, 1, reinterpret_cast<const sockaddr*>(&addr),
            sizeof(addr), ActorId(0), static_cast<uint32_t>(OpType::SendTo));
    } else {
        // Legacy fallback: blocking sendto
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&dest)) {
            addr.sin_addr.s_addr = ipv4->addr;
            addr.sin_port = ipv4->port_nw;
        }
        sendto(sock, data.data(), data.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    }
}

// ---- Per-instance state (RNG, join retry, forwarded pings) ----

struct InstanceExtras {
    std::mt19937 rng;
    bool rng_seeded = false;
    size_t join_seed_index = 0;
    uint64_t join_retry_timer = 0;
    std::unordered_map<EndPoint, EndPoint> forwarded_pings;
};

static std::unordered_map<const GossipMembership*, InstanceExtras> s_extras;

static InstanceExtras& extras_for(const GossipMembership* self) {
    auto& extras = s_extras[self];
    if (!extras.rng_seeded) {
        std::random_device rd;
        extras.rng.seed(rd());
        extras.rng_seeded = true;
    }
    return extras;
}

static void cleanup_extras(const GossipMembership* self) {
    s_extras.erase(self);
}

} // anonymous namespace

// =============================================================================
// GossipMembership — Constructor / Destructor
// =============================================================================

GossipMembership::GossipMembership(const GossipConfig& cfg, EventLoop* loop)
    : config_(cfg), loop_(loop), incarnation_(1) // Start at 1 so 0 means "no
                                                 // incarnation"
      ,
      recv_buffer_(kGossipMaxMsgSize) {}

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

    setup_udp_socket();

    // Add self to the membership table.
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        Member self = config_.local_state;
        self.incarnation = incarnation_;
        self.status = MemberStatus::Alive;
        self.last_seen = std::chrono::steady_clock::now();
        members_[self.endpoint] = std::move(self);
    }

    // Bootstrap: join an existing cluster via a seed, or start a solo cluster.
    if (!config_.seeds.empty()) {
        auto& ext = extras_for(this);
        ext.join_seed_index = 0;
        send_join(config_.seeds[0]);

        // Recursive retry chain: try the next seed after 1 s if no SyncRsp
        // arrived.
        auto retry_fn = std::make_shared<std::function<void()>>();
        *retry_fn = [this, retry_fn]() {
            auto& ext_inner = extras_for(this);
            ext_inner.join_seed_index++;
            if (ext_inner.join_seed_index >= config_.seeds.size()) {
                return; // No more seeds to try
            }
            {
                std::shared_lock<std::shared_mutex> lock(members_mutex_);
                if (members_.size() > 1) {
                    return; // Already received SyncRsp — joined successfully
                }
            }
            send_join(config_.seeds[ext_inner.join_seed_index]);
            ext_inner.join_retry_timer = loop_->run_after(*retry_fn, 1000);
        };
        ext.join_retry_timer = loop_->run_after(*retry_fn, 1000);
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
    auto& ext = extras_for(this);
    if (ext.join_retry_timer != 0 && loop_) {
        loop_->cancel_timer(ext.join_retry_timer);
        ext.join_retry_timer = 0;
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

    // Tear down the UDP socket.
    teardown_udp_socket();

    // Clear local state.
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        members_.clear();
        pending_pings_.clear();
    }

    cleanup_extras(this);
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
                              config_.local_state.endpoint);
            ping->set_incarnation(inc);
            ping->set_seq_no(seq);
            for (const auto& e : pb)
                to_pb_piggyback(ping->add_piggyback(), e);
            break;
        }
        case GossipMessageType::Ack: {
            auto* ack = env.mutable_ack();
            ep_to_pb_endpoint(ack->mutable_sender_endpoint(),
                              config_.local_state.endpoint);
            ack->set_incarnation(inc);
            for (const auto& e : pb)
                to_pb_piggyback(ack->add_piggyback(), e);
            break;
        }
        case GossipMessageType::PingReq: {
            auto* pr = env.mutable_ping_req();
            ep_to_pb_endpoint(pr->mutable_sender_endpoint(),
                              config_.local_state.endpoint);
            ep_to_pb_endpoint(pr->mutable_target_endpoint(), ping_target);
            pr->set_incarnation(inc);
            break;
        }
        case GossipMessageType::IndirectAck: {
            auto* ia = env.mutable_indirect_ack();
            ep_to_pb_endpoint(ia->mutable_sender_endpoint(),
                              config_.local_state.endpoint);
            ep_to_pb_endpoint(ia->mutable_original_requester(), ping_target);
            ia->set_incarnation(inc);
            break;
        }
        case GossipMessageType::Join: {
            auto* join = env.mutable_join();
            ep_to_pb_endpoint(join->mutable_sender_endpoint(),
                              config_.local_state.endpoint);
            join->set_incarnation(inc);
            join->set_host(config_.local_state.host);
            join->set_uds_path(config_.local_state.uds_path);
            for (const auto& s : config_.local_state.actor_types)
                join->add_actor_types(s);
            for (const auto& acc : config_.local_state.acceptors) {
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
                              config_.local_state.endpoint);
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
    ep_to_pb_endpoint(rsp.mutable_sender_endpoint(), config_.local_state.endpoint);
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
        meta.endpoint = config.local_state.endpoint;
        meta.incarnation = incarnation;
        meta.actor_types = config.local_state.actor_types;
        meta.load = 0;
        meta.acceptors = config.local_state.acceptors;
        entries.push_back(std::move(meta));
    }

    // Piggyback all Suspicious and Dead members so the cluster converges
    // quickly.
    for (const auto& [ep, m] : members) {
        if (m.status == MemberStatus::Suspicious) {
            PiggybackEntry e;
            e.type = PiggybackType::Suspicious;
            e.endpoint = ep;
            e.incarnation = m.incarnation;
            entries.push_back(std::move(e));
        } else if (m.status == MemberStatus::Dead) {
            PiggybackEntry e;
            e.type = PiggybackType::Dead;
            e.endpoint = ep;
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
    auto now = std::chrono::steady_clock::now();

    // ── 1. Pick peers and send Pings ──────────────────────────────────────
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);

        // Collect Alive members excluding self.
        std::vector<EndPoint> alive;
        for (const auto& [ep, m] : members_) {
            if (m.status == MemberStatus::Alive &&
                !(ep == config_.local_state.endpoint)) {
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
                    encode_message(GossipMessageType::Ping, incarnation_,
                                   seq_no_++, config_.local_state.endpoint, pb);
                async_udp_send(loop_, udp_socket_, msg, target);

                // Record pending ping.
                // pending_pings_ is protected by members_mutex_ in our design.
                PendingPing pp;
                pp.expires_at = now + config_.ping_timeout;
                pp.indirect_requested = false;
                pending_pings_[target] = pp;
            }

            // Clear dissemination flag now that we've piggybacked our metadata.
            if (needs_dissemination_ && !pb.empty()) {
                needs_dissemination_ = false;
            }
        }
    } // shared_lock released

    // ── 2. Check pending pings ─────────────────────────────────────────────
    // Re-acquire unique lock because we may mutate pending_pings_ and members_.
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
                    target, config_.local_state.endpoint};
                auto probes = pick_random_peers(config_.indirect_probes, exclude);

                if (probes.empty()) {
                    // 2-node cluster or no other peers available — skip
                    // indirect probes and immediately mark suspicious.
                    mark_suspicious(target);
                    pending_pings_.erase(target);
                } else {
                    for (const auto& proxy : probes) {
                        send_ping_req(proxy, target);
                    }
                }
            } else {
                // Second expiry — indirect probes did not succeed.
                mark_suspicious(target);
                pending_pings_.erase(it);
            }
        }
    }

    // ── 3. Check suspicious members for timeout ────────────────────────────
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        std::vector<EndPoint> to_mark_dead;
        for (auto& [ep, m] : members_) {
            if (m.status == MemberStatus::Suspicious) {
                if (now - m.last_seen > config_.suspicion_timeout) {
                    to_mark_dead.push_back(ep);
                }
            }
        }
        for (const auto& ep : to_mark_dead) {
            mark_dead(ep);
        }
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
        return; // Malformed or unknown message
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
        case GossipMessageType::SyncRsp: {
            // SyncRsp uses a different wire format (decode_sync_rsp, not
            // decode_message). The data after the standard header contains the
            // SyncRsp payload. However, handle_sync_rsp expects a
            // vector<Member>, so we delegate to the sender to re-encode.
            // Actually the SyncRsp is carried in a regular message envelope —
            // decode_message parsed the header, and the piggyback entries are
            // not used.  We extract the member list from the raw data by
            // skipping the standard header.
            //
            // For SyncRsp, the sender includes the full table after the
            // piggyback count.  We re-parse from the raw data, but
            // decode_message already consumed everything.  Fortunately, SyncRsp
            // uses a dedicated encode_sync_rsp / decode_sync_rsp code path. The
            // incoming data is the *entire* SyncRsp payload (not the standard
            // message envelope). The handle_packet caller (UDP read handler)
            // passes the raw data, so for SyncRsp we decode differently.

            // Actually, looking at the wire design: Join is a standard message;
            // the seed responds with SyncRsp which is a SEPARATE wire format
            // (not through encode_message).  But it arrives on the same UDP
            // socket. We need a heuristic to distinguish standard messages from
            // SyncRsp.

            // Try SyncRsp format first (no magic prefix, starts with 4B count):
            std::vector<Member> members;
            if (decode_sync_rsp(data, members)) {
                handle_sync_rsp(std::move(members));
            }
            break;
        }
        case GossipMessageType::Leave:
            handle_leave(sender, inc);
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
        m.endpoint = sender;
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
                       config_.local_state.endpoint, ack_pb);
    async_udp_send(loop_, udp_socket_, ack_msg, sender);
}

void GossipMembership::handle_ack(EndPoint sender, uint64_t inc,
                                  std::vector<PiggybackEntry> pb) {
    // Merge the sender — an Ack proves they are alive.
    {
        Member m;
        m.endpoint = sender;
        m.incarnation = inc;
        m.status = MemberStatus::Alive;
        m.last_seen = std::chrono::steady_clock::now();
        merge_member(m);
    }

    // Check if this ack is for a forwarded PingReq.
    auto& ext = extras_for(this);
    auto fwd_it = ext.forwarded_pings.find(sender);
    if (fwd_it != ext.forwarded_pings.end()) {
        // We forwarded a Ping on behalf of fwd_it->second.
        // The target (sender) responded, so send IndirectAck to the original
        // requester.
        send_indirect_ack(fwd_it->second, sender);
        ext.forwarded_pings.erase(fwd_it);
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
        m.endpoint = sender;
        m.status = MemberStatus::Alive;
        m.last_seen = std::chrono::steady_clock::now();
        merge_member(m);
    }

    // Forward the ping on behalf of the requester.
    auto& ext = extras_for(this);
    ext.forwarded_pings[target] = sender;

    // Build piggyback and send Ping to target.
    std::vector<PiggybackEntry> pb;
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);
        pb = build_piggyback_impl(config_, incarnation_, needs_dissemination_,
                                  members_);
    }

    StreamBuffer msg = encode_message(GossipMessageType::Ping, incarnation_,
                                      seq_no_++, config_.local_state.endpoint, pb);
    async_udp_send(loop_, udp_socket_, msg, target);
}

void GossipMembership::handle_indirect_ack(EndPoint sender, EndPoint target) {
    // Merge the indirect ack sender.
    {
        Member m;
        m.endpoint = sender;
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
        m.endpoint = sender;
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
    // Merge all received members into our table.
    for (auto& m : members) {
        merge_member(m);
    }

    // Cancel any pending join retry — we have successfully joined.
    auto& ext = extras_for(this);
    if (ext.join_retry_timer != 0 && loop_) {
        loop_->cancel_timer(ext.join_retry_timer);
        ext.join_retry_timer = 0;
    }

    // Ensure self is still in the table (should always be, but be defensive).
    {
        std::unique_lock<std::shared_mutex> lock(members_mutex_);
        if (members_.find(config_.local_state.endpoint) == members_.end()) {
            Member self = config_.local_state;
            self.incarnation = incarnation_;
            self.status = MemberStatus::Alive;
            self.last_seen = std::chrono::steady_clock::now();
            members_[self.endpoint] = std::move(self);
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
    std::vector<PiggybackEntry> pb;
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);
        pb = build_piggyback_impl(config_, incarnation_, needs_dissemination_,
                                  members_);
    }
    StreamBuffer msg = encode_message(GossipMessageType::Ping, incarnation_,
                                      seq_no_++, config_.local_state.endpoint, pb);
    async_udp_send(loop_, udp_socket_, msg, target);
}

void GossipMembership::send_ack(EndPoint target, std::vector<PiggybackEntry> pb) {
    StreamBuffer msg = encode_message(GossipMessageType::Ack, incarnation_,
                                      seq_no_++, config_.local_state.endpoint, pb);
    async_udp_send(loop_, udp_socket_, msg, target);
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
    async_udp_send(loop_, udp_socket_, msg, proxy);
}

void GossipMembership::send_indirect_ack(EndPoint target, EndPoint orig_target) {
    // IndirectAck carries the original target in the ping_target field.
    std::vector<PiggybackEntry> pb; // empty piggyback for indirect ack
    StreamBuffer msg = encode_message(GossipMessageType::IndirectAck,
                                      incarnation_, seq_no_++, orig_target, pb);
    async_udp_send(loop_, udp_socket_, msg, target);
}

void GossipMembership::send_join(EndPoint seed) {
    // Join includes self metadata as piggyback.
    std::vector<PiggybackEntry> pb;
    {
        PiggybackEntry meta;
        meta.type = PiggybackType::Metadata;
        meta.endpoint = config_.local_state.endpoint;
        meta.incarnation = incarnation_;
        meta.actor_types = config_.local_state.actor_types;
        meta.acceptors = config_.local_state.acceptors;
        pb.push_back(std::move(meta));
    }
    StreamBuffer msg = encode_message(GossipMessageType::Join, incarnation_,
                                      seq_no_++, config_.local_state.endpoint, pb);
    async_udp_send(loop_, udp_socket_, msg, seed);
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
    async_udp_send(loop_, udp_socket_, data, target);
}

void GossipMembership::send_leave(EndPoint target) {
    std::vector<PiggybackEntry> pb; // empty piggyback for leave
    StreamBuffer msg = encode_message(GossipMessageType::Leave, incarnation_,
                                      seq_no_++, config_.local_state.endpoint, pb);
    async_udp_send(loop_, udp_socket_, msg, target);
}

// =============================================================================
// State mutation
// =============================================================================

void GossipMembership::mark_suspicious(EndPoint ep) {
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
    std::unique_lock<std::shared_mutex> lock(members_mutex_);

    auto it = members_.find(remote.endpoint);
    if (it == members_.end()) {
        // First time seeing this endpoint — insert.
        members_[remote.endpoint] = remote;
        members_[remote.endpoint].last_seen = std::chrono::steady_clock::now();
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
    if (!remote.host.empty()) {
        existing.host = remote.host;
    }
    if (!remote.acceptors.empty()) {
        existing.acceptors = remote.acceptors;
    }

    existing.last_seen = std::chrono::steady_clock::now();
}

void GossipMembership::apply_piggyback(const std::vector<PiggybackEntry>& entries) {
    std::unique_lock<std::shared_mutex> lock(members_mutex_);

    for (const auto& entry : entries) {
        auto it = members_.find(entry.endpoint);
        if (it == members_.end()) {
            // We don't know this endpoint — insert it with the piggyback info.
            Member m;
            m.endpoint = entry.endpoint;
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
                    m.acceptors = entry.acceptors;
                    break;
            }
            members_[entry.endpoint] = std::move(m);
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
                if (!entry.acceptors.empty()) {
                    existing.acceptors = entry.acceptors;
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
    auto& ext = extras_for(this);

    std::shared_lock<std::shared_mutex> lock(members_mutex_);

    // Collect Alive peers excluding self and explicitly excluded endpoints.
    std::vector<EndPoint> candidates;
    for (const auto& [ep, m] : members_) {
        if (m.status != MemberStatus::Alive)
            continue;
        if (ep == config_.local_state.endpoint)
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
    std::shuffle(candidates.begin(), candidates.end(), ext.rng);
    candidates.resize(count);
    return candidates;
}

// =============================================================================
// Socket setup/teardown
// =============================================================================

void GossipMembership::setup_udp_socket() {
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_socket_ < 0)
        return;

    int reuse = 1;
    setsockopt(udp_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.gossip_port);

    if (bind(udp_socket_, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        close(udp_socket_);
        udp_socket_ = -1;
        return;
    }

    if (loop_) {
        loop_->add_fd(udp_socket_, EventLoop::Event::Read);
        loop_->set_read_handler(udp_socket_, [this](int /*fd*/) {
            // Non-blocking recvfrom loop (edge-triggered).
            struct sockaddr_in src_addr;
            socklen_t src_addr_len = sizeof(src_addr);

            while (true) {
                ssize_t n = recvfrom(udp_socket_, recv_buffer_.data(),
                                     recv_buffer_.size(), MSG_DONTWAIT,
                                     reinterpret_cast<struct sockaddr*>(&src_addr),
                                     &src_addr_len);
                if (n <= 0)
                    break;

                StreamBuffer data(recv_buffer_.data(),
                                  recv_buffer_.data() + static_cast<size_t>(n));

                std::string from_host;
                uint16_t from_port = 0;
                char ip_str[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str))) {
                    from_host = ip_str;
                }
                from_port = ntohs(src_addr.sin_port);

                handle_packet(data, from_host, from_port);
            }
        });
    }
}

void GossipMembership::teardown_udp_socket() {
    if (udp_socket_ >= 0) {
        if (loop_) {
            loop_->clear_read_handler(udp_socket_);
            loop_->remove_fd(udp_socket_);
        }
        close(udp_socket_);
        udp_socket_ = -1;
    }
}

} // namespace hpactor::net
