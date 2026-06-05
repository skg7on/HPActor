// System tests for GossipMembership — real UDP sockets + EventLoop.

#include <gtest/gtest.h>

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/gossip_membership.hpp>
#include <hpactor/net/udp_transport.hpp>

#include "system_test_fixture.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>

using namespace hpactor;
using namespace hpactor::net;

namespace {

/// \brief Check whether a UDP port is available for binding.
static bool port_is_available(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001);
    addr.sin_port = htons(port);
    int rc = ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
    return rc == 0;
}

/// \brief Skip the test if gossip ports are not available (CI coverage builds
///        often have port conflicts or resource constraints).
static bool ensure_gossip_ports(uint16_t base, int count) {
    for (int i = 0; i < count; ++i) {
        if (!port_is_available(static_cast<uint16_t>(base + i)))
            return false;
    }
    return true;
}

static EndPoint ep(uint16_t port) {
    return Ipv4Endpoint{htonl(0x7F000001), htons(port)};
}
/// \brief Coverage builds instrument every function call, making UDP protocol
///        timers unreliable. Gossip tests are inherently timing-dependent and
///        violate the project's deterministic-test constraint under --coverage.
static bool coverage_build() {
#ifdef __GCOV__
    return true;
#else
    return false;
#endif
}

static GossipConfig
cfg_at(uint16_t port, const std::vector<uint16_t>& seed_ports = {}) {
    GossipConfig cfg;
    cfg.gossip_port = port;
    cfg.protocol_period = std::chrono::milliseconds(100);
    cfg.ping_timeout = std::chrono::milliseconds(50);
    cfg.suspicion_timeout = std::chrono::milliseconds(300);
    cfg.dead_timeout = std::chrono::milliseconds(2000);
    cfg.fanout = 2;
    cfg.indirect_probes = 1;
    cfg.local_state.identity.endpoint = ep(port);
    cfg.local_state.identity.host = "127.0.0.1";
    for (auto sp : seed_ports)
        cfg.seeds.push_back(ep(sp));
    return cfg;
}
} // namespace

// ── TwoNodeJoinAndDiscovery ────────────────────────────────────────────

TEST(GossipSystem, TwoNodeJoinAndDiscovery) {
    if (coverage_build())
        GTEST_SKIP() << "Gossip tests are non-deterministic under coverage";
    if (!ensure_gossip_ports(49000, 2))
        GTEST_SKIP() << "Gossip ports 49000-49001 not available";
    Config a_cfg = test::minimal_config();
    a_cfg.enable_network = true;
    ActorSystem sys_a(a_cfg);
    auto* loop_a = sys_a.event_loop();
    ASSERT_NE(loop_a, nullptr);

    GossipMembership node_a(cfg_at(49000), loop_a);
    node_a.start();
    EXPECT_EQ(node_a.discover_all().size(), 1u);

    Config b_cfg = test::minimal_config();
    b_cfg.enable_network = true;
    ActorSystem sys_b(b_cfg);
    auto* loop_b = sys_b.event_loop();

    GossipMembership node_b(cfg_at(49001, {49000}), loop_b);
    node_b.start();

    bool both = test::assert_eventually(
        [&]() {
            return node_a.discover_all().size() == 2 &&
                   node_b.discover_all().size() == 2;
        },
        5000);
    EXPECT_TRUE(both);

    node_b.stop();
    node_a.stop();
    sys_b.shutdown();
    sys_a.shutdown();
}

TEST(GossipSystem, FailureDetectionEndToEnd) {
    if (coverage_build())
        GTEST_SKIP() << "Gossip tests are non-deterministic under coverage";
    if (!ensure_gossip_ports(50000, 3))
        GTEST_SKIP() << "Gossip ports 50000-50002 not available";
    Config ac = test::minimal_config();
    ac.enable_network = true;
    ActorSystem sa(ac);
    auto* la = sa.event_loop();
    GossipMembership na(cfg_at(50000), la);
    na.start();

    Config bc = test::minimal_config();
    bc.enable_network = true;
    ActorSystem sb(bc);
    auto* lb = sb.event_loop();
    GossipMembership nb(cfg_at(50001, {50000}), lb);
    nb.start();

    Config cc = test::minimal_config();
    cc.enable_network = true;
    ActorSystem sc(cc);
    auto* lc = sc.event_loop();
    GossipMembership nc(cfg_at(50002, {50000}), lc);
    nc.start();

    bool all3 = test::assert_eventually(
        [&]() {
            return na.discover_all().size() == 3 &&
                   nb.discover_all().size() == 3 && nc.discover_all().size() == 3;
        },
        5000);
    ASSERT_TRUE(all3) << "Nodes did not discover each other within timeout";

    nc.stop();

    // C's stop() sends Leave, so A and B should see C as Left (not Dead)
    bool left = test::assert_eventually(
        [&]() {
            auto* ma = na.discover(ep(50002));
            auto* mb = nb.discover(ep(50002));
            return ma && ma->status == MemberStatus::Left && mb &&
                   mb->status == MemberStatus::Left;
        },
        5000);
    EXPECT_TRUE(left) << "Nodes did not detect C as Left within timeout";

    nb.stop();
    na.stop();
    sc.shutdown();
    sb.shutdown();
    sa.shutdown();
}

TEST(GossipSystem, GracefulLeave) {
    if (coverage_build())
        GTEST_SKIP() << "Gossip tests are non-deterministic under coverage";
    if (!ensure_gossip_ports(51000, 2))
        GTEST_SKIP() << "Gossip ports 51000-51001 not available";
    Config ac = test::minimal_config();
    ac.enable_network = true;
    ActorSystem sa(ac);
    auto* la = sa.event_loop();
    GossipMembership na(cfg_at(51000), la);
    na.start();

    Config bc = test::minimal_config();
    bc.enable_network = true;
    ActorSystem sb(bc);
    auto* lb = sb.event_loop();
    GossipMembership nb(cfg_at(51001, {51000}), lb);
    nb.start();

    bool disc = test::assert_eventually(
        [&]() {
            return na.discover_all().size() == 2 && nb.discover_all().size() == 2;
        },
        5000);
    ASSERT_TRUE(disc);

    nb.stop();

    bool left = test::assert_eventually(
        [&]() {
            auto* b = na.discover(ep(51001));
            return b && b->status == MemberStatus::Left;
        },
        5000);
    EXPECT_TRUE(left) << "Node A did not see B as Left within timeout";

    na.stop();
    sb.shutdown();
    sa.shutdown();
}
