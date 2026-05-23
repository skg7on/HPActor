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

// tests/unit/sched/test_actor_state.cpp
#include <gtest/gtest.h>
#include <hpactor/actor/actor_state.hpp>

TEST(ActorStateTest, InitialStateIsIdle) {
    hpactor::ActorState state;
    EXPECT_EQ(state.get(), hpactor::ActorState::kIdle);
    EXPECT_TRUE(state.is_idle());
}

TEST(ActorStateTest, ValidTransitions) {
    hpactor::ActorState state;

    EXPECT_TRUE(state.cas(hpactor::ActorState::kIdle, hpactor::ActorState::kReady));
    EXPECT_TRUE(state.is_ready());

    EXPECT_TRUE(
        state.cas(hpactor::ActorState::kReady, hpactor::ActorState::kRunning));
    EXPECT_TRUE(state.is_running());

    EXPECT_TRUE(state.cas(hpactor::ActorState::kRunning, hpactor::ActorState::kIdle));
    EXPECT_TRUE(state.is_idle());
}

TEST(ActorStateTest, InvalidTransitionNoChange) {
    hpactor::ActorState state;
    // state is Idle, not Running → CAS should fail
    bool ok = state.cas(hpactor::ActorState::kRunning, hpactor::ActorState::kIdle);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(state.is_idle());
}

TEST(ActorStateTest, SetOverridesRegardlessOfCurrentState) {
    hpactor::ActorState state;
    state.set(hpactor::ActorState::kTerminated);
    EXPECT_TRUE(state.is_terminated());
}

TEST(ActorStateTest, IOWaiting) {
    hpactor::ActorState state;
    state.set(hpactor::ActorState::kIOWaiting);
    EXPECT_TRUE(state.is_io_waiting());
    EXPECT_FALSE(state.is_running());
    EXPECT_FALSE(state.is_idle());
}
