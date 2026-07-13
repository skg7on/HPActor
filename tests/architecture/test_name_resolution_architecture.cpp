// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

// Architecture enforcement: no RTTI, no exceptions, no std::function
// in cluster/name production code.

#include <gtest/gtest.h>

// Verify name resolution headers compile without RTTI/exceptions.
// These tests are compiled with -fno-rtti -fno-exceptions.

#include <hpactor/cluster/name/name_directory.hpp>
#include <hpactor/cluster/name/name_resolve_cache.hpp>
#include <hpactor/cluster/name/consistent_hash_ring.hpp>
#include <hpactor/cluster/name/name_resolver.hpp>
#include <hpactor/cluster/name/name_registration_port.hpp>
#include <hpactor/cluster/name/inbound_name_port.hpp>
#include <hpactor/cluster/name/outbound_name_query_port.hpp>
#include <hpactor/config/name_resolution_config.hpp>

namespace {

TEST(NameResolutionArchitecture, PortTypesAreFixedSizes) {
    // NameRegistrationPort: two fn ptrs + 1 void* = 3 pointers max.
    EXPECT_LE(sizeof(hpactor::cluster::name::NameRegistrationPort),
              3 * sizeof(void*));
    // OutboundNameQueryPort: 1 fn ptr + 1 void*.
    EXPECT_LE(sizeof(hpactor::cluster::name::OutboundNameQueryPort),
              2 * sizeof(void*));
    // InboundNamePort: 3 fn ptrs + 1 void*.
    EXPECT_LE(sizeof(hpactor::cluster::name::InboundNamePort),
              4 * sizeof(void*));
}

TEST(NameResolutionArchitecture, ConfigFieldsAreDefaulted) {
    hpactor::config::NameResolutionConfig cfg;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_EQ(cfg.resolve_timeout_ms, 2000u);
    EXPECT_GE(cfg.register_timeout_ms, 100u);
    EXPECT_GE(cfg.cache_ttl_seconds, 1u);
    EXPECT_TRUE(cfg.valid());
}

TEST(NameResolutionArchitecture, ConsistentHashRingIsDefaultConstructible) {
    hpactor::cluster::name::ConsistentHashRing ring;
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.size(), 0u);
}

TEST(NameResolutionArchitecture, NameDirectoryIsDefaultConstructible) {
    hpactor::cluster::name::NameDirectory dir;
    EXPECT_EQ(dir.size(), 0u);
}

TEST(NameResolutionArchitecture, NameResolveCacheIsDefaultConstructible) {
    hpactor::cluster::name::NameResolveCache cache;
    EXPECT_FALSE(cache.get("any").has_value());
}

} // namespace
