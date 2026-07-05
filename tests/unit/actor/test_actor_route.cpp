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
#include <hpactor/actor/lifecycle/passivation_config.hpp>
#include <hpactor/actor/system/actor_route.hpp>

using namespace hpactor;

TEST(LocalPassivatedRouteTest, ConstructWithDefaults) {
    ActorId aid{42};
    PassivationRecord rec;

    LocalPassivatedRoute route(aid, "test-actor", rec, 8);

    EXPECT_EQ(route.actor_id().value(), 42u);
    EXPECT_EQ(route.persistence_id(), "test-actor");
    EXPECT_FALSE(route.is_active());
    EXPECT_EQ(route.state(), LifecycleState::kPassivated);
    EXPECT_FALSE(route.reactivation_in_progress());
}

TEST(LocalPassivatedRouteTest, ClaimReactivationWinsOnce) {
    ActorId aid{1};
    PassivationRecord rec;
    LocalPassivatedRoute route(aid, "", rec, 8);

    EXPECT_FALSE(route.reactivation_in_progress());
    EXPECT_TRUE(route.claim_reactivation());
    EXPECT_TRUE(route.reactivation_in_progress());

    // Second claim fails — already in progress
    EXPECT_FALSE(route.claim_reactivation());
}

TEST(LocalPassivatedRouteTest, StateReflectsReactivationInProgress) {
    ActorId aid{1};
    PassivationRecord rec;
    LocalPassivatedRoute route(aid, "", rec, 8);

    EXPECT_EQ(route.state(), LifecycleState::kPassivated);

    route.claim_reactivation();
    EXPECT_EQ(route.state(), LifecycleState::kRecovering);
}

TEST(LocalPassivatedRouteTest, TransitionToRecovering) {
    ActorId aid{1};
    PassivationRecord rec;
    LocalPassivatedRoute route(aid, "", rec, 8);

    route.transition_to_recovering();
    EXPECT_EQ(route.state(), LifecycleState::kRecovering);
}

TEST(LocalPassivatedRouteTest, SetState) {
    ActorId aid{1};
    PassivationRecord rec;
    LocalPassivatedRoute route(aid, "", rec, 8);

    route.set_state(LifecycleState::kActive);
    EXPECT_EQ(route.state(), LifecycleState::kActive);

    route.set_state(LifecycleState::kFailed);
    EXPECT_EQ(route.state(), LifecycleState::kFailed);
}

TEST(LocalPassivatedRouteTest, BufferMessagesWithinCapacity) {
    ActorId aid{1};
    PassivationRecord rec;
    LocalPassivatedRoute route(aid, "", rec, 4);

    EXPECT_TRUE(route.try_buffer_message());
    EXPECT_TRUE(route.try_buffer_message());
    EXPECT_TRUE(route.try_buffer_message());
    EXPECT_TRUE(route.try_buffer_message());
    EXPECT_EQ(route.buffered_count(), 4u);

    // Buffer full
    EXPECT_FALSE(route.try_buffer_message());
    EXPECT_EQ(route.buffered_count(), 4u);
}

TEST(LocalPassivatedRouteTest, NonDurableHasEmptyPersistenceId) {
    ActorId aid{99};
    PassivationRecord rec;
    LocalPassivatedRoute route(aid, "", rec, 64);

    EXPECT_TRUE(route.persistence_id().empty());
}

TEST(LocalPassivatedRouteTest, DescribeContainsActorId) {
    ActorId aid{123};
    PassivationRecord rec;
    LocalPassivatedRoute route(aid, "persist-1", rec, 64);

    auto desc = route.describe();
    EXPECT_NE(desc.find("123"), std::string::npos);
    EXPECT_NE(desc.find("persist-1"), std::string::npos);
}
