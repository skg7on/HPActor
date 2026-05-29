// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#include <hpactor/fault/fault_point.hpp>

#include <gtest/gtest.h>

namespace hpactor::fault {
namespace {

TEST(FaultPointRegistry, LookupExactMatch) {
    auto& reg = FaultPointRegistry::instance();
    const auto* pt = reg.lookup("hpactor.mailbox.enqueue.fail");
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt->path, "hpactor.mailbox.enqueue.fail");
    EXPECT_EQ(pt->domain, FaultDomain::kMailbox);
}

TEST(FaultPointRegistry, LookupMissingReturnsNull) {
    auto& reg = FaultPointRegistry::instance();
    EXPECT_EQ(reg.lookup("nonexistent.point"), nullptr);
}

TEST(FaultPointRegistry, PrefixMatchExact) {
    auto& reg = FaultPointRegistry::instance();
    EXPECT_TRUE(reg.matches_prefix("hpactor.mailbox.enqueue.fail",
                                   "hpactor.mailbox.enqueue.fail"));
}

TEST(FaultPointRegistry, PrefixMatchWildcard) {
    auto& reg = FaultPointRegistry::instance();
    EXPECT_TRUE(reg.matches_prefix("hpactor.mailbox.enqueue.fail",
                                   "hpactor.mailbox.*"));
}

TEST(FaultPointRegistry, PrefixMatchStar) {
    auto& reg = FaultPointRegistry::instance();
    EXPECT_TRUE(reg.matches_prefix("anything.here", "*"));
}

TEST(FaultPointRegistry, PrefixMatchNoMatch) {
    auto& reg = FaultPointRegistry::instance();
    EXPECT_FALSE(reg.matches_prefix("hpactor.mailbox.enqueue.fail",
                                    "hpactor.transport.*"));
}

TEST(FaultPointRegistry, CollectByPrefix) {
    auto& reg = FaultPointRegistry::instance();
    std::vector<const FaultPoint*> out;
    reg.collect_prefix("hpactor.transport.*", out);
    EXPECT_GE(out.size(), 5u);
    for (const auto* pt : out) {
        EXPECT_EQ(pt->domain, FaultDomain::kTransport);
    }
}

TEST(FaultPointRegistry, NonEmptyCatalog) {
    auto& reg = FaultPointRegistry::instance();
    EXPECT_GE(reg.points().size(), 12u);
}

} // anonymous namespace
} // namespace hpactor::fault
