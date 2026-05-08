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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <random>

namespace hpactor::net {

// =============================================================================
// Internal wire-format helpers (anonymous namespace)
// =============================================================================
namespace {

// ---- Write helpers (big-endian, append to StreamBuffer) ----

void write_u8(StreamBuffer& buf, uint8_t v) {
    buf.push_back(v);
}

void write_u16_be(StreamBuffer& buf, uint16_t v) {
    uint16_t be = htons(v);
    buf.append(reinterpret_cast<const uint8_t*>(&be), sizeof(be));
}

void write_u32_be(StreamBuffer& buf, uint32_t v) {
    uint32_t be = htonl(v);
    buf.append(reinterpret_cast<const uint8_t*>(&be), sizeof(be));
}

void write_u64_be(StreamBuffer& buf, uint64_t v) {
    // Manual big-endian encoding (portable, no htonll dependency)
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>(v >> (static_cast<unsigned>(i) * 8)));
    }
}

void write_bytes(StreamBuffer& buf, const uint8_t* data, size_t len) {
    buf.append(data, len);
}

// ---- Read helpers (big-endian, from raw buffer, return false on underrun) ----

bool read_u8(const uint8_t* data, size_t data_len, size_t& pos, uint8_t& out) {
    if (pos + 1 > data_len) return false;
    out = data[pos];
    pos += 1;
    return true;
}

bool read_u16_be(const uint8_t* data, size_t data_len, size_t& pos, uint16_t& out) {
    if (pos + 2 > data_len) return false;
    uint16_t be;
    std::memcpy(&be, data + pos, sizeof(be));
    out = ntohs(be);
    pos += 2;
    return true;
}

bool read_u32_be(const uint8_t* data, size_t data_len, size_t& pos, uint32_t& out) {
    if (pos + 4 > data_len) return false;
    uint32_t be;
    std::memcpy(&be, data + pos, sizeof(be));
    out = ntohl(be);
    pos += 4;
    return true;
}

bool read_u64_be(const uint8_t* data, size_t data_len, size_t& pos, uint64_t& out) {
    if (pos + 8 > data_len) return false;
    // Manual big-endian decode
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | static_cast<uint64_t>(data[pos + static_cast<size_t>(i)]);
    }
    pos += 8;
    return true;
}

// ---- Endpoint encode/decode ----
//
// Wire format for an endpoint (with length prefix):
//   len (1B) | addr_family (1B) | address | port (2B BE)
//   addr_family: 0x04 = IPv4, 0x06 = IPv6
//   len = 1 + addr_bytes + 2  (i.e., 7 for IPv4, 19 for IPv6)

void encode_endpoint_with_len(StreamBuffer& buf, const EndPoint& ep) {
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
        write_u8(buf, 7);            // len: 1(af) + 4(addr) + 2(port)
        write_u8(buf, 0x04);         // AF_INET
        write_u32_be(buf, ipv4->addr);
        write_u16_be(buf, ipv4->port_nw);
    } else {
        auto* ipv6 = std::get_if<Ipv6Endpoint>(&ep);
        write_u8(buf, 19);           // len: 1(af) + 16(addr) + 2(port)
        write_u8(buf, 0x06);         // AF_INET6
        write_bytes(buf, ipv6->addr.data(), 16);
        write_u16_be(buf, ipv6->port_nw);
    }
}

bool decode_endpoint_with_len(const uint8_t* data, size_t data_len, size_t& pos,
                               EndPoint& out) {
    uint8_t ep_len;
    if (!read_u8(data, data_len, pos, ep_len)) return false;
    if (pos + ep_len > data_len) return false;

    uint8_t af;
    if (!read_u8(data, data_len, pos, af)) return false;

    if (af == 0x04) {
        // IPv4
        uint32_t addr;
        uint16_t port_nw;
        if (!read_u32_be(data, data_len, pos, addr)) return false;
        if (!read_u16_be(data, data_len, pos, port_nw)) return false;
        out = Ipv4Endpoint{addr, port_nw};
        return true;
    } else if (af == 0x06) {
        // IPv6
        if (pos + 16 + 2 > data_len) return false;
        std::array<uint8_t, 16> addr;
        std::memcpy(addr.data(), data + pos, 16);
        pos += 16;
        uint16_t port_nw;
        if (!read_u16_be(data, data_len, pos, port_nw)) return false;
        out = Ipv6Endpoint{addr, port_nw};
        return true;
    }
    return false; // Unknown address family
}

// ---- Piggyback entry encode/decode ----

void encode_piggyback(StreamBuffer& buf, const PiggybackEntry& entry) {
    write_u8(buf, static_cast<uint8_t>(entry.type));
    encode_endpoint_with_len(buf, entry.endpoint);
    write_u64_be(buf, entry.incarnation);

    // Metadata fields (PiggybackType::Metadata)
    // Encode metadata only when present (non-empty actor_types, non-zero load, or
    // non-empty acceptors). Write a field count byte even for non-Metadata entries
    // (0 fields) to keep the format regular.
    size_t field_count_pos = buf.size();
    write_u8(buf, 0); // Field count placeholder
    uint8_t field_count = 0;

    // actor_types (tag 0x01): null-separated strings
    if (!entry.actor_types.empty()) {
        ++field_count;
        write_u8(buf, 0x01); // tag: actor_types
        // Compute payload length
        size_t payload = 0;
        for (const auto& s : entry.actor_types) {
            payload += s.size() + 1; // string + null terminator
        }
        write_u16_be(buf, static_cast<uint16_t>(payload));
        for (const auto& s : entry.actor_types) {
            write_bytes(buf, reinterpret_cast<const uint8_t*>(s.data()), s.size());
            write_u8(buf, 0); // null separator
        }
    }

    // load (tag 0x02): uint32_t BE
    if (entry.load != 0) {
        ++field_count;
        write_u8(buf, 0x02); // tag: load
        write_u16_be(buf, 4); // length
        write_u32_be(buf, entry.load);
    }

    // acceptors (tag 0x03): repeated (port 2B BE | handshake 1B | protocol 1B | tls 1B)
    if (!entry.acceptors.empty()) {
        ++field_count;
        write_u8(buf, 0x03); // tag: acceptors
        uint16_t payload = static_cast<uint16_t>(entry.acceptors.size() * 5);
        write_u16_be(buf, payload);
        for (const auto& acc : entry.acceptors) {
            write_u16_be(buf, acc.port);
            write_u8(buf, acc.handshake_version);
            write_u8(buf, acc.protocol_version);
            write_u8(buf, acc.tls_required ? 1 : 0);
        }
    }

    // Patch field count
    if (field_count <= 0xff && buf.size() > field_count_pos) {
        // We need to write to buf[field_count_pos]. The field count pos is relative
        // to the logical start, but the buffer may have consumed data. Since we
        // haven't consumed anything in this function, the logical index equals the
        // absolute index.
        buf[field_count_pos] = field_count;
    }
}

bool decode_piggyback(const uint8_t* data, size_t data_len, size_t& pos,
                       PiggybackEntry& out) {
    uint8_t type_byte;
    if (!read_u8(data, data_len, pos, type_byte)) return false;
    out.type = static_cast<PiggybackType>(type_byte);

    if (!decode_endpoint_with_len(data, data_len, pos, out.endpoint)) return false;
    if (!read_u64_be(data, data_len, pos, out.incarnation)) return false;

    // Metadata fields
    uint8_t field_count;
    if (!read_u8(data, data_len, pos, field_count)) return false;

    for (uint8_t f = 0; f < field_count; ++f) {
        uint8_t tag;
        uint16_t field_len;
        if (!read_u8(data, data_len, pos, tag)) return false;
        if (!read_u16_be(data, data_len, pos, field_len)) return false;
        if (pos + field_len > data_len) return false;

        if (tag == 0x01) {
            // actor_types: null-separated strings
            const uint8_t* field_start = data + pos;
            size_t remaining = field_len;
            while (remaining > 0) {
                const char* str_start = reinterpret_cast<const char*>(field_start);
                size_t str_len = strnlen(str_start, remaining);
                if (str_len > 0) {
                    out.actor_types.emplace_back(str_start, str_len);
                }
                size_t consumed = str_len + 1; // string + null
                consumed = std::min(consumed, remaining);
                field_start += consumed;
                remaining -= consumed;
            }
        } else if (tag == 0x02) {
            // load: uint32_t BE
            if (field_len >= 4) {
                uint32_t be;
                std::memcpy(&be, data + pos, sizeof(be));
                out.load = ntohl(be);
            }
        } else if (tag == 0x03) {
            // acceptors: repeated (port 2B | handshake 1B | protocol 1B | tls 1B)
            if (field_len >= 5) {
                size_t count = field_len / 5;
                for (size_t i = 0; i < count; ++i) {
                    AcceptorInfo acc;
                    uint16_t port_be;
                    std::memcpy(&port_be, data + pos, sizeof(port_be));
                    acc.port = ntohs(port_be);
                    pos += 2;
                    acc.handshake_version = data[pos++];
                    acc.protocol_version = data[pos++];
                    acc.tls_required = (data[pos++] != 0);
                    out.acceptors.push_back(acc);
                }
                continue; // pos already advanced, skip the pos += field_len below
            }
        }
        pos += field_len;
    }

    return true;
}

// ---- Instance extras (per-instance state not in the header) ----
// This avoids modifying gossip_membership.hpp while supporting the full protocol.

struct InstanceExtras {
    std::mt19937 rng{std::random_device{}()};
    size_t join_seed_index = 0;
    uint64_t join_retry_timer = 0;
    // Map: ping_target -> original_requester (for PingReq -> IndirectAck bridging)
    std::unordered_map<EndPoint, EndPoint> forwarded_pings;
    // Piggyback entries to include in next outgoing message
    std::vector<PiggybackEntry> pending_piggyback;
};

// Per-instance extended state keyed by GossipMembership pointer.
static std::unordered_map<const GossipMembership*, InstanceExtras> s_extras;

static InstanceExtras& extras_for(const GossipMembership* self) {
    return s_extras[self];
}

static void cleanup_extras(const GossipMembership* self) {
    s_extras.erase(self);
}

// ---- Sockaddr helpers ----

/// Build a sockaddr_in from an IPv4Endpoint and UDP port.
/// The port in the endpoint is used directly.
sockaddr_in make_sockaddr(const EndPoint& ep) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
        addr.sin_addr.s_addr = ipv4->addr;
        addr.sin_port = ipv4->port_nw;
    }
    // IPv6 not yet supported for gossip transport
    return addr;
}

/// Send a pre-built buffer to a destination endpoint via UDP.
bool udp_sendto(int sock, const StreamBuffer& data, const EndPoint& dest) {
    if (sock < 0) return false;
    sockaddr_in addr = make_sockaddr(dest);
    ssize_t sent = sendto(sock, data.data(), data.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<ssize_t>(data.size());
}

} // anonymous namespace

// =============================================================================
// GossipMembership — Constructor / Destructor
// =============================================================================

GossipMembership::GossipMembership(const GossipConfig& cfg, EventLoop* loop)
    : config_(cfg)
    , loop_(loop)
    , incarnation_(1)  // Start at 1 so 0 means "no incarnation"
    , recv_buffer_(kGossipMaxMsgSize)
{
}

GossipMembership::~GossipMembership() {
    stop();
}

// =============================================================================
// IServiceDiscovery — lifecycle
// =============================================================================

void GossipMembership::start() {
    // Incarnation is a wall-clock timestamp so that restarted nodes automatically
    // have a higher incarnation than any previous run.
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

        // Recursive retry chain: try the next seed after 1 s if no SyncRsp arrived.
        auto retry_fn = std::make_shared<std::function<void()>>();
        *retry_fn = [this, retry_fn]() {
            auto& ext = extras_for(this);
            ext.join_seed_index++;
            if (ext.join_seed_index >= config_.seeds.size()) {
                return; // No more seeds to try
            }
            {
                std::shared_lock<std::shared_mutex> lock(members_mutex_);
                if (members_.size() > 1) {
                    return; // Already received SyncRsp — joined successfully
                }
            }
            send_join(config_.seeds[ext.join_seed_index]);
            ext.join_retry_timer = loop_->run_after(*retry_fn, 1000);
        };
        ext.join_retry_timer = loop_->run_after(*retry_fn, 1000);
    }

    // Schedule the periodic protocol round.
    protocol_timer_ = loop_->run_every(
        [this] { protocol_round(); },
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

StreamBuffer GossipMembership::encode_message(GossipMessageType type, uint64_t inc,
    uint32_t seq, EndPoint ping_target, const std::vector<PiggybackEntry>& pb) const {
    StreamBuffer buf;

    // Fixed header: magic + version + type + flags
    write_u32_be(buf, GossipMagic);          // 4B
    write_u8(buf, GossipVersion);            // 1B
    write_u8(buf, static_cast<uint8_t>(type)); // 1B
    write_u16_be(buf, 0);                    // 2B flags (reserved)

    // Sender endpoint (our local state endpoint)
    encode_endpoint_with_len(buf, config_.local_state.endpoint);

    // Incarnation + sequence number
    write_u64_be(buf, inc);  // 8B
    write_u32_be(buf, seq);  // 4B

    // Ping target (PingReq only)
    if (type == GossipMessageType::PingReq) {
        encode_endpoint_with_len(buf, ping_target);
    }

    // Piggyback entries
    write_u16_be(buf, static_cast<uint16_t>(pb.size()));
    for (const auto& entry : pb) {
        encode_piggyback(buf, entry);
    }

    return buf;
}

// =============================================================================
// Wire protocol — decode_message
// =============================================================================

bool GossipMembership::decode_message(const StreamBuffer& data, GossipMessageType& type,
    EndPoint& sender, uint64_t& inc, uint32_t& seq, EndPoint& ping_target,
    std::vector<PiggybackEntry>& pb) const {
    const uint8_t* raw = data.data();
    size_t len = data.size();
    size_t pos = 0;

    // Magic
    uint32_t magic;
    if (!read_u32_be(raw, len, pos, magic)) return false;
    if (magic != GossipMagic) return false;

    // Version
    uint8_t version;
    if (!read_u8(raw, len, pos, version)) return false;
    if (version != GossipVersion) return false;

    // Type
    uint8_t type_byte;
    if (!read_u8(raw, len, pos, type_byte)) return false;
    type = static_cast<GossipMessageType>(type_byte);

    // Flags (skip for now)
    uint16_t flags;
    if (!read_u16_be(raw, len, pos, flags)) return false;
    (void)flags;

    // Sender endpoint
    if (!decode_endpoint_with_len(raw, len, pos, sender)) return false;

    // Incarnation
    if (!read_u64_be(raw, len, pos, inc)) return false;

    // Sequence number
    if (!read_u32_be(raw, len, pos, seq)) return false;

    // Ping target (PingReq only)
    if (type == GossipMessageType::PingReq) {
        if (!decode_endpoint_with_len(raw, len, pos, ping_target)) return false;
    }

    // Piggyback entries
    uint16_t pb_count;
    if (!read_u16_be(raw, len, pos, pb_count)) return false;

    pb.clear();
    pb.reserve(pb_count);
    for (uint16_t i = 0; i < pb_count; ++i) {
        PiggybackEntry entry;
        if (!decode_piggyback(raw, len, pos, entry)) return false;
        pb.push_back(std::move(entry));
    }

    // We don't require exact consumption (future extensions may append fields)
    return true;
}

// =============================================================================
// Wire protocol — encode_sync_rsp
// =============================================================================
//
// Binary format:
//   Count (4B BE) | Member[0..N]
//   Member: endpoint (var, len-prefixed) | incarnation (8B BE) | flags (1B: status)

StreamBuffer GossipMembership::encode_sync_rsp(const std::vector<Member>& members) const {
    StreamBuffer buf;

    write_u32_be(buf, static_cast<uint32_t>(members.size()));

    for (const auto& m : members) {
        encode_endpoint_with_len(buf, m.endpoint);
        write_u64_be(buf, m.incarnation);
        write_u8(buf, static_cast<uint8_t>(m.status));
    }

    return buf;
}

// =============================================================================
// Wire protocol — decode_sync_rsp
// =============================================================================

bool GossipMembership::decode_sync_rsp(const StreamBuffer& data,
                                        std::vector<Member>& members) const {
    const uint8_t* raw = data.data();
    size_t len = data.size();
    size_t pos = 0;

    uint32_t count;
    if (!read_u32_be(raw, len, pos, count)) return false;

    members.clear();
    members.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        Member m;
        if (!decode_endpoint_with_len(raw, len, pos, m.endpoint)) return false;
        if (!read_u64_be(raw, len, pos, m.incarnation)) return false;

        uint8_t status_byte;
        if (!read_u8(raw, len, pos, status_byte)) return false;
        m.status = static_cast<MemberStatus>(status_byte);

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
build_piggyback_impl(const GossipConfig& config,
                     uint64_t incarnation,
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

    // Piggyback all Suspicious and Dead members so the cluster converges quickly.
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
                StreamBuffer msg = encode_message(GossipMessageType::Ping,
                    incarnation_, seq_no_++, config_.local_state.endpoint, pb);
                udp_sendto(udp_socket_, msg, target);

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
            if (it == pending_pings_.end()) continue;
            auto& pp = it->second;

            if (!pp.indirect_requested) {
                // First expiry — request indirect probes.
                pp.indirect_requested = true;
                pp.indirect_expires_at = now + config_.ping_timeout;

                // Pick indirect probe peers: Alive, not self, not the target.
                std::unordered_set<EndPoint> exclude{target, config_.local_state.endpoint};
                auto probes = pick_random_peers(config_.indirect_probes, exclude);

                if (probes.empty()) {
                    // 2-node cluster or no other peers available — skip indirect
                    // probes and immediately mark suspicious.
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
        // SyncRsp uses a different wire format (decode_sync_rsp, not decode_message).
        // The data after the standard header contains the SyncRsp payload.
        // However, handle_sync_rsp expects a vector<Member>, so we delegate to the
        // sender to re-encode.  Actually the SyncRsp is carried in a regular
        // message envelope — decode_message parsed the header, and the piggyback
        // entries are not used.  We extract the member list from the raw data
        // by skipping the standard header.
        //
        // For SyncRsp, the sender includes the full table after the piggyback
        // count.  We re-parse from the raw data, but decode_message already
        // consumed everything.  Fortunately, SyncRsp uses a dedicated
        // encode_sync_rsp / decode_sync_rsp code path.  The incoming data is
        // the *entire* SyncRsp payload (not the standard message envelope).
        // The handle_packet caller (UDP read handler) passes the raw data,
        // so for SyncRsp we decode differently.

        // Actually, looking at the wire design: Join is a standard message;
        // the seed responds with SyncRsp which is a SEPARATE wire format
        // (not through encode_message).  But it arrives on the same UDP socket.
        // We need a heuristic to distinguish standard messages from SyncRsp.

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
    StreamBuffer ack_msg = encode_message(GossipMessageType::Ack,
        incarnation_, seq_no_++, config_.local_state.endpoint, ack_pb);
    udp_sendto(udp_socket_, ack_msg, sender);
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
        // The target (sender) responded, so send IndirectAck to the original requester.
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
        pb = build_piggyback_impl(config_, incarnation_,
                                   needs_dissemination_, members_);
    }

    StreamBuffer msg = encode_message(GossipMessageType::Ping,
        incarnation_, seq_no_++, config_.local_state.endpoint, pb);
    udp_sendto(udp_socket_, msg, target);
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
        pb = build_piggyback_impl(config_, incarnation_,
                                   needs_dissemination_, members_);
    }
    StreamBuffer msg = encode_message(GossipMessageType::Ping,
        incarnation_, seq_no_++, config_.local_state.endpoint, pb);
    udp_sendto(udp_socket_, msg, target);
}

void GossipMembership::send_ack(EndPoint target, std::vector<PiggybackEntry> pb) {
    StreamBuffer msg = encode_message(GossipMessageType::Ack,
        incarnation_, seq_no_++, config_.local_state.endpoint, pb);
    udp_sendto(udp_socket_, msg, target);
}

void GossipMembership::send_ping_req(EndPoint proxy, EndPoint target) {
    // Build minimal piggyback for PingReq.
    std::vector<PiggybackEntry> pb;
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);
        pb = build_piggyback_impl(config_, incarnation_,
                                   needs_dissemination_, members_);
    }
    StreamBuffer msg = encode_message(GossipMessageType::PingReq,
        incarnation_, seq_no_++, target, pb);
    udp_sendto(udp_socket_, msg, proxy);
}

void GossipMembership::send_indirect_ack(EndPoint target, EndPoint orig_target) {
    // IndirectAck carries the original target in the ping_target field.
    std::vector<PiggybackEntry> pb; // empty piggyback for indirect ack
    StreamBuffer msg = encode_message(GossipMessageType::IndirectAck,
        incarnation_, seq_no_++, orig_target, pb);
    udp_sendto(udp_socket_, msg, target);
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
    StreamBuffer msg = encode_message(GossipMessageType::Join,
        incarnation_, seq_no_++, config_.local_state.endpoint, pb);
    udp_sendto(udp_socket_, msg, seed);
}

void GossipMembership::send_sync_rsp(EndPoint target) {
    // Collect non-Dead, non-Left members for the sync response.
    std::vector<Member> table;
    {
        std::shared_lock<std::shared_mutex> lock(members_mutex_);
        for (const auto& [ep, m] : members_) {
            if (m.status != MemberStatus::Dead &&
                m.status != MemberStatus::Left) {
                table.push_back(m);
            }
        }
    }
    StreamBuffer data = encode_sync_rsp(table);
    udp_sendto(udp_socket_, data, target);
}

void GossipMembership::send_leave(EndPoint target) {
    std::vector<PiggybackEntry> pb; // empty piggyback for leave
    StreamBuffer msg = encode_message(GossipMessageType::Leave,
        incarnation_, seq_no_++, config_.local_state.endpoint, pb);
    udp_sendto(udp_socket_, msg, target);
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
    if (remote.status != MemberStatus::Alive || existing.status == MemberStatus::Dead) {
        // Accept status from remote unless we already have it as Dead and it's
        // not a reactivation.
    }
    if (remote.status == MemberStatus::Suspicious ||
        remote.status == MemberStatus::Dead ||
        remote.status == MemberStatus::Left) {
        existing.status = remote.status;
    }

    // Accept metadata if present.
    if (!remote.actor_types.empty()) {
        existing.actor_types = remote.actor_types;
    }
    if (remote.tcp_port != 0) {
        existing.tcp_port = remote.tcp_port;
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

        if ((m.status == MemberStatus::Dead ||
             m.status == MemberStatus::Left) &&
            now - m.last_seen > config_.dead_timeout) {
            it = members_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<EndPoint> GossipMembership::pick_random_peers(size_t count,
    std::unordered_set<EndPoint> exclude) {
    auto& ext = extras_for(this);

    std::shared_lock<std::shared_mutex> lock(members_mutex_);

    // Collect Alive peers excluding self and explicitly excluded endpoints.
    std::vector<EndPoint> candidates;
    for (const auto& [ep, m] : members_) {
        if (m.status != MemberStatus::Alive) continue;
        if (ep == config_.local_state.endpoint) continue; // exclude self
        if (exclude.find(ep) != exclude.end()) continue;
        candidates.push_back(ep);
    }

    if (candidates.empty()) return {};
    if (candidates.size() <= count) return candidates;

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
    if (udp_socket_ < 0) return;

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
                if (n <= 0) break;

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
