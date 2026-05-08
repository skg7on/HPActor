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

// Unit tests for GossipMembership (SWIM protocol in isolation).
// We use #define private public to allow direct testing of private methods
// (merge_member, mark_suspicious, pick_random_peers, etc.).  The layout and
// symbols are unchanged — the library is already compiled with the real access
// levels; this is compile-time only.
#define private public
#include <hpactor/net/gossip_membership.hpp>
#undef private

#include <cassert>
#include <cstdio>

using namespace hpactor;
using namespace hpactor::net;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static EndPoint ep(uint16_t port) {
    return Ipv4Endpoint{0x7F000001, htons(port)};
}

static Member make_member(EndPoint e, const char* host, MemberStatus st,
                          uint64_t inc) {
    Member m;
    m.endpoint = e;
    if (host)
        m.host = host;
    m.status = st;
    m.incarnation = inc;
    m.last_seen = std::chrono::steady_clock::now();
    return m;
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

int main() {
    // ---- Test 1: Construction with defaults ---------------------------------
    {
        GossipConfig cfg;
        GossipMembership gm(cfg, nullptr);

        assert(gm.backend_name() == "gossip");
        assert(gm.config_.gossip_port == 5354);
        assert(gm.config_.protocol_period.count() == 1000);
        assert(gm.config_.ping_timeout.count() == 200);
        assert(gm.config_.suspicion_timeout.count() == 3000);
        assert(gm.config_.dead_timeout.count() == 30000);
        assert(gm.config_.fanout == 3);
        assert(gm.config_.indirect_probes == 3);
        assert(gm.config_.seeds.empty());
        assert(gm.incarnation_ == 1);
        assert(gm.needs_dissemination_ == false);
        assert(gm.members_.empty());
        assert(gm.udp_socket_ == -1);
    }

    // ---- Test 2: Bootstrap — solo cluster (zero seeds) ---------------------
    {
        GossipConfig cfg;
        EndPoint self_ep = ep(9000);
        cfg.local_state.endpoint = self_ep;
        cfg.local_state.host = "127.0.0.1";
        GossipMembership gm(cfg, nullptr);

        // Simulate the self-insertion that start() would perform, without I/O.
        gm.incarnation_ = 1;
        Member self = make_member(self_ep, "127.0.0.1", MemberStatus::Alive,
                                 gm.incarnation_);
        gm.members_[self_ep] = std::move(self);

        // discover_all returns a snapshot containing self.
        auto all = gm.discover_all();
        assert(all.size() == 1);
        assert(all[0].status == MemberStatus::Alive);
        assert(all[0].endpoint == self_ep);

        // discover returns a pointer for the known endpoint.
        const auto* found = gm.discover(self_ep);
        assert(found != nullptr);
        assert(found->status == MemberStatus::Alive);

        // discover returns nullptr for an unknown endpoint.
        assert(gm.discover(ep(9999)) == nullptr);
    }

    // ---- Test 3: announce() bumps incarnation and sets flag ----------------
    {
        GossipConfig cfg;
        EndPoint self_ep = ep(9000);
        cfg.local_state.endpoint = self_ep;
        cfg.local_state.host = "original-host";
        GossipMembership gm(cfg, nullptr);
        gm.incarnation_ = 100;
        gm.needs_dissemination_ = false;

        Member ann;
        ann.endpoint = self_ep;
        ann.host = "updated-host";
        gm.announce(std::move(ann));

        assert(gm.incarnation_ > 100);               // incarnation bumped
        assert(gm.needs_dissemination_ == true);      // dissemination flag set
        assert(gm.config_.local_state.incarnation == gm.incarnation_);
        assert(gm.config_.local_state.host == "updated-host");
    }

    // ---- Test 4: discover_all() returns a copy, not a reference -----------
    {
        GossipConfig cfg;
        EndPoint self_ep = ep(9000);
        cfg.local_state.endpoint = self_ep;
        GossipMembership gm(cfg, nullptr);

        Member self;
        self.endpoint = self_ep;
        self.status = MemberStatus::Alive;
        self.incarnation = 1;
        gm.members_[self_ep] = self;

        auto all = gm.discover_all();
        assert(all.size() == 1);

        // Mutate the returned vector — original must be untouched.
        all[0].status = MemberStatus::Dead;
        assert(gm.members_[self_ep].status == MemberStatus::Alive);

        // Also verify via discover() that it's unchanged.
        const auto* via_discover = gm.discover(self_ep);
        assert(via_discover != nullptr);
        assert(via_discover->status == MemberStatus::Alive);
    }

    // ---- Test 5: discover() — covered thoroughly in tests 2 and 4 ---------

    // ---- Test 6: merge_member None -> Alive on first contact ---------------
    {
        GossipConfig cfg;
        EndPoint self_ep = ep(9000);
        cfg.local_state.endpoint = self_ep;
        GossipMembership gm(cfg, nullptr);
        gm.members_[self_ep] = Member{};

        EndPoint alice = ep(9001);
        Member remote = make_member(alice, "10.0.0.1", MemberStatus::Alive, 5);
        gm.merge_member(remote);

        assert(gm.members_.size() == 2);
        auto it = gm.members_.find(alice);
        assert(it != gm.members_.end());
        assert(it->second.status == MemberStatus::Alive);
        assert(it->second.incarnation == 5);
        assert(it->second.host == "10.0.0.1");
    }

    // ---- Test 7: merge_member — update on higher incarnation ---------------
    {
        GossipConfig cfg;
        EndPoint self_ep = ep(9000);
        cfg.local_state.endpoint = self_ep;
        GossipMembership gm(cfg, nullptr);
        gm.members_[self_ep] = Member{};

        EndPoint alice = ep(9001);
        Member first = make_member(alice, "host-a", MemberStatus::Alive, 5);
        gm.merge_member(first);

        // Update with higher incarnation.
        Member second = make_member(alice, "host-b", MemberStatus::Alive, 10);
        gm.merge_member(second);

        assert(gm.members_[alice].incarnation == 10);
        assert(gm.members_[alice].host == "host-b");
    }

    // ---- Test 8: merge_member — ignore lower incarnation (stale) -----------
    {
        GossipConfig cfg;
        EndPoint self_ep = ep(9000);
        cfg.local_state.endpoint = self_ep;
        GossipMembership gm(cfg, nullptr);
        gm.members_[self_ep] = Member{};

        EndPoint alice = ep(9001);
        Member first = make_member(alice, "original", MemberStatus::Alive, 10);
        gm.merge_member(first);

        // Stale update — entirely ignored.
        Member stale;
        stale.endpoint = alice;
        stale.incarnation = 5;
        stale.status = MemberStatus::Suspicious;
        stale.host = "stale";
        gm.merge_member(stale);

        assert(gm.members_[alice].incarnation == 10);
        assert(gm.members_[alice].status == MemberStatus::Alive);
        assert(gm.members_[alice].host == "original");
    }

    // ---- Test 9: merge_member — Dead -> Alive on reincarnation ------------
    {
        GossipConfig cfg;
        EndPoint self_ep = ep(9000);
        cfg.local_state.endpoint = self_ep;
        GossipMembership gm(cfg, nullptr);
        gm.members_[self_ep] = Member{};

        EndPoint alice = ep(9001);
        Member first = make_member(alice, "host-a", MemberStatus::Alive, 10);
        gm.merge_member(first);
        gm.members_[alice].status = MemberStatus::Dead;

        // Reincarnate with higher incarnation.
        Member re = make_member(alice, "host-b", MemberStatus::Alive, 20);
        gm.merge_member(re);

        assert(gm.members_[alice].status == MemberStatus::Alive);
        assert(gm.members_[alice].incarnation == 20);
        assert(gm.members_[alice].host == "host-b");
    }

    // ---- Test 10: mark_suspicious / mark_dead transitions ------------------
    {
        GossipConfig cfg;
        EndPoint self_ep = ep(9000);
        cfg.local_state.endpoint = self_ep;
        GossipMembership gm(cfg, nullptr);
        gm.members_[self_ep] = Member{};

        EndPoint alice = ep(9001);
        gm.merge_member(make_member(alice, "h", MemberStatus::Alive, 1));

        gm.mark_suspicious(alice);
        assert(gm.members_[alice].status == MemberStatus::Suspicious);

        gm.mark_dead(alice);
        assert(gm.members_[alice].status == MemberStatus::Dead);
    }

    // ---- Test 11: pick_random_peers — all available when fewer than count ---
    {
        GossipConfig cfg;
        cfg.local_state.endpoint = ep(9000);
        GossipMembership gm(cfg, nullptr);
        // Self must be Alive.
        Member self;
        self.endpoint = ep(9000);
        self.status = MemberStatus::Alive;
        gm.members_[ep(9000)] = self;

        // Add three Alive peers.
        for (uint16_t p = 9001; p <= 9003; ++p) {
            Member peer;
            peer.endpoint = ep(p);
            peer.status = MemberStatus::Alive;
            gm.members_[ep(p)] = std::move(peer);
        }

        auto peers = gm.pick_random_peers(10, {});
        assert(peers.size() == 3);     // all peers, self excluded
    }

    // ---- Test 12: pick_random_peers — empty when solo cluster -------------
    {
        GossipConfig cfg;
        cfg.local_state.endpoint = ep(9000);
        GossipMembership gm(cfg, nullptr);
        Member self;
        self.endpoint = ep(9000);
        self.status = MemberStatus::Alive;
        gm.members_[ep(9000)] = self;

        auto peers = gm.pick_random_peers(3, {});
        assert(peers.empty());
    }

    // ---- Test 13: purge_dead_tombstones -----------------------------------
    {
        GossipConfig cfg;
        cfg.dead_timeout = std::chrono::milliseconds(50);
        cfg.local_state.endpoint = ep(9000);
        GossipMembership gm(cfg, nullptr);
        gm.members_[ep(9000)] = Member{};

        // Dead member far past timeout.
        auto ancient = std::chrono::steady_clock::now() -
                       std::chrono::seconds(60);
        Member dead;
        dead.endpoint = ep(9001);
        dead.status = MemberStatus::Dead;
        dead.last_seen = ancient;
        gm.members_[ep(9001)] = std::move(dead);

        // Left member far past timeout.
        Member left;
        left.endpoint = ep(9002);
        left.status = MemberStatus::Left;
        left.last_seen = ancient;
        gm.members_[ep(9002)] = std::move(left);

        // Alive member (should not be purged).
        Member alive = make_member(ep(9003), "h", MemberStatus::Alive, 1);
        gm.members_[ep(9003)] = std::move(alive);

        gm.purge_dead_tombstones();

        assert(gm.members_.find(ep(9001)) == gm.members_.end()); // dead purged
        assert(gm.members_.find(ep(9002)) == gm.members_.end()); // left purged
        assert(gm.members_.find(ep(9003)) != gm.members_.end()); // alive kept
    }

    // ---- Test 14: Wire encode/decode roundtrip — Ping with piggyback ------
    {
        GossipConfig cfg;
        cfg.local_state.endpoint = ep(9000);
        GossipMembership gm(cfg, nullptr);
        gm.incarnation_ = 42;

        std::vector<PiggybackEntry> pb;
        PiggybackEntry e;
        e.type = PiggybackType::Alive;
        e.endpoint = ep(9001);
        e.incarnation = 5;
        pb.push_back(e);

        StreamBuffer encoded =
            gm.encode_message(GossipMessageType::Ping, gm.incarnation_, 1,
                              ep(9000), pb);

        GossipMessageType out_type;
        EndPoint out_sender;
        uint64_t out_inc;
        uint32_t out_seq;
        EndPoint out_ping_target;
        std::vector<PiggybackEntry> out_pb;

        bool ok = gm.decode_message(encoded, out_type, out_sender, out_inc,
                                    out_seq, out_ping_target, out_pb);
        assert(ok);
        assert(out_type == GossipMessageType::Ping);
        assert(out_sender == ep(9000));
        assert(out_inc == 42);
        assert(out_seq == 1);
        assert(out_pb.size() == 1);
        assert(out_pb[0].type == PiggybackType::Alive);
        assert(out_pb[0].endpoint == ep(9001));
        assert(out_pb[0].incarnation == 5);
    }

    // ---- Test 15: Metadata piggyback encode/decode roundtrip --------------
    {
        GossipConfig cfg;
        cfg.local_state.endpoint = ep(9000);
        GossipMembership gm(cfg, nullptr);
        gm.incarnation_ = 42;

        std::vector<PiggybackEntry> pb;
        PiggybackEntry meta;
        meta.type = PiggybackType::Metadata;
        meta.endpoint = ep(9000);
        meta.incarnation = 42;
        meta.actor_types = {"actorType1", "actorType2"};
        meta.load = 75;
        AcceptorInfo acc;
        acc.port = 9000;
        acc.handshake_version = 1;
        acc.protocol_version = 1;
        acc.tls_required = false;
        meta.acceptors.push_back(acc);
        pb.push_back(meta);

        StreamBuffer encoded =
            gm.encode_message(GossipMessageType::Ping, gm.incarnation_, 1,
                              ep(9000), pb);

        GossipMessageType out_type;
        EndPoint out_sender;
        uint64_t out_inc;
        uint32_t out_seq;
        EndPoint out_ping_target;
        std::vector<PiggybackEntry> out_pb;

        bool ok = gm.decode_message(encoded, out_type, out_sender, out_inc,
                                    out_seq, out_ping_target, out_pb);
        assert(ok);
        assert(out_pb.size() == 1);
        assert(out_pb[0].type == PiggybackType::Metadata);
        assert(out_pb[0].incarnation == 42);
        assert(out_pb[0].load == 75);
        assert(out_pb[0].actor_types.size() == 2);
        assert(out_pb[0].actor_types[0] == "actorType1");
        assert(out_pb[0].actor_types[1] == "actorType2");
        assert(out_pb[0].acceptors.size() == 1);
        assert(out_pb[0].acceptors[0].port == 9000);
        assert(out_pb[0].acceptors[0].handshake_version == 1);
        assert(out_pb[0].acceptors[0].protocol_version == 1);
        assert(out_pb[0].acceptors[0].tls_required == false);
    }

    std::printf("All gossip membership tests passed\n");
    return 0;
}
