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

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/testing/behavior_test_kit.hpp>

#include <gtest/gtest.h>

#include "hpactor/common.pb.h"
#include "hpactor/messages.pb.h"

using namespace hpactor;
using namespace hpactor::testing;

// ── Basic translation ──────────────────────────────────────────

TEST(MessageAdapterCombinatorTest, TranslatesFromToTo) {
    bool inner_called = false;
    uint32_t received_error_code = 0;

    auto inner = Behavior::make().on<SpawnResponseMessage>(
        [&](const SpawnResponseMessage& rsp) {
            inner_called = true;
            received_error_code = rsp.error_code();
        });

    // Adapter: SpawnRequest → SpawnResponse.
    // error_code = length of actor_type_name.
    auto adapted =
        Behavior::message_adapter<SpawnRequestMessage, SpawnResponseMessage>(
            [](const SpawnRequestMessage& req) {
                SpawnResponseMessage rsp;
                rsp.set_error_code(
                    static_cast<uint32_t>(req.actor_type_name().size()));
                return rsp;
            },
            inner);

    BehaviorTestKit kit(adapted);
    SpawnRequestMessage req;
    req.set_actor_type_name("abcd"); // length 4
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    auto effect = kit.run(msg);

    EXPECT_EQ(effect.kind, EffectKind::Handled);
    EXPECT_TRUE(inner_called);
    EXPECT_EQ(received_error_code, 4u);
}

// ── Pass-through for non-matching types ────────────────────────

TEST(MessageAdapterCombinatorTest, PassesThroughNonMatchingType) {
    bool inner_called = false;

    auto inner = Behavior::make().on<SpawnResponseMessage>(
        [&](const SpawnResponseMessage&) { inner_called = true; });

    // Adapter only translates SpawnRequest → SpawnResponse.
    auto adapted = Behavior::message_adapter<SpawnRequestMessage, SpawnResponseMessage>(
        [](const SpawnRequestMessage& /*req*/) { return SpawnResponseMessage{}; },
        inner);

    BehaviorTestKit kit(adapted);
    // Send a SpawnResponse — not SpawnRequest, so adapter passes through.
    SpawnResponseMessage rsp;
    rsp.set_error_code(7);
    TypedMessage msg(TypeTag::SpawnResponseTag, rsp);
    auto effect = kit.run(msg);

    EXPECT_EQ(effect.kind, EffectKind::Handled);
    EXPECT_TRUE(inner_called);
}

// ── Adapter with empty inner ───────────────────────────────────

TEST(MessageAdapterCombinatorTest, AdapterWithEmptyInnerDoesNotCrash) {
    auto adapted = Behavior::message_adapter<SpawnRequestMessage, SpawnResponseMessage>(
        [](const SpawnRequestMessage& /*req*/) { return SpawnResponseMessage{}; },
        Behavior::empty());

    BehaviorTestKit kit(adapted);
    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    auto effect = kit.run(msg);
    EXPECT_EQ(effect.kind, EffectKind::Handled);
}

// ── Chained with interceptor ───────────────────────────────────

TEST(MessageAdapterCombinatorTest, AdapterBeforeInterceptor) {
    int interceptor_call_count = 0;

    auto inner = Behavior::make().on<SpawnResponseMessage>(
        [](const SpawnResponseMessage&) {});

    auto adapted = Behavior::message_adapter<SpawnRequestMessage, SpawnResponseMessage>(
        [](const SpawnRequestMessage& /*req*/) { return SpawnResponseMessage{}; },
        inner);

    auto intercepted = Behavior::intercept(
        adapted, [&](TypedMessage& msg, Behavior::next_fn next) {
            interceptor_call_count++;
            next(msg);
        });

    BehaviorTestKit kit(intercepted);
    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    auto effect = kit.run(msg);

    EXPECT_EQ(effect.kind, EffectKind::Handled);
    EXPECT_EQ(interceptor_call_count, 1);
}

// ── Non-matching passes through adapter → interceptor ──────────

TEST(MessageAdapterCombinatorTest, NonMatchingPassesThroughToInterceptor) {
    bool interceptor_called = false;
    bool inner_called = false;

    auto inner = Behavior::make().on<SpawnResponseMessage>(
        [&](const SpawnResponseMessage&) { inner_called = true; });

    auto adapted = Behavior::message_adapter<SpawnRequestMessage, SpawnResponseMessage>(
        [](const SpawnRequestMessage& /*req*/) { return SpawnResponseMessage{}; },
        inner);

    auto intercepted = Behavior::intercept(
        adapted, [&](TypedMessage& msg, Behavior::next_fn next) {
            interceptor_called = true;
            next(msg);
        });

    BehaviorTestKit kit(intercepted);
    // Send SpawnResponse — does NOT match adapter's From tag (SpawnRequest).
    // Should go through adapter pass-through → interceptor → inner.
    SpawnResponseMessage rsp;
    rsp.set_error_code(1);
    TypedMessage msg(TypeTag::SpawnResponseTag, rsp);
    auto effect = kit.run(msg);

    EXPECT_EQ(effect.kind, EffectKind::Handled);
    EXPECT_TRUE(interceptor_called);
    EXPECT_TRUE(inner_called);
}
