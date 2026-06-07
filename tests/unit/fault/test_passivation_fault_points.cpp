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
#include <hpactor/fault/fault_point.hpp>

using namespace hpactor::fault;

TEST(PassivationFaultPointsTest, AllPointsRegistered) {
    auto& registry = FaultPointRegistry::instance();
    std::vector<const FaultPoint*> points;
    registry.collect_prefix("hpactor.passivation.*", points);

    EXPECT_EQ(points.size(), 12u) << "All 12 passivation fault points must be "
                                     "registered";
}

TEST(PassivationFaultPointsTest, DomainIsPassivation) {
    auto& registry = FaultPointRegistry::instance();
    std::vector<const FaultPoint*> points;
    registry.collect_prefix("hpactor.passivation.*", points);

    for (const auto* pt : points) {
        EXPECT_EQ(pt->domain, FaultDomain::kPassivation)
            << "Fault point " << pt->path << " must belong to kPassivation domain";
    }
}

TEST(PassivationFaultPointsTest, IdleTimerFireRegistered) {
    auto* pt = FaultPointRegistry::instance().lookup("hpactor.passivation.idle."
                                                     "timer_fire");
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt->domain, FaultDomain::kPassivation);
}

TEST(PassivationFaultPointsTest, SnapshotWriteFailRegistered) {
    auto* pt = FaultPointRegistry::instance().lookup("hpactor.passivation."
                                                     "snapshot.write_fail");
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt->domain, FaultDomain::kPassivation);
}

TEST(PassivationFaultPointsTest, ReactivationRestoreFailRegistered) {
    auto* pt = FaultPointRegistry::instance().lookup("hpactor.passivation."
                                                     "reactivation.restore_"
                                                     "fail");
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt->domain, FaultDomain::kPassivation);
}

TEST(PassivationFaultPointsTest, MemoryPressureTriggerRegistered) {
    auto* pt = FaultPointRegistry::instance().lookup("hpactor.passivation."
                                                     "memory_pressure.trigger");
    ASSERT_NE(pt, nullptr);
    EXPECT_EQ(pt->domain, FaultDomain::kPassivation);
}

TEST(PassivationFaultPointsTest, NoForeignPointsInPassivationPrefix) {
    auto& registry = FaultPointRegistry::instance();
    std::vector<const FaultPoint*> points;
    registry.collect_prefix("hpactor.passivation.*", points);

    for (const auto* pt : points) {
        EXPECT_NE(pt->domain, FaultDomain::kMailbox);
        EXPECT_NE(pt->domain, FaultDomain::kTransport);
        EXPECT_NE(pt->domain, FaultDomain::kScheduler);
    }
}
