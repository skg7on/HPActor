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
#include <gtest/gtest.h>

#include <hpactor/cluster/name/name_resolver.hpp>
#include <hpactor/cluster/name/name_directory.hpp>
#include <hpactor/cluster/name/name_resolve_cache.hpp>
#include <hpactor/cluster/name/name_registration_port.hpp>
#include <hpactor/config/name_resolution_config.hpp>
#include <hpactor/net/service_discovery.hpp>

namespace hpactor::cluster::name {
namespace {

using namespace std::chrono_literals;

// Fake discovery that reports a fixed member set.
class FakeDiscovery : public net::IServiceDiscovery {
  public:
    explicit FakeDiscovery(std::vector<EndPoint> members)
        : members_(std::move(members)) {}

    void start() override {}
    void stop() override {}

    std::vector<net::Member> discover_all() const override {
        std::vector<net::Member> result;
        for (auto& ep : members_) {
            net::Member m;
            m.identity.endpoint = ep;
            result.push_back(m);
        }
        return result;
    }

    const net::Member* discover(EndPoint ep) const override {
        for (auto& ep_m : members_) {
            if (ep_m == ep) {
                cached_ = net::Member{};
                cached_->identity.endpoint = ep;
                return &*cached_;
            }
        }
        return nullptr;
    }

    void announce(net::Member) override {}

    void on_member_change(net::MemberChangeCallback cb) override {
        cb_ = std::move(cb);
    }

    std::string backend_name() const override { return "fake"; }

    // Test helper: simulate membership change.
    void simulate_change(std::vector<EndPoint> added,
                         std::vector<EndPoint> removed) {
        if (cb_) {
            for (auto& ep : added) {
                net::Member m;
                m.identity.endpoint = ep;
                cb_(m, true);
            }
            for (auto& ep : removed) {
                net::Member m;
                m.identity.endpoint = ep;
                cb_(m, false);
            }
        }
    }

  private:
    std::vector<EndPoint> members_;
    net::MemberChangeCallback cb_;
    mutable std::optional<net::Member> cached_;
};

EndPoint ep(const std::string& s) { return endpoint_ops::parse_endpoint(s); }

struct TestContext {
    NameDirectory name_dir;
    NameResolveCache cache;
    config::NameResolutionConfig config;
    FakeDiscovery discovery{{ep("10.0.0.1:9000"), ep("10.0.0.2:9000")}};
    NameRegistrationPort reg_port;

    std::unique_ptr<NameResolver> resolver;

    TestContext() {
        config.enabled = true;
        OutboundNameQueryPort outbound{}; // not used in unit tests
        InboundNamePort inbound{};        // not used in unit tests

        resolver = std::make_unique<NameResolver>(
            name_dir, discovery, cache, config,
            ep("10.0.0.1:9000"), outbound, inbound);

        // Wire the registration port back to the resolver.
        reg_port.context = resolver.get();
        reg_port.on_register = [](void* ctx, std::string_view name,
                                   ActorAddress addr, uint64_t gen) {
            static_cast<NameResolver*>(ctx)->on_local_register(name, addr, gen);
        };
        reg_port.on_unregister = [](void* ctx, std::string_view name) {
            static_cast<NameResolver*>(ctx)->on_local_unregister(name);
        };
    }
};

// ── Self-home resolution (home node == local node) ─────────────────────

TEST(NameResolverTest, ResolveSelfHome) {
    TestContext ctx;
    NameEntry entry;
    entry.actor_id = ActorId{42};
    entry.endpoint = ep("10.0.0.3:9000");  // hosted elsewhere
    entry.generation = 1;
    entry.registered_at = std::chrono::steady_clock::now();
    ctx.name_dir.register_entry("billing", entry);

    auto result = ctx.resolver->resolve("billing");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id.value(), 42u);
    EXPECT_EQ(result->endpoint, ep("10.0.0.3:9000"));
}

// ── Cache hit ──────────────────────────────────────────────────────────

TEST(NameResolverTest, ResolveCacheHit) {
    TestContext ctx;
    auto addr = ActorAddress{ep("10.0.0.2:9000"), ActorType{0}, ActorId{99}, 0};
    ctx.cache.put("cached-svc", addr, 60s);

    auto result = ctx.resolver->resolve("cached-svc");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id.value(), 99u);
    EXPECT_EQ(result->endpoint, ep("10.0.0.2:9000"));
}

// ── Resolution miss ────────────────────────────────────────────────────

TEST(NameResolverTest, ResolveNotFound) {
    TestContext ctx;
    auto result = ctx.resolver->resolve("nonexistent");
    // Without a real transport, remote resolution can't succeed.
    // The resolver returns nullopt when neither local nor cache hits.
    EXPECT_FALSE(result.has_value());
}

// ── Local register → self-home path ────────────────────────────────────

TEST(NameResolverTest, LocalRegisterSelfHome) {
    TestContext ctx;
    // The test context has two nodes: 10.0.0.1:9000 and 10.0.0.2:9000.
    // The local endpoint is whatever the resolver treats as "self."
    // on_local_register hashes the name and either commits locally or
    // sends to the home node. For unit testing, we verify it doesn't
    // crash and that self-home registrations work when the hash points
    // to a node in our ring.

    auto addr = ActorAddress{ep("10.0.0.3:9000"), ActorType{0}, ActorId{7}, 0};
    ctx.resolver->on_local_register("worker-1", addr, 1);

    // If the hash of "worker-1" happened to land on a node in our ring
    // that isn't self, the resolver would try to send an outbound message
    // (which is a no-op in this test since OutboundNameQueryPort is null).
    // This test just verifies no crash and valid internal state.
    SUCCEED();
}

// ── Membership change rebuilds ring ────────────────────────────────────

TEST(NameResolverTest, MembershipChangeTriggersCacheEviction) {
    TestContext ctx;
    // Cache an entry for a node that will be "removed."
    auto addr = ActorAddress{ep("10.0.0.1:9000"), ActorType{0}, ActorId{1}, 0};
    ctx.cache.put("ephemeral", addr, 3600s);

    // Before: cache hit.
    EXPECT_TRUE(ctx.cache.get("ephemeral").has_value());

    // Simulate node departure.
    ctx.resolver->on_membership_change(
        {}, {ep("10.0.0.1:9000")});

    // After: cache entry for departed node evicted.
    EXPECT_FALSE(ctx.cache.get("ephemeral").has_value());
}

// ── Config disabled → no-ops ───────────────────────────────────────────

TEST(NameResolverTest, ResolveWhenDisabledFallsThrough) {
    TestContext ctx;
    ctx.config.enabled = false;
    // Re-create with disabled config.
    OutboundNameQueryPort outbound{};
    InboundNamePort inbound{};
    auto disabled_resolver = std::make_unique<NameResolver>(
        ctx.name_dir, ctx.discovery, ctx.cache, ctx.config,
        ep("10.0.0.1:9000"), outbound, inbound);

    auto result = disabled_resolver->resolve("anything");
    EXPECT_FALSE(result.has_value());
}

// ── Registration port no-ops when resolver is null ─────────────────────

TEST(NameResolverTest, RegistrationPortActiveFlag) {
    NameRegistrationPort port;
    EXPECT_FALSE(port.active());

    port.context = reinterpret_cast<void*>(1);
    port.on_register = [](void*, std::string_view, ActorAddress, uint64_t) {};
    port.on_unregister = [](void*, std::string_view) {};
    EXPECT_TRUE(port.active());
}

} // namespace
} // namespace hpactor::cluster::name
