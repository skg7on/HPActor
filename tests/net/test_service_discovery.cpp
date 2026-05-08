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

#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/gossip_membership.hpp>
#include <hpactor/net/hybrid_discovery.hpp>

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

// Minimal concrete class that does not override raw_members(), so the default
// implementation (returning nullptr) is exercised.
struct TestDiscovery : IServiceDiscovery {
    void start() override {}
    void stop() override {}
    std::vector<Member> discover_all() const override { return {}; }
    const Member* discover(EndPoint) const override { return nullptr; }
    void announce(Member) override {}
    void on_member_change(MemberChangeCallback) override {}
    std::string backend_name() const override { return "test"; }
};

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

int main() {
    // ---- Test 1: IServiceDiscovery default raw_members() returns nullptr ---
    {
        TestDiscovery td;
        assert(td.raw_members() == nullptr);
    }

    // ---- Test 2: StaticDiscovery discover() returns correct member --------
    {
        auto ep1 = ep(9000);
        auto ep2 = ep(9001);
        std::vector<Member> members;
        Member m1;
        m1.endpoint = ep1;
        m1.host = "host-a";
        m1.tcp_port = 9000;
        members.push_back(m1);
        Member m2;
        m2.endpoint = ep2;
        m2.host = "host-b";
        m2.tcp_port = 9001;
        members.push_back(m2);

        StaticDiscovery sd(std::move(members));
        const auto* found = sd.discover(ep1);
        assert(found != nullptr);
        assert(found->endpoint == ep1);
        assert(found->host == "host-a");
        assert(found->tcp_port == 9000);
    }

    // ---- Test 3: StaticDiscovery discover() returns nullptr for unknown ----
    {
        auto known = ep(9000);
        auto unknown = ep(9999);
        std::vector<Member> members;
        Member m;
        m.endpoint = known;
        members.push_back(m);

        StaticDiscovery sd(std::move(members));
        assert(sd.discover(unknown) == nullptr);
    }

    // ---- Test 4: StaticDiscovery discover_all() returns all members -------
    {
        auto ep1 = ep(9000);
        auto ep2 = ep(9001);
        std::vector<Member> members;
        Member m1;
        m1.endpoint = ep1;
        members.push_back(m1);
        Member m2;
        m2.endpoint = ep2;
        members.push_back(m2);

        StaticDiscovery sd(std::move(members));
        auto all = sd.discover_all();
        assert(all.size() == 2);
    }

    // ---- Test 5: StaticDiscovery backend_name() ---------------------------
    {
        StaticDiscovery sd({});
        const auto& ref = sd;
        assert(ref.backend_name() == "static");
    }

    // ---- Test 6: ActorLocationCache put / get roundtrip -------------------
    {
        ActorLocationCache cache;
        ActorId id(42);
        auto ep1 = ep(9000);

        cache.put(id, ep1);
        auto result = cache.get(id);
        assert(result.has_value());
        assert(*result == ep1);
    }

    // ---- Test 7: ActorLocationCache expired entry returns nullopt ---------
    {
        ActorLocationCache cache;
        ActorId id(42);
        auto ep1 = ep(9000);

        cache.put(id, ep1, std::chrono::seconds(-1));  // already expired
        auto result = cache.get(id);
        assert(!result.has_value());
    }

    // ---- Test 8: ActorLocationCache evict removes entry -------------------
    {
        ActorLocationCache cache;
        ActorId id(42);
        auto ep1 = ep(9000);

        cache.put(id, ep1);
        assert(cache.get(id).has_value());

        cache.evict(id);
        assert(!cache.get(id).has_value());
    }

    // ---- Test 9: ActorLocationCache evict_node removes all for endpoint ----
    {
        ActorLocationCache cache;
        auto ep1 = ep(9000);
        auto ep2 = ep(9001);

        cache.put(ActorId(1), ep1);
        cache.put(ActorId(2), ep1);
        cache.put(ActorId(3), ep2);   // different endpoint

        cache.evict_node(ep1);

        assert(!cache.get(ActorId(1)).has_value());
        assert(!cache.get(ActorId(2)).has_value());
        assert(cache.get(ActorId(3)).has_value());   // still present
    }

    // ---- Test 10: ActorLocationCache purge_expired cleans up --------------
    {
        ActorLocationCache cache;
        auto ep1 = ep(9000);

        cache.put(ActorId(1), ep1, std::chrono::seconds(-1));    // expired
        cache.put(ActorId(2), ep1, std::chrono::seconds(3600));  // not expired

        cache.purge_expired();

        assert(!cache.get(ActorId(1)).has_value());   // expired
        auto result = cache.get(ActorId(2));
        assert(result.has_value());                    // kept
        assert(*result == ep1);
    }

    // ---- Test 11: UdpRegistrar backend_name() -----------------------------
    {
        RegistrarConfig rcfg;
        auto ep1 = ep(0);
        UdpRegistrar reg(rcfg, ep1, nullptr);
        assert(reg.backend_name() == "udp-registrar");
    }

    // ---- Test 12: GossipMembership backend_name() -------------------------
    {
        GossipConfig gcfg;
        GossipMembership gm(gcfg, nullptr);
        assert(gm.backend_name() == "gossip");
    }

    // ---- Test 13: HybridDiscovery backend_name() --------------------------
    {
        RegistrarConfig rcfg;
        GossipConfig gcfg;
        auto ep1 = ep(0);
        HybridDiscovery hd(rcfg, gcfg, ep1, nullptr);
        assert(hd.backend_name() == "hybrid");
    }

    std::printf("All service discovery tests passed\n");
    return 0;
}
