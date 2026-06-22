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

#include <hpactor/actor/durable/passivation_manager.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::actor::durable {

TEST(PassivationManagerTest, ConstructionHasNoActors) {
    PassivationManager mgr;
    EXPECT_EQ(mgr.tracked_count(), 0u);
}

TEST(PassivationManagerTest, RegisterAndUnregisterActor) {
    PassivationManager mgr;
    mgr.register_actor(ActorId{100}, "order-1", PassivationConfig{});
    EXPECT_EQ(mgr.tracked_count(), 1u);
    EXPECT_TRUE(mgr.is_tracked(ActorId{100}));
    mgr.unregister_actor(ActorId{100});
    EXPECT_EQ(mgr.tracked_count(), 0u);
    EXPECT_FALSE(mgr.is_tracked(ActorId{100}));
}

TEST(PassivationManagerTest, BeginPassivateSetsState) {
    PassivationManager mgr;
    mgr.register_actor(ActorId{100}, "order-1", PassivationConfig{});
    EXPECT_TRUE(mgr.begin_passivate(ActorId{100}));
    EXPECT_EQ(mgr.get_state(ActorId{100}), PassivationState::Passivating);
}

TEST(PassivationManagerTest, CompletePassivationRemovesActor) {
    PassivationManager mgr;
    mgr.register_actor(ActorId{100}, "order-1", PassivationConfig{});
    mgr.begin_passivate(ActorId{100});
    mgr.complete_passivation(ActorId{100});
    EXPECT_EQ(mgr.tracked_count(), 0u);
}

TEST(PassivationManagerTest, BeginPassivateUnknownActorReturnsFalse) {
    PassivationManager mgr;
    EXPECT_FALSE(mgr.begin_passivate(ActorId{999}));
}

TEST(PassivationManagerTest, GetStateForUnknownActorReturnsPassivated) {
    PassivationManager mgr;
    EXPECT_EQ(mgr.get_state(ActorId{999}), PassivationState::Passivated);
}

} // namespace hpactor::actor::durable
