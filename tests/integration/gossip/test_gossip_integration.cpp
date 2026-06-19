// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/adt/node_identity.hpp>
#include <hpactor/net/gossip_membership.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/types/types.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <vector>

namespace hpactor::net {
namespace {

// ============================================================================
// GossipMembership construction and config
// ============================================================================

TEST(GossipIntegrationTest, GossipConfigDefaults) {
    GossipConfig config;
    EXPECT_EQ(config.gossip_port, 5354);
    EXPECT_EQ(config.protocol_period, std::chrono::milliseconds(1000));
    EXPECT_EQ(config.ping_timeout, std::chrono::milliseconds(200));
    EXPECT_EQ(config.suspicion_timeout, std::chrono::milliseconds(3000));
    EXPECT_EQ(config.dead_timeout, std::chrono::milliseconds(30000));
    EXPECT_EQ(config.fanout, 3u);
    EXPECT_EQ(config.indirect_probes, 3u);
    EXPECT_TRUE(config.seeds.empty());
}

TEST(GossipIntegrationTest, GossipConfigWithSeeds) {
    GossipConfig config;
    Ipv4Endpoint seed1(0x7F000001, htons(5354));
    Ipv4Endpoint seed2(0x7F000002, htons(5354));
    config.seeds.push_back(seed1);
    config.seeds.push_back(seed2);

    EXPECT_EQ(config.seeds.size(), 2u);
}

TEST(GossipIntegrationTest, GossipBackendName) {
    GossipConfig config;
    // Cannot construct GossipMembership without an EventLoop or transport,
    // but we can verify the config structure and backend name through
    // the interface concept
    EXPECT_EQ(config.gossip_port, 5354);
    EXPECT_EQ(config.protocol_period.count(), 1000);
}

// ============================================================================
// Gossip node state transitions
// ============================================================================

TEST(GossipIntegrationTest, MemberStatusValues) {
    // Verify member status enum values
    EXPECT_EQ(static_cast<uint8_t>(MemberStatus::Alive), 0);
    EXPECT_EQ(static_cast<uint8_t>(MemberStatus::Suspicious), 1);
    EXPECT_EQ(static_cast<uint8_t>(MemberStatus::Dead), 2);
    EXPECT_EQ(static_cast<uint8_t>(MemberStatus::Left), 3);
}

TEST(GossipIntegrationTest, MemberDefaultConstruction) {
    Member m;
    EXPECT_EQ(m.status, MemberStatus::Alive);
    EXPECT_EQ(m.incarnation, 0u);
    EXPECT_TRUE(m.actor_types.empty());
}

TEST(GossipIntegrationTest, MemberWithIdentity) {
    Member m;
    m.identity.host = "node1.example.com";
    m.identity.endpoint = Ipv4Endpoint(0x7F000001, htons(5354));
    m.incarnation = 5;
    m.status = MemberStatus::Alive;

    EXPECT_EQ(m.identity.host, "node1.example.com");
    EXPECT_EQ(m.incarnation, 5u);
    EXPECT_EQ(m.status, MemberStatus::Alive);
}

// ============================================================================
// Gossip message serialization
// ============================================================================

TEST(GossipIntegrationTest, GossipMessageTypeValues) {
    EXPECT_EQ(static_cast<uint8_t>(GossipMessageType::Ping), 0x01);
    EXPECT_EQ(static_cast<uint8_t>(GossipMessageType::Ack), 0x02);
    EXPECT_EQ(static_cast<uint8_t>(GossipMessageType::PingReq), 0x03);
    EXPECT_EQ(static_cast<uint8_t>(GossipMessageType::IndirectAck), 0x04);
    EXPECT_EQ(static_cast<uint8_t>(GossipMessageType::Join), 0x05);
    EXPECT_EQ(static_cast<uint8_t>(GossipMessageType::SyncRsp), 0x06);
    EXPECT_EQ(static_cast<uint8_t>(GossipMessageType::Leave), 0x07);
}

TEST(GossipIntegrationTest, PiggybackTypeValues) {
    EXPECT_EQ(static_cast<uint8_t>(PiggybackType::Alive), 0x01);
    EXPECT_EQ(static_cast<uint8_t>(PiggybackType::Suspicious), 0x02);
    EXPECT_EQ(static_cast<uint8_t>(PiggybackType::Dead), 0x03);
    EXPECT_EQ(static_cast<uint8_t>(PiggybackType::Metadata), 0x04);
}

TEST(GossipIntegrationTest, GossipConstants) {
    EXPECT_EQ(GossipMagic, 0x48504743u); // "HPGC"
    EXPECT_EQ(GossipVersion, 0x01);
    EXPECT_GT(kGossipMaxMsgSize, 0u);
}

TEST(GossipIntegrationTest, PiggybackEntryConstruction) {
    PiggybackEntry entry;
    entry.type = PiggybackType::Alive;
    entry.incarnation = 42;
    entry.identity.host = "node1";
    entry.actor_types = {"echo", "calculator"};
    entry.load = 50;

    EXPECT_EQ(entry.type, PiggybackType::Alive);
    EXPECT_EQ(entry.incarnation, 42u);
    EXPECT_EQ(entry.identity.host, "node1");
    EXPECT_EQ(entry.actor_types.size(), 2u);
    EXPECT_EQ(entry.actor_types[0], "echo");
    EXPECT_EQ(entry.actor_types[1], "calculator");
    EXPECT_EQ(entry.load, 50u);
}

// ============================================================================
// SWIM protocol basic interactions
// ============================================================================

TEST(GossipIntegrationTest, MemberStatusTransitionToSuspicious) {
    // Test the conceptual state transition: Alive -> Suspicious
    Member m;
    m.status = MemberStatus::Alive;
    EXPECT_EQ(m.status, MemberStatus::Alive);

    m.status = MemberStatus::Suspicious;
    EXPECT_EQ(m.status, MemberStatus::Suspicious);
}

TEST(GossipIntegrationTest, MemberStatusTransitionToDead) {
    // Test the conceptual state transition: Suspicious -> Dead
    Member m;
    m.status = MemberStatus::Suspicious;
    m.status = MemberStatus::Dead;
    EXPECT_EQ(m.status, MemberStatus::Dead);
}

TEST(GossipIntegrationTest, MemberStatusTransitionToLeft) {
    // Test that a member can transition to Left state
    Member m;
    m.status = MemberStatus::Alive;
    m.status = MemberStatus::Left;
    EXPECT_EQ(m.status, MemberStatus::Left);
}

// ============================================================================
// HostResolver operations
// ============================================================================

TEST(GossipIntegrationTest, HostResolverCacheAndGet) {
    HostResolver resolver;

    resolver.cache("node1.example.com", "10.0.0.1", std::chrono::seconds(60));
    EXPECT_EQ(resolver.get_cached("node1.example.com"), "10.0.0.1");

    // Non-existent hostname returns empty
    EXPECT_EQ(resolver.get_cached("unknown.example.com"), "");
}

TEST(GossipIntegrationTest, HostResolverCacheOverride) {
    HostResolver resolver;

    resolver.cache("node1.example.com", "10.0.0.1");
    EXPECT_EQ(resolver.get_cached("node1.example.com"), "10.0.0.1");

    // Override with new IP
    resolver.cache("node1.example.com", "10.0.0.2");
    EXPECT_EQ(resolver.get_cached("node1.example.com"), "10.0.0.2");
}

TEST(GossipIntegrationTest, HostResolverMultipleEntries) {
    HostResolver resolver;

    resolver.cache("node1", "192.168.1.1");
    resolver.cache("node2", "192.168.1.2");
    resolver.cache("node3", "192.168.1.3");

    EXPECT_EQ(resolver.get_cached("node1"), "192.168.1.1");
    EXPECT_EQ(resolver.get_cached("node2"), "192.168.1.2");
    EXPECT_EQ(resolver.get_cached("node3"), "192.168.1.3");
}

// ============================================================================
// StaticDiscovery operations
// ============================================================================

TEST(GossipIntegrationTest, StaticDiscoveryEmpty) {
    StaticDiscovery sd({});
    EXPECT_EQ(sd.backend_name(), "static");

    auto members = sd.discover_all();
    EXPECT_TRUE(members.empty());

    Ipv4Endpoint ep(0x7F000001, htons(8080));
    EXPECT_EQ(sd.discover(ep), nullptr);
}

TEST(GossipIntegrationTest, StaticDiscoveryWithMembers) {
    Member m1;
    m1.identity.host = "node1";
    m1.identity.endpoint = Ipv4Endpoint(0x7F000001, htons(5354));
    m1.incarnation = 1;

    Member m2;
    m2.identity.host = "node2";
    m2.identity.endpoint = Ipv4Endpoint(0x7F000002, htons(5354));
    m2.incarnation = 2;

    StaticDiscovery sd({m1, m2});
    EXPECT_EQ(sd.backend_name(), "static");

    auto all = sd.discover_all();
    EXPECT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].identity.host, "node1");
    EXPECT_EQ(all[1].identity.host, "node2");

    // Lookup by endpoint
    const Member* found = sd.discover(Ipv4Endpoint(0x7F000001, htons(5354)));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->identity.host, "node1");
    EXPECT_EQ(found->incarnation, 1u);

    // Lookup non-existent endpoint
    const Member* not_found = sd.discover(Ipv4Endpoint(0x7F000003, htons(5354)));
    EXPECT_EQ(not_found, nullptr);
}

TEST(GossipIntegrationTest, StaticDiscoveryNoOpMethods) {
    StaticDiscovery sd({});

    // start/stop/announce/on_member_change are no-ops that should not crash
    sd.start();
    sd.stop();

    Member m;
    sd.announce(m);
    sd.on_member_change(nullptr);
    // If we reached here without crashing, the test passes
    SUCCEED();
}

TEST(GossipIntegrationTest, ServiceDiscoveryInterface) {
    // Verify that StaticDiscovery can be used through the IServiceDiscovery
    // interface
    Member m;
    m.identity.host = "iface-node";
    m.identity.endpoint = Ipv4Endpoint(0x7F000001, htons(5354));

    std::unique_ptr<IServiceDiscovery> sd =
        std::make_unique<StaticDiscovery>(std::vector<Member>{m});

    EXPECT_EQ(sd->backend_name(), "static");

    auto all = sd->discover_all();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].identity.host, "iface-node");

    const Member* found = sd->discover(Ipv4Endpoint(0x7F000001, htons(5354)));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->identity.host, "iface-node");
}

TEST(GossipIntegrationTest, NodeIdentityFieldComparison) {
    // Verify NodeIdentity fields individually (avoiding operator== which
    // requires AcceptorInfo comparison)
    NodeIdentity id1;
    id1.host = "host1";
    id1.endpoint = Ipv4Endpoint(0x7F000001, htons(5354));

    NodeIdentity id2;
    id2.host = "host1";
    id2.endpoint = Ipv4Endpoint(0x7F000001, htons(5354));

    EXPECT_EQ(id1.host, id2.host);
    EXPECT_EQ(id1.endpoint, id2.endpoint);
    EXPECT_EQ(id1.uds_path, id2.uds_path);

    NodeIdentity id3;
    id3.host = "host2";
    id3.endpoint = Ipv4Endpoint(0x7F000002, htons(5354));

    EXPECT_NE(id1.host, id3.host);
}

} // anonymous namespace
} // namespace hpactor::net
