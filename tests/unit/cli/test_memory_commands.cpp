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

#include <hpactor/mem/memory_region.hpp>

#include <gtest/gtest.h>
#include <string>

namespace {

TEST(SystemMemoryCommandUnitTest, SystemMemoryRegionAccessible) {
    auto& reg = hpactor::mem::MemoryRegionRegistry::instance();
    auto snap = reg.snapshot(hpactor::mem::RegionType::kActor);
    EXPECT_GE(snap.alloc_count, 0ULL);
    EXPECT_GE(snap.free_count, 0ULL);
    EXPECT_GE(snap.active_bytes, 0ULL);
}

TEST(SystemMemoryCommandUnitTest, AllSixRegionsAreQueryable) {
    auto& reg = hpactor::mem::MemoryRegionRegistry::instance();
    static constexpr hpactor::mem::RegionType kRegions[] = {
        hpactor::mem::RegionType::kActor,
        hpactor::mem::RegionType::kMessage,
        hpactor::mem::RegionType::kCoroutine,
        hpactor::mem::RegionType::kNetwork,
        hpactor::mem::RegionType::kInternal,
        hpactor::mem::RegionType::kHibernate,
    };
    for (auto region : kRegions) {
        auto snap = reg.snapshot(region);
        EXPECT_GE(snap.active_bytes, 0ULL);
        EXPECT_GE(snap.alloc_count, 0ULL);
        EXPECT_GE(snap.free_count, 0ULL);
        EXPECT_GE(snap.corruption_events, 0ULL);
        EXPECT_GE(snap.high_water_mark, 0ULL);
    }
}

TEST(SystemMemoryCommandUnitTest, RegionToStringReturnsNonNull) {
    static constexpr hpactor::mem::RegionType kRegions[] = {
        hpactor::mem::RegionType::kActor,
        hpactor::mem::RegionType::kMessage,
        hpactor::mem::RegionType::kCoroutine,
        hpactor::mem::RegionType::kNetwork,
        hpactor::mem::RegionType::kInternal,
        hpactor::mem::RegionType::kHibernate,
    };
    for (auto region : kRegions) {
        const char* name = hpactor::mem::to_string(region);
        ASSERT_NE(name, nullptr);
        EXPECT_STRNE(name, "Unknown");
    }
}

TEST(SystemMemoryCommandUnitTest, PressureStateToStringReturnsNonNull) {
    EXPECT_STREQ(hpactor::mem::to_string(hpactor::mem::MemoryPressureState::kNormal),
                 "normal");
    EXPECT_STREQ(hpactor::mem::to_string(hpactor::mem::MemoryPressureState::kHigh),
                 "high");
    EXPECT_STREQ(
        hpactor::mem::to_string(hpactor::mem::MemoryPressureState::kHardLimit),
        "hard-limit");
}

TEST(SystemMemoryCommandUnitTest, SnapshotPressureIsNormalByDefault) {
    auto& reg = hpactor::mem::MemoryRegionRegistry::instance();
    auto snap = reg.snapshot(hpactor::mem::RegionType::kActor);
    EXPECT_EQ(snap.pressure, hpactor::mem::MemoryPressureState::kNormal);
}

TEST(SystemMemoryCommandUnitTest, LimitDefaultsToUnlimited) {
    auto& reg = hpactor::mem::MemoryRegionRegistry::instance();
    auto limit = reg.limit(hpactor::mem::RegionType::kMessage);
    EXPECT_EQ(limit.hard_limit_bytes, 0ULL);
}

TEST(SystemMemoryCommandUnitTest, ConfigureRegionSetsLimit) {
    auto& reg = hpactor::mem::MemoryRegionRegistry::instance();
    hpactor::mem::RegionLimit lim;
    lim.hard_limit_bytes = 1024 * 1024;
    lim.high_watermark_ratio = 0.8f;
    reg.configure_region(hpactor::mem::RegionType::kNetwork, lim);

    auto fetched = reg.limit(hpactor::mem::RegionType::kNetwork);
    EXPECT_EQ(fetched.hard_limit_bytes, 1024ULL * 1024);
    EXPECT_FLOAT_EQ(fetched.high_watermark_ratio, 0.8f);

    // Reset to unlimited so other tests are not affected.
    hpactor::mem::RegionLimit reset;
    reg.configure_region(hpactor::mem::RegionType::kNetwork, reset);
}

TEST(SystemMemoryCommandUnitTest, RecordCorruptionIncrementsCounter) {
    auto& reg = hpactor::mem::MemoryRegionRegistry::instance();
    auto before = reg.snapshot(hpactor::mem::RegionType::kInternal);
    reg.record_corruption(hpactor::mem::RegionType::kInternal);
    auto after = reg.snapshot(hpactor::mem::RegionType::kInternal);
    EXPECT_GT(after.corruption_events, before.corruption_events);
}

} // namespace
