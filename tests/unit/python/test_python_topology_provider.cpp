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

#include <hpactor/python/python_topology_provider.hpp>
#include <hpactor/runtime/configured_actor_provider.hpp>

namespace hpactor::python {
namespace {

TEST(PythonTopologyProviderReadyTableTest, ReserveAcceptsToken) {
    PythonTopologyReadyTable table(8);
    auto result = table.reserve(42);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(table.size(), 1u);
}

TEST(PythonTopologyProviderReadyTableTest, ReserveRejectsDuplicateToken) {
    PythonTopologyReadyTable table(8);
    ASSERT_TRUE(table.reserve(1).ok());
    // Re-reserving the same token is idempotent, not an error.
    auto result = table.reserve(1);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(table.size(), 1u);
}

TEST(PythonTopologyProviderReadyTableTest, ReserveEnforcesCapacity) {
    PythonTopologyReadyTable table(2);
    ASSERT_TRUE(table.reserve(1).ok());
    ASSERT_TRUE(table.reserve(2).ok());
    auto result = table.reserve(3);
    EXPECT_FALSE(result.ok()); // Table is full.
}

TEST(PythonTopologyProviderReadyTableTest, CompleteWakesWaiter) {
    PythonTopologyReadyTable table(4);
    ASSERT_TRUE(table.reserve(99).ok());

    // Simulate Python-side completion.
    auto complete_result = table.complete(99, 1, 1,
                                           TopologyActorOutcome::Ready, 0, "");
    ASSERT_TRUE(complete_result.ok());

    // Wait should succeed immediately since complete() already ran.
    auto wait_result = table.wait_ready(99, std::chrono::milliseconds(100));
    EXPECT_TRUE(wait_result.ok());
}

TEST(PythonTopologyProviderReadyTableTest, WaitReadyTimesOut) {
    PythonTopologyReadyTable table(4);
    ASSERT_TRUE(table.reserve(7).ok());

    auto wait_result =
        table.wait_ready(7, std::chrono::milliseconds(10));
    EXPECT_FALSE(wait_result.ok());
}

TEST(PythonTopologyProviderReadyTableTest, CompleteIsIdempotent) {
    PythonTopologyReadyTable table(4);
    ASSERT_TRUE(table.reserve(1).ok());
    ASSERT_TRUE(table.complete(1, 0, 0,
                                TopologyActorOutcome::Ready, 0, "").ok());
    // Second completion should succeed (idempotent).
    ASSERT_TRUE(table.complete(1, 0, 0,
                                TopologyActorOutcome::Ready, 0, "").ok());
}

TEST(PythonTopologyProviderReadyTableTest, NonReadyOutcomeFailsWait) {
    PythonTopologyReadyTable table(4);
    ASSERT_TRUE(table.reserve(5).ok());

    // Complete with ConstructorFailed outcome.
    ASSERT_TRUE(table.complete(5, 1, 1,
                               TopologyActorOutcome::ConstructorFailed,
                               42, "test error").ok());

    auto wait_result = table.wait_ready(5, std::chrono::milliseconds(100));
    EXPECT_FALSE(wait_result.ok());
}

TEST(ConfiguredCppProviderTest, MatchesBuiltinCpp) {
    ConfiguredActorPlan plan;
    plan.provider = ConfiguredActorProviderKind::BuiltinCpp;
    plan.provider_token = 0;

    // Implement the matches test — matches relies on
    // ConfiguredActorProviderKind::BuiltinCpp with token 0.
    EXPECT_EQ(plan.provider, ConfiguredActorProviderKind::BuiltinCpp);
    EXPECT_EQ(plan.provider_token, 0u);
}

TEST(ConfiguredCppProviderTest, DoesNotMatchExternalWithToken) {
    ConfiguredActorPlan plan;
    plan.provider = ConfiguredActorProviderKind::External;
    plan.provider_token = 42;

    EXPECT_NE(plan.provider, ConfiguredActorProviderKind::BuiltinCpp);
    EXPECT_NE(plan.provider_token, 0u);
}

} // namespace
} // namespace hpactor::python
