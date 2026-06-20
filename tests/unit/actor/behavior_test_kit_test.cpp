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

#include <hpactor/actor/testing/behavior_test_kit.hpp>

#include <gtest/gtest.h>

#include "hpactor/common.pb.h"
#include "hpactor/messages.pb.h"

using namespace hpactor;
using namespace hpactor::testing;

// ── Direct dispatch (baseline) ─────────────────────────────────

TEST(BehaviorTestKitTest, DirectDispatchWorks) {
    bool called = false;
    Behavior bh = Behavior::make().on<SpawnRequestMessage>(
        [&called](const SpawnRequestMessage& /*req*/) { called = true; });

    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    bh(msg);

    EXPECT_TRUE(called);
}

// ── Kit dispatch ───────────────────────────────────────────────

TEST(BehaviorTestKitTest, KitDispatchWorks) {
    bool called = false;
    Behavior bh = Behavior::make().on<SpawnRequestMessage>(
        [&called](const SpawnRequestMessage& /*req*/) { called = true; });

    BehaviorTestKit kit(bh);
    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    auto effect = kit.run(msg);

    EXPECT_EQ(effect.kind, EffectKind::Handled);
    EXPECT_TRUE(called);
}

TEST(BehaviorTestKitTest, NonMatchingMessageGoesToOtherHandler) {
    bool req_called = false;
    bool rsp_called = false;

    Behavior bh = Behavior::make()
                      .on<SpawnRequestMessage>([&](const SpawnRequestMessage&) {
                          req_called = true;
                      })
                      .on<SpawnResponseMessage>([&](const SpawnResponseMessage&) {
                          rsp_called = true;
                      });

    BehaviorTestKit kit(bh);
    SpawnResponseMessage rsp;
    rsp.set_error_code(1);
    TypedMessage msg(TypeTag::SpawnResponseTag, rsp);
    auto effect = kit.run(msg);

    EXPECT_EQ(effect.kind, EffectKind::Handled);
    EXPECT_TRUE(rsp_called);
    EXPECT_FALSE(req_called);
}

TEST(BehaviorTestKitTest, BecomeReplacesCurrentBehavior) {
    bool first_called = false;
    bool second_called = false;
    Behavior first = Behavior::make().on<SpawnRequestMessage>(
        [&](const SpawnRequestMessage&) { first_called = true; });
    Behavior second = Behavior::make().on<SpawnResponseMessage>(
        [&](const SpawnResponseMessage&) { second_called = true; });

    BehaviorTestKit kit(first);
    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg1(TypeTag::SpawnRequestTag, req);
    kit.run(msg1);
    EXPECT_TRUE(first_called);

    kit.become(second);
    SpawnResponseMessage rsp;
    rsp.set_error_code(1);
    TypedMessage msg2(TypeTag::SpawnResponseTag, rsp);
    kit.run(msg2);
    EXPECT_TRUE(second_called);
}

TEST(BehaviorTestKitTest, MultipleRunsDispatchIndependently) {
    int call_count = 0;
    Behavior bh = Behavior::make().on<SpawnRequestMessage>(
        [&](const SpawnRequestMessage&) { call_count++; });

    BehaviorTestKit kit(bh);
    SpawnRequestMessage req;
    req.set_actor_type_name("a");

    TypedMessage msg1(TypeTag::SpawnRequestTag, req);
    TypedMessage msg2(TypeTag::SpawnRequestTag, req);
    kit.run(msg1);
    kit.run(msg2);
    EXPECT_EQ(call_count, 2);
}
