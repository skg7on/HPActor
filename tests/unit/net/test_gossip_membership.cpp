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
    EXPECT_EQ(gm.udp_socket_, -1);
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

} // namespace hpactor::net
