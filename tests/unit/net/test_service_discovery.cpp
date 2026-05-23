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

#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/gossip_membership.hpp>
#include <hpactor/net/hybrid_discovery.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/static_discovery.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

static EndPoint ep(uint16_t port) {
    return Ipv4Endpoint{0x7F000001, htons(port)};
}

// Minimal concrete class that does not override raw_members()
struct TestDiscovery : IServiceDiscovery {
    void start() override {}
    void stop() override {}
    std::vector<Member> discover_all() const override {
        return {};
    }
    const Member* discover(EndPoint) const override {
        return nullptr;
    }
    void announce(Member) override {}
    void on_member_change(MemberChangeCallback) override {}
    std::string backend_name() const override {
        return "test";
    }
};

TEST(ServiceDiscoveryTest, DefaultRawMembersNull) {
    TestDiscovery td;
    EXPECT_EQ(td.raw_members(), nullptr);
}

TEST(ServiceDiscoveryTest, StaticDiscoveryDiscover) {
    auto ep1 = ep(9000);
    auto ep2 = ep(9001);
    std::vector<Member> members;
    Member m1;
    m1.identity.endpoint = ep1;
    m1.identity.host = "host-a";
    members.push_back(m1);
    Member m2;
    m2.identity.endpoint = ep2;
    m2.identity.host = "host-b";
    members.push_back(m2);

    StaticDiscovery sd(std::move(members));
    const auto* found = sd.discover(ep1);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->identity.endpoint, ep1);
    EXPECT_EQ(found->identity.host, "host-a");
}

TEST(ServiceDiscoveryTest, StaticDiscoveryDiscoverUnknown) {
    auto known = ep(9000);
    auto unknown = ep(9999);
    std::vector<Member> members;
    Member m;
    m.identity.endpoint = known;
    members.push_back(m);

    StaticDiscovery sd(std::move(members));
    EXPECT_EQ(sd.discover(unknown), nullptr);
}

TEST(ServiceDiscoveryTest, StaticDiscoveryDiscoverAll) {
    auto ep1 = ep(9000);
    auto ep2 = ep(9001);
    std::vector<Member> members;
    Member m1;
    m1.identity.endpoint = ep1;
    members.push_back(m1);
    Member m2;
    m2.identity.endpoint = ep2;
    members.push_back(m2);

    StaticDiscovery sd(std::move(members));
    auto all = sd.discover_all();
    EXPECT_EQ(all.size(), 2u);
}

TEST(ServiceDiscoveryTest, StaticDiscoveryBackendName) {
    StaticDiscovery sd({});
    EXPECT_EQ(sd.backend_name(), "static");
}

TEST(ServiceDiscoveryTest, ActorLocationCachePutGet) {
    ActorLocationCache cache;
    ActorId id(42);
    auto ep1 = ep(9000);

    cache.put(id, ep1);
    auto result = cache.get(id);
    ASSERT_TRUE(result.has_value());
    if (!result)
        GTEST_SKIP();
    EXPECT_EQ(*result, ep1);
}

TEST(ServiceDiscoveryTest, ActorLocationCacheExpiredEntry) {
    ActorLocationCache cache;
    ActorId id(42);
    auto ep1 = ep(9000);

    cache.put(id, ep1, std::chrono::seconds(-1));
    auto result = cache.get(id);
    EXPECT_FALSE(result.has_value());
}

TEST(ServiceDiscoveryTest, ActorLocationCacheEvict) {
    ActorLocationCache cache;
    ActorId id(42);
    auto ep1 = ep(9000);

    cache.put(id, ep1);
    ASSERT_TRUE(cache.get(id).has_value());

    cache.evict(id);
    EXPECT_FALSE(cache.get(id).has_value());
}

TEST(ServiceDiscoveryTest, ActorLocationCacheEvictNode) {
    ActorLocationCache cache;
    auto ep1 = ep(9000);
    auto ep2 = ep(9001);

    cache.put(ActorId(1), ep1);
    cache.put(ActorId(2), ep1);
    cache.put(ActorId(3), ep2);

    cache.evict_node(ep1);

    EXPECT_FALSE(cache.get(ActorId(1)).has_value());
    EXPECT_FALSE(cache.get(ActorId(2)).has_value());
    EXPECT_TRUE(cache.get(ActorId(3)).has_value());
}

TEST(ServiceDiscoveryTest, ActorLocationCachePurgeExpired) {
    ActorLocationCache cache;
    auto ep1 = ep(9000);

    cache.put(ActorId(1), ep1, std::chrono::seconds(-1));
    cache.put(ActorId(2), ep1, std::chrono::seconds(3600));

    cache.purge_expired();

    EXPECT_FALSE(cache.get(ActorId(1)).has_value());
    auto result = cache.get(ActorId(2));
    ASSERT_TRUE(result.has_value());
    if (!result)
        GTEST_SKIP();
    EXPECT_EQ(*result, ep1);
}

TEST(ServiceDiscoveryTest, UdpRegistrarBackendName) {
    RegistrarConfig rcfg;
    auto ep1 = ep(0);
    UdpRegistrar reg(rcfg, ep1, nullptr);
    EXPECT_EQ(reg.backend_name(), "udp-registrar");
}

TEST(ServiceDiscoveryTest, GossipMembershipBackendName) {
    GossipConfig gcfg;
    GossipMembership gm(gcfg, nullptr);
    EXPECT_EQ(gm.backend_name(), "gossip");
}

TEST(ServiceDiscoveryTest, HybridDiscoveryBackendName) {
    RegistrarConfig rcfg;
    GossipConfig gcfg;
    auto ep1 = ep(0);
    HybridDiscovery hd(rcfg, gcfg, ep1, nullptr);
    EXPECT_EQ(hd.backend_name(), "hybrid");
}
