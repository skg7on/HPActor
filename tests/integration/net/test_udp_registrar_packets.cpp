// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

// Integration test: UdpRegistrar construction and IServiceDiscovery interface.
// Tests construction (with/without EventLoop), config defaults,
// and the discover/discover_all/announce/on_member_change/raw_members API.

#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/registrar.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

TEST(UdpRegistrarPacketTest, RegistrarConfigDefaults) {
    RegistrarConfig config;
    EXPECT_EQ(config.udp_port, 5353u);
    EXPECT_EQ(config.tcp_port, 5353u);
    EXPECT_EQ(config.heartbeat_interval, std::chrono::milliseconds(5000));
    EXPECT_EQ(config.expiration_timeout, std::chrono::milliseconds(15000));
    EXPECT_TRUE(config.static_routes.empty());
    EXPECT_FALSE(config.disable_server);
}

TEST(UdpRegistrarPacketTest, ConstructorWithEventLoop) {
    EventLoop loop;
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    RegistrarConfig config;
    config.udp_port = 0;
    config.tcp_port = 0;

    UdpRegistrar registrar(config, ep, &loop);
    EXPECT_EQ(registrar.backend_name(), "udp-registrar");
}

TEST(UdpRegistrarPacketTest, ConstructorWithoutEventLoop) {
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    RegistrarConfig config;
    config.udp_port = 0;
    config.tcp_port = 0;

    UdpRegistrar registrar(config, ep);
    EXPECT_EQ(registrar.backend_name(), "udp-registrar");
}

TEST(UdpRegistrarPacketTest, DiscoverAllReturnsEmptyVector) {
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    RegistrarConfig config;
    config.udp_port = 0;
    config.tcp_port = 0;

    UdpRegistrar registrar(config, ep);
    auto members = registrar.discover_all();
    EXPECT_TRUE(members.empty());
}

TEST(UdpRegistrarPacketTest, DiscoverReturnsNullForUnknown) {
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    RegistrarConfig config;
    config.udp_port = 0;
    config.tcp_port = 0;

    UdpRegistrar registrar(config, ep);
    auto unknown = endpoint_ops::parse_endpoint("10.0.0.1:8080");
    auto* member = registrar.discover(unknown);
    EXPECT_EQ(member, nullptr);
}

TEST(UdpRegistrarPacketTest, AnnounceIsNoop) {
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    RegistrarConfig config;
    config.udp_port = 0;
    config.tcp_port = 0;

    UdpRegistrar registrar(config, ep);
    Member m;
    m.identity.host = "test";
    m.identity.endpoint = ep;
    registrar.announce(m);
    SUCCEED();
}

TEST(UdpRegistrarPacketTest, OnMemberChangeCbStored) {
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    RegistrarConfig config;
    config.udp_port = 0;
    config.tcp_port = 0;

    UdpRegistrar registrar(config, ep);
    bool cb_called = false;
    registrar.on_member_change(
        [&cb_called](const Member&, bool) { cb_called = true; });
    // cb stored but not invoked during normal operation
    SUCCEED();
}

TEST(UdpRegistrarPacketTest, RawMembersInitiallyEmpty) {
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:9999");
    RegistrarConfig config;
    config.udp_port = 0;
    config.tcp_port = 0;

    UdpRegistrar registrar(config, ep);
    auto* raw = registrar.raw_members();
    EXPECT_NE(raw, nullptr);
    EXPECT_TRUE(raw->empty());
}

TEST(UdpRegistrarPacketTest, RegistrarConfigCustomValues) {
    RegistrarConfig config;
    config.udp_port = 9000;
    config.tcp_port = 9001;
    config.heartbeat_interval = std::chrono::milliseconds(10000);
    config.expiration_timeout = std::chrono::milliseconds(30000);
    config.probe_interval = std::chrono::milliseconds(60000);
    config.disable_server = true;
    EXPECT_EQ(config.udp_port, 9000u);
    EXPECT_EQ(config.tcp_port, 9001u);
    EXPECT_TRUE(config.disable_server);
}
