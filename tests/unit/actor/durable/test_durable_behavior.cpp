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

#include <hpactor/actor/durable/durable_behavior.hpp>
#include <hpactor/actor/durable_state_store.hpp>
#include <hpactor/types/types.hpp>

#include "durable_test_helpers.hpp"

#include <memory>
#include <string>

namespace hpactor::actor::durable {

// Serialization specializations are shared in durable_test_helpers.hpp

class DurableBehaviorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        store_ = std::make_unique<TestInMemoryStore>();
    }
    std::unique_ptr<DurableStateStore> store_;
};

TEST_F(DurableBehaviorTest, ConstructionIsNotRecovered) {
    DurableBehavior<TestState> behavior("actor-1", *store_, TestState{0, "initial"});
    EXPECT_FALSE(behavior.is_recovered());
    EXPECT_EQ(behavior.persistence_id(), "actor-1");
}

TEST_F(DurableBehaviorTest, RecoverWithNoPriorSnapshotReturnsSuccess) {
    DurableBehavior<TestState> behavior("actor-1", *store_, TestState{0, "initial"});
    auto result = behavior.recover();
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(behavior.is_recovered());
}

TEST_F(DurableBehaviorTest, SnapshotThenRecover) {
    {
        DurableBehavior<TestState> b1("actor-1", *store_, TestState{0, "initial"});
        b1.recover();
        b1.state().counter = 42;
        b1.state().name = "modified";
        auto snap = b1.snapshot();
        ASSERT_TRUE(snap.ok());
    }
    {
        DurableBehavior<TestState> b2("actor-1", *store_, TestState{0, "default"});
        auto result = b2.recover();
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(b2.is_recovered());
        EXPECT_EQ(b2.state().counter, 42);
        EXPECT_EQ(b2.state().name, "modified");
    }
}

} // namespace hpactor::actor::durable
