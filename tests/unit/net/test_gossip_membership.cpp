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
// Uses FRIEND_TEST declarations in the header instead of #define private
// public.

#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/net/gossip_membership.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

// Helpers
static EndPoint ep(uint16_t port) {
    return Ipv4Endpoint{0x7F000001, htons(port)};
}

static Member
make_member(EndPoint e, const char* host, MemberStatus st, uint64_t inc) {
    Member m;
    m.identity.endpoint = e;
    if (host)
        m.identity.host = host;
    m.status = st;
    m.incarnation = inc;
    m.last_seen = std::chrono::steady_clock::now();
    return m;
}

namespace hpactor::net {

class GossipMembershipTest : public ::testing::Test {
  protected:
    GossipMembershipTest() {}
};

TEST_F(GossipMembershipTest, ConstructionDefaults) {
    GossipConfig cfg;
    GossipMembership gm(cfg, nullptr);

    EXPECT_EQ(gm.backend_name(), "gossip");
    EXPECT_EQ(gm.config_.gossip_port, 5354u);
    EXPECT_EQ(gm.config_.protocol_period.count(), 1000);
    EXPECT_EQ(gm.config_.ping_timeout.count(), 200);
    EXPECT_EQ(gm.config_.suspicion_timeout.count(), 3000);
    EXPECT_EQ(gm.config_.dead_timeout.count(), 30000);
    EXPECT_EQ(gm.config_.fanout, 3u);
    EXPECT_EQ(gm.config_.indirect_probes, 3u);
    EXPECT_TRUE(gm.config_.seeds.empty());
    EXPECT_EQ(gm.incarnation_, 1u);
    EXPECT_FALSE(gm.needs_dissemination_);
    EXPECT_TRUE(gm.members_.empty());
    EXPECT_EQ(gm.transport_.get(), nullptr);
}

TEST_F(GossipMembershipTest, BootstrapSoloCluster) {
    GossipConfig cfg;
    EndPoint self_ep = ep(9000);
    cfg.local_state.identity.endpoint = self_ep;
    cfg.local_state.identity.host = "127.0.0.1";
    GossipMembership gm(cfg, nullptr);

    gm.incarnation_ = 1;
    Member self =
        make_member(self_ep, "127.0.0.1", MemberStatus::Alive, gm.incarnation_);
    gm.members_[self_ep] = std::move(self);

    auto all = gm.discover_all();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].status, MemberStatus::Alive);
    EXPECT_EQ(all[0].identity.endpoint, self_ep);

    const auto* found = gm.discover(self_ep);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->status, MemberStatus::Alive);

    EXPECT_EQ(gm.discover(ep(9999)), nullptr);
}

TEST_F(GossipMembershipTest, AnnounceBumpsIncarnation) {
    GossipConfig cfg;
    EndPoint self_ep = ep(9000);
    cfg.local_state.identity.endpoint = self_ep;
    cfg.local_state.identity.host = "original-host";
    GossipMembership gm(cfg, nullptr);
    gm.incarnation_ = 100;
    gm.needs_dissemination_ = false;

    Member ann;
    ann.identity.endpoint = self_ep;
    ann.identity.host = "updated-host";
    gm.announce(std::move(ann));

    EXPECT_GT(gm.incarnation_, 100u);
    EXPECT_TRUE(gm.needs_dissemination_);
    EXPECT_EQ(gm.config_.local_state.incarnation, gm.incarnation_);
    EXPECT_EQ(gm.config_.local_state.identity.host, "updated-host");
}

TEST_F(GossipMembershipTest, DiscoverAllReturnsCopy) {
    GossipConfig cfg;
    EndPoint self_ep = ep(9000);
    cfg.local_state.identity.endpoint = self_ep;
    GossipMembership gm(cfg, nullptr);

    Member self;
    self.identity.endpoint = self_ep;
    self.status = MemberStatus::Alive;
    self.incarnation = 1;
    gm.members_[self_ep] = self;

    auto all = gm.discover_all();
    ASSERT_EQ(all.size(), 1u);

    all[0].status = MemberStatus::Dead;
    EXPECT_EQ(gm.members_[self_ep].status, MemberStatus::Alive);

    const auto* via_discover = gm.discover(self_ep);
    ASSERT_NE(via_discover, nullptr);
    EXPECT_EQ(via_discover->status, MemberStatus::Alive);
}

TEST_F(GossipMembershipTest, MergeMemberNoneToAlive) {
    GossipConfig cfg;
    EndPoint self_ep = ep(9000);
    cfg.local_state.identity.endpoint = self_ep;
    GossipMembership gm(cfg, nullptr);
    gm.members_[self_ep] = Member{};

    EndPoint alice = ep(9001);
    Member remote = make_member(alice, "10.0.0.1", MemberStatus::Alive, 5);
    gm.merge_member(remote);

    EXPECT_EQ(gm.members_.size(), 2u);
    auto it = gm.members_.find(alice);
    ASSERT_NE(it, gm.members_.end());
    EXPECT_EQ(it->second.status, MemberStatus::Alive);
    EXPECT_EQ(it->second.incarnation, 5u);
    EXPECT_EQ(it->second.identity.host, "10.0.0.1");
}

TEST_F(GossipMembershipTest, MergeMemberUpdateHigher) {
    GossipConfig cfg;
    EndPoint self_ep = ep(9000);
    cfg.local_state.identity.endpoint = self_ep;
    GossipMembership gm(cfg, nullptr);
    gm.members_[self_ep] = Member{};

    EndPoint alice = ep(9001);
    Member first = make_member(alice, "host-a", MemberStatus::Alive, 5);
    gm.merge_member(first);

    Member second = make_member(alice, "host-b", MemberStatus::Alive, 10);
    gm.merge_member(second);

    EXPECT_EQ(gm.members_[alice].incarnation, 10u);
    EXPECT_EQ(gm.members_[alice].identity.host, "host-b");
}

TEST_F(GossipMembershipTest, MergeMemberIgnoreLower) {
    GossipConfig cfg;
    EndPoint self_ep = ep(9000);
    cfg.local_state.identity.endpoint = self_ep;
    GossipMembership gm(cfg, nullptr);
    gm.members_[self_ep] = Member{};

    EndPoint alice = ep(9001);
    Member first = make_member(alice, "original", MemberStatus::Alive, 10);
    gm.merge_member(first);

    Member stale;
    stale.identity.endpoint = alice;
    stale.incarnation = 5;
    stale.status = MemberStatus::Suspicious;
    stale.identity.host = "stale";
    gm.merge_member(stale);

    EXPECT_EQ(gm.members_[alice].incarnation, 10u);
    EXPECT_EQ(gm.members_[alice].status, MemberStatus::Alive);
    EXPECT_EQ(gm.members_[alice].identity.host, "original");
}

TEST_F(GossipMembershipTest, MergeMemberDeadToAlive) {
    GossipConfig cfg;
    EndPoint self_ep = ep(9000);
    cfg.local_state.identity.endpoint = self_ep;
    GossipMembership gm(cfg, nullptr);
    gm.members_[self_ep] = Member{};

    EndPoint alice = ep(9001);
    Member first = make_member(alice, "host-a", MemberStatus::Alive, 10);
    gm.merge_member(first);
    gm.members_[alice].status = MemberStatus::Dead;

    Member re = make_member(alice, "host-b", MemberStatus::Alive, 20);
    gm.merge_member(re);

    EXPECT_EQ(gm.members_[alice].status, MemberStatus::Alive);
    EXPECT_EQ(gm.members_[alice].incarnation, 20u);
    EXPECT_EQ(gm.members_[alice].identity.host, "host-b");
}

TEST_F(GossipMembershipTest, MarkSuspiciousDeadTransitions) {
    GossipConfig cfg;
    EndPoint self_ep = ep(9000);
    cfg.local_state.identity.endpoint = self_ep;
    GossipMembership gm(cfg, nullptr);
    gm.members_[self_ep] = Member{};

    EndPoint alice = ep(9001);
    gm.merge_member(make_member(alice, "h", MemberStatus::Alive, 1));

    gm.mark_suspicious(alice);
    EXPECT_EQ(gm.members_[alice].status, MemberStatus::Suspicious);

    gm.mark_dead(alice);
    EXPECT_EQ(gm.members_[alice].status, MemberStatus::Dead);
}

TEST_F(GossipMembershipTest, PickRandomPeersAllAvailable) {
    GossipConfig cfg;
    cfg.local_state.identity.endpoint = ep(9000);
    GossipMembership gm(cfg, nullptr);
    Member self;
    self.identity.endpoint = ep(9000);
    self.status = MemberStatus::Alive;
    gm.members_[ep(9000)] = self;

    for (uint16_t p = 9001; p <= 9003; ++p) {
        Member peer;
        peer.identity.endpoint = ep(p);
        peer.status = MemberStatus::Alive;
        gm.members_[ep(p)] = std::move(peer);
    }

    auto peers = gm.pick_random_peers(10, {});
    EXPECT_EQ(peers.size(), 3u);
}

TEST_F(GossipMembershipTest, PickRandomPeersEmptySolo) {
    GossipConfig cfg;
    cfg.local_state.identity.endpoint = ep(9000);
    GossipMembership gm(cfg, nullptr);
    Member self;
    self.identity.endpoint = ep(9000);
    self.status = MemberStatus::Alive;
    gm.members_[ep(9000)] = self;

    auto peers = gm.pick_random_peers(3, {});
    EXPECT_TRUE(peers.empty());
}

TEST_F(GossipMembershipTest, PurgeDeadTombstones) {
    GossipConfig cfg;
    cfg.dead_timeout = std::chrono::milliseconds(50);
    cfg.local_state.identity.endpoint = ep(9000);
    GossipMembership gm(cfg, nullptr);
    gm.members_[ep(9000)] = Member{};

    auto ancient = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    Member dead;
    dead.identity.endpoint = ep(9001);
    dead.status = MemberStatus::Dead;
    dead.last_seen = ancient;
    gm.members_[ep(9001)] = std::move(dead);

    Member left;
    left.identity.endpoint = ep(9002);
    left.status = MemberStatus::Left;
    left.last_seen = ancient;
    gm.members_[ep(9002)] = std::move(left);

    Member alive = make_member(ep(9003), "h", MemberStatus::Alive, 1);
    gm.members_[ep(9003)] = std::move(alive);

    gm.purge_dead_tombstones();

    EXPECT_EQ(gm.members_.find(ep(9001)), gm.members_.end());
    EXPECT_EQ(gm.members_.find(ep(9002)), gm.members_.end());
    EXPECT_NE(gm.members_.find(ep(9003)), gm.members_.end());
}

TEST_F(GossipMembershipTest, WireEncodeDecodePing) {
    GossipConfig cfg;
    cfg.local_state.identity.endpoint = ep(9000);
    GossipMembership gm(cfg, nullptr);
    gm.incarnation_ = 42;

    std::vector<PiggybackEntry> pb;
    PiggybackEntry e;
    e.type = PiggybackType::Alive;
    e.identity.endpoint = ep(9001);
    e.incarnation = 5;
    pb.push_back(e);

    StreamBuffer encoded = gm.encode_message(GossipMessageType::Ping,
                                             gm.incarnation_, 1, ep(9000), pb);

    GossipMessageType out_type;
    EndPoint out_sender;
    uint64_t out_inc;
    uint32_t out_seq;
    EndPoint out_ping_target;
    std::vector<PiggybackEntry> out_pb;

    bool ok = gm.decode_message(encoded, out_type, out_sender, out_inc, out_seq,
                                out_ping_target, out_pb);
    ASSERT_TRUE(ok);
    EXPECT_EQ(out_type, GossipMessageType::Ping);
    EXPECT_EQ(out_sender, ep(9000));
    EXPECT_EQ(out_inc, 42u);
    EXPECT_EQ(out_seq, 1u);
    ASSERT_EQ(out_pb.size(), 1u);
    EXPECT_EQ(out_pb[0].type, PiggybackType::Alive);
    EXPECT_EQ(out_pb[0].identity.endpoint, ep(9001));
    EXPECT_EQ(out_pb[0].incarnation, 5u);
}

TEST_F(GossipMembershipTest, WireEncodeDecodeMetadata) {
    GossipConfig cfg;
    cfg.local_state.identity.endpoint = ep(9000);
    GossipMembership gm(cfg, nullptr);
    gm.incarnation_ = 42;

    std::vector<PiggybackEntry> pb;
    PiggybackEntry meta;
    meta.type = PiggybackType::Metadata;
    meta.identity.endpoint = ep(9000);
    meta.incarnation = 42;
    meta.actor_types = {"actorType1", "actorType2"};
    meta.load = 75;
    AcceptorInfo acc;
    acc.port = 9000;
    acc.handshake_version = 1;
    acc.protocol_version = 1;
    acc.tls_required = false;
    meta.identity.acceptors.push_back(acc);
    pb.push_back(meta);

    StreamBuffer encoded = gm.encode_message(GossipMessageType::Ping,
                                             gm.incarnation_, 1, ep(9000), pb);

    GossipMessageType out_type;
    EndPoint out_sender;
    uint64_t out_inc;
    uint32_t out_seq;
    EndPoint out_ping_target;
    std::vector<PiggybackEntry> out_pb;

    bool ok = gm.decode_message(encoded, out_type, out_sender, out_inc, out_seq,
                                out_ping_target, out_pb);
    ASSERT_TRUE(ok);
    ASSERT_EQ(out_pb.size(), 1u);
    EXPECT_EQ(out_pb[0].type, PiggybackType::Metadata);
    EXPECT_EQ(out_pb[0].incarnation, 42u);
    EXPECT_EQ(out_pb[0].load, 75u);
    ASSERT_EQ(out_pb[0].actor_types.size(), 2u);
    EXPECT_EQ(out_pb[0].actor_types[0], "actorType1");
    EXPECT_EQ(out_pb[0].actor_types[1], "actorType2");
    ASSERT_EQ(out_pb[0].identity.acceptors.size(), 1u);
    EXPECT_EQ(out_pb[0].identity.acceptors[0].port, 9000u);
    EXPECT_EQ(out_pb[0].identity.acceptors[0].handshake_version, 1u);
    EXPECT_EQ(out_pb[0].identity.acceptors[0].protocol_version, 1u);
    EXPECT_FALSE(out_pb[0].identity.acceptors[0].tls_required);
}

// ── Integration tests (protocol flow with FakeUdpTransport) ──────

class GossipProtocolIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fc_.install();
    }
    void TearDown() override {
        fc_.remove();
    }

    // Helper: create a GossipConfig for a node at the given port.
    static GossipConfig cfg_for(uint16_t port, const char* host = "127.0.0.1") {
        GossipConfig cfg;
        cfg.gossip_port = port;
        cfg.local_state.identity.endpoint = ep(port);
        cfg.local_state.identity.host = host;
        cfg.protocol_period = std::chrono::milliseconds(100);
        cfg.ping_timeout = std::chrono::milliseconds(50);
        cfg.suspicion_timeout = std::chrono::milliseconds(200);
        cfg.dead_timeout = std::chrono::milliseconds(1000);
        return cfg;
    }

    fault::FaultController fc_;
};

TEST_F(GossipProtocolIntegrationTest, JoinFlow) {
    // Helper lambda: route all packets from `from`'s transport to `to`'s
    // handle_packet.  Defined in the test body so it executes with friend
    // access (FRIEND_TEST grants friendship to the GTest-generated test
    // class, not the fixture).
    auto deliver_packets = [](GossipMembership& from, GossipMembership& to,
                              const std::string& src_host = "127.0.0.1",
                              uint16_t src_port = 9000) {
        auto* t = static_cast<FakeUdpTransport*>(from.transport_.get());
        for (const auto& pkt : t->sent_packets) {
            to.handle_packet(pkt.data, src_host, src_port);
        }
        t->clear_sent();
    };

    // Node A: solo node (seed)
    auto cfg_a = cfg_for(9000);
    auto transport_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(transport_a));
    // Manually bootstrap A (like start() does, but without EventLoop)
    node_a.incarnation_ = 100;
    Member self_a;
    self_a.identity.endpoint = ep(9000);
    self_a.incarnation = 100;
    self_a.status = MemberStatus::Alive;
    self_a.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = std::move(self_a);

    // Node B: joins via A as seed
    auto cfg_b = cfg_for(9001);
    cfg_b.seeds.push_back(ep(9000));
    auto transport_b = std::make_unique<FakeUdpTransport>();
    GossipMembership node_b(cfg_b, std::move(transport_b));
    node_b.incarnation_ = 200;

    // B sends Join to A
    node_b.send_join(ep(9000));

    // Deliver B's Join to A
    deliver_packets(node_b, node_a);

    // A should now know about B (via merge in handle_join)
    EXPECT_EQ(node_a.members_.size(), 2u);
    EXPECT_NE(node_a.members_.find(ep(9001)), node_a.members_.end());
    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Alive);

    // A should have sent a SyncRsp back to B
    auto* ta = static_cast<FakeUdpTransport*>(node_a.transport_.get());
    ASSERT_FALSE(ta->sent_packets.empty());

    // Deliver A's SyncRsp to B
    deliver_packets(node_a, node_b);

    // B should now know about A
    EXPECT_EQ(node_b.members_.size(), 2u);
    EXPECT_NE(node_b.members_.find(ep(9000)), node_b.members_.end());
    EXPECT_EQ(node_b.members_[ep(9000)].status, MemberStatus::Alive);
}

TEST_F(GossipProtocolIntegrationTest, ProtocolRoundPingAck) {
    auto deliver = [](GossipMembership& from, GossipMembership& to) {
        auto* t = static_cast<FakeUdpTransport*>(from.transport_.get());
        for (const auto& pkt : t->sent_packets) {
            to.handle_packet(pkt.data, "127.0.0.1", 9000);
        }
        t->clear_sent();
    };

    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    auto cfg_b = cfg_for(9001);
    auto t_b = std::make_unique<FakeUdpTransport>();
    GossipMembership node_b(cfg_b, std::move(t_b));
    node_b.incarnation_ = 200;

    // Both know each other
    for (uint16_t p = 9000; p <= 9001; ++p) {
        Member m;
        m.identity.endpoint = ep(p);
        m.status = MemberStatus::Alive;
        m.incarnation = 100 + (p - 9000) * 100;
        m.last_seen = std::chrono::steady_clock::now();
        node_a.members_[ep(p)] = m;
        node_b.members_[ep(p)] = m;
    }

    node_a.protocol_round();
    EXPECT_NE(node_a.pending_pings_.find(ep(9001)), node_a.pending_pings_.end());

    deliver(node_a, node_b);
    auto* tb = static_cast<FakeUdpTransport*>(node_b.transport_.get());
    ASSERT_FALSE(tb->sent_packets.empty());

    deliver(node_b, node_a);
    EXPECT_EQ(node_a.pending_pings_.find(ep(9001)), node_a.pending_pings_.end());
}

TEST_F(GossipProtocolIntegrationTest, IndirectProbePingReq) {
    auto deliver = [](GossipMembership& from, GossipMembership& to) {
        auto* t = static_cast<FakeUdpTransport*>(from.transport_.get());
        for (const auto& pkt : t->sent_packets) {
            to.handle_packet(pkt.data, "127.0.0.1", 9000);
        }
        t->clear_sent();
    };

    // 3 nodes: A (requester), B (proxy), C (target)
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    auto cfg_b = cfg_for(9001);
    auto t_b = std::make_unique<FakeUdpTransport>();
    GossipMembership node_b(cfg_b, std::move(t_b));
    node_b.incarnation_ = 200;

    auto cfg_c = cfg_for(9002);
    auto t_c = std::make_unique<FakeUdpTransport>();
    GossipMembership node_c(cfg_c, std::move(t_c));
    node_c.incarnation_ = 300;

    // All three know each other as Alive
    for (auto* n : {&node_a, &node_b, &node_c}) {
        for (uint16_t p = 9000; p <= 9002; ++p) {
            Member m;
            m.identity.endpoint = ep(p);
            m.status = MemberStatus::Alive;
            m.incarnation = 100 + (p - 9000) * 100;
            m.last_seen = std::chrono::steady_clock::now();
            n->members_[ep(p)] = std::move(m);
        }
    }

    // A has an expired pending ping for C (first expiry)
    auto now = std::chrono::steady_clock::now();
    node_a.pending_pings_[ep(9002)] =
        PendingPing{now - std::chrono::milliseconds(100), false, {}};

    node_a.protocol_round();

    auto* ta = static_cast<FakeUdpTransport*>(node_a.transport_.get());
    ASSERT_FALSE(ta->sent_packets.empty());

    deliver(node_a, node_b);

    auto* tb = static_cast<FakeUdpTransport*>(node_b.transport_.get());
    ASSERT_FALSE(tb->sent_packets.empty());

    deliver(node_b, node_c);

    auto* tc = static_cast<FakeUdpTransport*>(node_c.transport_.get());
    ASSERT_FALSE(tc->sent_packets.empty());

    deliver(node_c, node_b);

    ASSERT_FALSE(tb->sent_packets.empty());

    deliver(node_b, node_a);

    EXPECT_EQ(node_a.pending_pings_.find(ep(9002)), node_a.pending_pings_.end());
    EXPECT_EQ(node_a.members_[ep(9002)].status, MemberStatus::Alive);
}

TEST_F(GossipProtocolIntegrationTest, FailureDetectionSuspicious) {
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    for (uint16_t p = 9000; p <= 9001; ++p) {
        Member m;
        m.identity.endpoint = ep(p);
        m.status = MemberStatus::Alive;
        m.incarnation = 100;
        m.last_seen = std::chrono::steady_clock::now();
        node_a.members_[ep(p)] = std::move(m);
    }

    // Expired pending ping for B in a 2-node cluster (no indirect probes
    // available)
    auto now = std::chrono::steady_clock::now();
    node_a.pending_pings_[ep(9001)] =
        PendingPing{now - std::chrono::milliseconds(100), false, {}};

    node_a.protocol_round();

    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Suspicious);
    EXPECT_EQ(node_a.pending_pings_.find(ep(9001)), node_a.pending_pings_.end());
}

TEST_F(GossipProtocolIntegrationTest, FailureDetectionDead) {
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    Member self;
    self.identity.endpoint = ep(9000);
    self.status = MemberStatus::Alive;
    self.incarnation = 100;
    self.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = std::move(self);

    Member suspicious;
    suspicious.identity.endpoint = ep(9001);
    suspicious.status = MemberStatus::Suspicious;
    suspicious.incarnation = 200;
    suspicious.last_seen =
        std::chrono::steady_clock::now() - std::chrono::seconds(10);
    node_a.members_[ep(9001)] = std::move(suspicious);

    bool callback_fired = false;
    Member callback_member;
    node_a.on_member_change([&](const Member& m, bool) {
        callback_fired = true;
        callback_member = m;
    });

    node_a.protocol_round();

    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Dead);
    EXPECT_TRUE(callback_fired);
    EXPECT_EQ(callback_member.identity.endpoint, ep(9001));
}

TEST_F(GossipProtocolIntegrationTest, LeavePropagation) {
    auto deliver = [](GossipMembership& from, GossipMembership& to) {
        auto* t = static_cast<FakeUdpTransport*>(from.transport_.get());
        for (const auto& pkt : t->sent_packets) {
            to.handle_packet(pkt.data, "127.0.0.1", 9000);
        }
        t->clear_sent();
    };

    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    auto cfg_b = cfg_for(9001);
    auto t_b = std::make_unique<FakeUdpTransport>();
    GossipMembership node_b(cfg_b, std::move(t_b));
    node_b.incarnation_ = 200;

    for (uint16_t p = 9000; p <= 9001; ++p) {
        Member m;
        m.identity.endpoint = ep(p);
        m.status = MemberStatus::Alive;
        m.incarnation = 100 + (p - 9000) * 100;
        m.last_seen = std::chrono::steady_clock::now();
        node_a.members_[ep(p)] = m;
        node_b.members_[ep(p)] = m;
    }

    bool callback_fired = false;
    node_a.on_member_change([&](const Member&, bool) { callback_fired = true; });

    node_b.send_leave(ep(9000));
    deliver(node_b, node_a);

    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Left);
    EXPECT_TRUE(callback_fired);
}

TEST_F(GossipProtocolIntegrationTest, PiggybackDissemination) {
    auto deliver = [](GossipMembership& from, GossipMembership& to) {
        auto* t = static_cast<FakeUdpTransport*>(from.transport_.get());
        for (const auto& pkt : t->sent_packets) {
            to.handle_packet(pkt.data, "127.0.0.1", 9000);
        }
        t->clear_sent();
    };

    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    auto cfg_c = cfg_for(9002);
    auto t_c = std::make_unique<FakeUdpTransport>();
    GossipMembership node_c(cfg_c, std::move(t_c));
    node_c.incarnation_ = 300;

    // A knows: self (Alive), B (Suspicious), C (Alive)
    Member self_a;
    self_a.identity.endpoint = ep(9000);
    self_a.status = MemberStatus::Alive;
    self_a.incarnation = 100;
    self_a.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = self_a;

    Member susp_b;
    susp_b.identity.endpoint = ep(9001);
    susp_b.status = MemberStatus::Suspicious;
    susp_b.incarnation = 200;
    susp_b.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9001)] = susp_b;

    Member alive_c;
    alive_c.identity.endpoint = ep(9002);
    alive_c.status = MemberStatus::Alive;
    alive_c.incarnation = 300;
    alive_c.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9002)] = alive_c;

    // C knows: self (Alive), A (Alive) — but NOT B
    Member self_c;
    self_c.identity.endpoint = ep(9002);
    self_c.status = MemberStatus::Alive;
    self_c.incarnation = 300;
    self_c.last_seen = std::chrono::steady_clock::now();
    node_c.members_[ep(9002)] = self_c;

    Member alive_a;
    alive_a.identity.endpoint = ep(9000);
    alive_a.status = MemberStatus::Alive;
    alive_a.incarnation = 100;
    alive_a.last_seen = std::chrono::steady_clock::now();
    node_c.members_[ep(9000)] = alive_a;

    EXPECT_EQ(node_c.members_.find(ep(9001)), node_c.members_.end());

    node_a.protocol_round();
    deliver(node_a, node_c);

    auto it = node_c.members_.find(ep(9001));
    ASSERT_NE(it, node_c.members_.end());
    EXPECT_EQ(it->second.status, MemberStatus::Suspicious);
    EXPECT_EQ(it->second.incarnation, 200u);
}

TEST_F(GossipProtocolIntegrationTest, IncarnationConflictResolution) {
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    Member b_in_a;
    b_in_a.identity.endpoint = ep(9001);
    b_in_a.status = MemberStatus::Alive;
    b_in_a.incarnation = 10;
    b_in_a.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = Member{};
    node_a.members_[ep(9001)] = b_in_a;

    // Stale piggyback: inc=8 Dead → should be ignored
    std::vector<PiggybackEntry> stale;
    PiggybackEntry stale_e;
    stale_e.type = PiggybackType::Dead;
    stale_e.identity.endpoint = ep(9001);
    stale_e.incarnation = 8;
    stale.push_back(stale_e);
    node_a.apply_piggyback(stale);

    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Alive);
    EXPECT_EQ(node_a.members_[ep(9001)].incarnation, 10u);

    // Fresher piggyback: inc=12 Dead → should be accepted
    std::vector<PiggybackEntry> fresh;
    PiggybackEntry fresh_e;
    fresh_e.type = PiggybackType::Dead;
    fresh_e.identity.endpoint = ep(9001);
    fresh_e.incarnation = 12;
    fresh.push_back(fresh_e);
    node_a.apply_piggyback(fresh);

    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Dead);
    EXPECT_EQ(node_a.members_[ep(9001)].incarnation, 12u);
}

TEST_F(GossipProtocolIntegrationTest, MemberChangeCallback) {
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;
    node_a.members_[ep(9000)] = Member{};

    std::vector<std::pair<Member, bool>> events;
    node_a.on_member_change(
        [&](const Member& m, bool joined) { events.emplace_back(m, joined); });

    // Add B as Alive, then mark dead — callback fires for Dead
    Member b;
    b.identity.endpoint = ep(9001);
    b.status = MemberStatus::Alive;
    b.incarnation = 200;
    b.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9001)] = b;

    node_a.mark_dead(ep(9001));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].first.identity.endpoint, ep(9001));
    EXPECT_FALSE(events[0].second); // joined=false for Dead

    // Test leave callback
    node_a.handle_leave(ep(9001), 300);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].first.identity.endpoint, ep(9001));
    EXPECT_FALSE(events[1].second);
}

TEST_F(GossipProtocolIntegrationTest, TombstonePurging) {
    auto cfg_a = cfg_for(9000);
    cfg_a.dead_timeout = std::chrono::milliseconds(100);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    Member self;
    self.identity.endpoint = ep(9000);
    self.status = MemberStatus::Alive;
    self.incarnation = 100;
    self.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = self;

    // Ancient Dead member
    Member dead;
    dead.identity.endpoint = ep(9001);
    dead.status = MemberStatus::Dead;
    dead.incarnation = 200;
    dead.last_seen = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    node_a.members_[ep(9001)] = dead;

    EXPECT_EQ(node_a.members_.size(), 2u);

    node_a.protocol_round();

    EXPECT_EQ(node_a.members_.find(ep(9001)), node_a.members_.end());
    EXPECT_NE(node_a.members_.find(ep(9000)), node_a.members_.end());
    EXPECT_EQ(node_a.members_.size(), 1u);
}

TEST_F(GossipProtocolIntegrationTest, FaultInjectionPacketLoss) {
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    auto* t_a_raw = t_a.get();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    Member self;
    self.identity.endpoint = ep(9000);
    self.status = MemberStatus::Alive;
    self.incarnation = 100;
    self.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = self;

    Member peer;
    peer.identity.endpoint = ep(9001);
    peer.status = MemberStatus::Alive;
    peer.incarnation = 200;
    peer.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9001)] = peer;

    // Normal send — packet should appear in transport
    node_a.send_ping(ep(9001));
    EXPECT_FALSE(t_a_raw->sent_packets.empty());
    t_a_raw->clear_sent();

    // Enable the ping.drop fault point — requires a loaded schedule
    auto* fc = hpactor::fault::FaultController::instance();
    ASSERT_NE(fc, nullptr) << "FaultController not initialized";

    hpactor::fault::FaultSchedule schedule;
    hpactor::fault::add_entry_to(schedule, hpactor::fault::FaultDomain::kGossip, 1)
        .drop("hpactor.gossip.ping.drop");
    fc->load(schedule);
    fc->enable("hpactor.gossip.ping.drop");

    node_a.send_ping(ep(9001));
    EXPECT_TRUE(t_a_raw->sent_packets.empty());

    fc->disable();

    node_a.send_ping(ep(9001));
    EXPECT_FALSE(t_a_raw->sent_packets.empty());
}

TEST_F(GossipProtocolIntegrationTest, FailureDetectionEndToEnd) {
    // Deterministic end-to-end 3-node failure detection scenario.
    // Bootstraps 3 nodes with full mutual knowledge, then stops one
    // and verifies the other two detect it as Left via member-change
    // callbacks — all without real networking or timing assumptions.
    //
    // This replaces the flaky GossipSystem.FailureDetectionEndToEnd
    // system test which used real UDP sockets and assert_eventually
    // polling.

    // ── Create 3 nodes ────────────────────────────────────────────
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership na(cfg_a, std::move(t_a));
    na.incarnation_ = 100;

    auto cfg_b = cfg_for(9001);
    auto t_b = std::make_unique<FakeUdpTransport>();
    GossipMembership nb(cfg_b, std::move(t_b));
    nb.incarnation_ = 200;

    auto cfg_c = cfg_for(9002);
    auto t_c = std::make_unique<FakeUdpTransport>();
    GossipMembership nc(cfg_c, std::move(t_c));
    nc.incarnation_ = 300;

    // ── Bootstrap all 3 nodes with full mutual knowledge ──────────
    // This models the state after successful join + piggyback
    // dissemination protocol rounds have completed.
    for (uint16_t p = 9000; p <= 9002; ++p) {
        Member m;
        m.identity.endpoint = ep(p);
        m.status = MemberStatus::Alive;
        m.incarnation = 100 + (p - 9000) * 100;
        m.last_seen = std::chrono::steady_clock::now();
        na.members_[ep(p)] = m;
        nb.members_[ep(p)] = m;
        nc.members_[ep(p)] = m;
    }

    // ── Verify initial discovery ──────────────────────────────────
    EXPECT_EQ(na.members_.size(), 3u);
    EXPECT_EQ(nb.members_.size(), 3u);
    EXPECT_EQ(nc.members_.size(), 3u);
    EXPECT_EQ(na.members_[ep(9001)].status, MemberStatus::Alive);
    EXPECT_EQ(na.members_[ep(9002)].status, MemberStatus::Alive);
    EXPECT_EQ(nb.members_[ep(9000)].status, MemberStatus::Alive);
    EXPECT_EQ(nb.members_[ep(9002)].status, MemberStatus::Alive);
    EXPECT_EQ(nc.members_[ep(9000)].status, MemberStatus::Alive);
    EXPECT_EQ(nc.members_[ep(9001)].status, MemberStatus::Alive);

    // ── Phase 2: Node C leaves gracefully ─────────────────────────
    int leave_callbacks_a = 0;
    Member leave_member_a;
    na.on_member_change([&](const Member& m, bool) {
        leave_callbacks_a++;
        leave_member_a = m;
    });

    int leave_callbacks_b = 0;
    Member leave_member_b;
    nb.on_member_change([&](const Member& m, bool) {
        leave_callbacks_b++;
        leave_member_b = m;
    });

    // C sends Leave to A and B.  Snapshot the packets so we can
    // deliver them to both nodes (deliver clears after iterating).
    nc.send_leave(ep(9000));
    nc.send_leave(ep(9001));
    {
        auto* tc = static_cast<FakeUdpTransport*>(nc.transport_.get());
        auto pkts = std::move(tc->sent_packets);
        tc->sent_packets.clear();

        for (const auto& pkt : pkts) {
            na.handle_packet(pkt.data, "127.0.0.1", 9002);
            nb.handle_packet(pkt.data, "127.0.0.1", 9002);
        }
    }

    // ── Verify A and B see C as Left ──────────────────────────────
    EXPECT_EQ(na.members_[ep(9002)].status, MemberStatus::Left);
    EXPECT_EQ(nb.members_[ep(9002)].status, MemberStatus::Left);

    // A and B themselves should still be Alive.
    EXPECT_EQ(na.members_[ep(9000)].status, MemberStatus::Alive);
    EXPECT_EQ(nb.members_[ep(9001)].status, MemberStatus::Alive);

    // ── Verify member-change callbacks fired ──────────────────────
    // Both packets (C→A and C→B) are delivered to both nodes, so each
    // node receives two leave notices from C — 2 callbacks per node.
    EXPECT_GE(leave_callbacks_a, 1);
    EXPECT_EQ(leave_member_a.identity.endpoint, ep(9002));
    EXPECT_GE(leave_callbacks_b, 1);
    EXPECT_EQ(leave_member_b.identity.endpoint, ep(9002));
}

} // namespace hpactor::net
