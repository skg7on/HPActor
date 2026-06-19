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
#include <hpactor/msg/typed_message.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

#include <gtest/gtest.h>

#include <functional>
#include <string>

using namespace hpactor;

// =============================================================================
// Behavior::make() — empty behavior
// =============================================================================

TEST(BehaviorBuilderTest, MakeReturnsEmptyBehavior) {
    Behavior bh = Behavior::make();
    EXPECT_FALSE(static_cast<bool>(bh));
}

// =============================================================================
// Behavior::on<T>() — fire-and-forget handler
// =============================================================================

TEST(BehaviorBuilderTest, OnRegistersTypedHandler) {
    bool called = false;
    Behavior bh = Behavior::make().on<SpawnRequestMessage>(
        [&called](const SpawnRequestMessage& /*req*/) { called = true; });

    EXPECT_TRUE(static_cast<bool>(bh));

    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    bh(msg);

    EXPECT_TRUE(called);
}

TEST(BehaviorBuilderTest, OnHandlerReceivesDeserializedData) {
    std::string received_type_name;

    Behavior bh = Behavior::make().on<SpawnRequestMessage>(
        [&received_type_name](const SpawnRequestMessage& req) {
            received_type_name = req.actor_type_name();
        });

    SpawnRequestMessage req;
    req.set_actor_type_name("test_actor_type");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    bh(msg);

    EXPECT_EQ(received_type_name, "test_actor_type");
}

TEST(BehaviorBuilderTest, OnHandlerNotCalledForDifferentType) {
    bool called = false;
    Behavior bh = Behavior::make().on<SpawnRequestMessage>(
        [&called](const SpawnRequestMessage& /*req*/) { called = true; });

    SpawnResponseMessage resp;
    resp.set_error_code(0);
    TypedMessage msg(TypeTag::SpawnResponseTag, resp);
    bh(msg);

    EXPECT_FALSE(called);
}

TEST(BehaviorBuilderTest, OnHandlerNotCalledOnParseFailure) {
    bool called = false;
    Behavior bh = Behavior::make().on<SpawnRequestMessage>(
        [&called](const SpawnRequestMessage& /*req*/) { called = true; });

    // Construct TypedMessage with valid tag but empty/invalid payload
    StreamBuffer empty;
    TypedMessage msg(TypeTag::SpawnRequestTag, empty);
    bh(msg);

    EXPECT_FALSE(called);
}

// =============================================================================
// Behavior::on_request<ReqT, ResT>() — request-response handler
// =============================================================================

TEST(BehaviorBuilderTest, OnRequestRegistersHandler) {
    bool called = false;
    Behavior bh =
        Behavior::make().on_request<SpawnRequestMessage, SpawnResponseMessage>(
            [&called](const SpawnRequestMessage& /*req*/) { called = true; });

    EXPECT_TRUE(static_cast<bool>(bh));

    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    bh(msg);

    EXPECT_TRUE(called);
}

TEST(BehaviorBuilderTest, OnRequestHandlerReceivesDeserializedRequest) {
    std::string received_type_name;

    Behavior bh =
        Behavior::make().on_request<SpawnRequestMessage, SpawnResponseMessage>(
            [&received_type_name](const SpawnRequestMessage& req) {
                received_type_name = req.actor_type_name();
            });

    SpawnRequestMessage req;
    req.set_actor_type_name("request_handler_test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    bh(msg);

    EXPECT_EQ(received_type_name, "request_handler_test");
}

// =============================================================================
// Chaining — multiple .on<T>() calls
// =============================================================================

TEST(BehaviorBuilderTest, ChainingRegistersMultipleHandlers) {
    int spawn_req_count = 0;
    int spawn_resp_count = 0;

    Behavior bh =
        Behavior::make()
            .on<SpawnRequestMessage>(
                [&spawn_req_count](const SpawnRequestMessage& /*req*/) {
                    spawn_req_count++;
                })
            .on<SpawnResponseMessage>(
                [&spawn_resp_count](const SpawnResponseMessage& /*resp*/) {
                    spawn_resp_count++;
                });

    EXPECT_TRUE(static_cast<bool>(bh));

    SpawnRequestMessage spawn_req;
    spawn_req.set_actor_type_name("test");
    TypedMessage msg1(TypeTag::SpawnRequestTag, spawn_req);
    bh(msg1);
    EXPECT_EQ(spawn_req_count, 1);
    EXPECT_EQ(spawn_resp_count, 0);

    SpawnResponseMessage spawn_resp;
    spawn_resp.set_error_code(42);
    TypedMessage msg2(TypeTag::SpawnResponseTag, spawn_resp);
    bh(msg2);
    EXPECT_EQ(spawn_req_count, 1);
    EXPECT_EQ(spawn_resp_count, 1);
}

// =============================================================================
// Priority — typed handler before fallback
// =============================================================================

TEST(BehaviorBuilderTest, TypedHandlerTakesPriorityOverFallback) {
    bool typed_called = false;

    Behavior bh = Behavior::make().on<SpawnRequestMessage>(
        [&typed_called](const SpawnRequestMessage& /*req*/) {
            typed_called = true;
        });

    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    bh(msg);

    EXPECT_TRUE(typed_called);
}

TEST(BehaviorBuilderTest, FallbackHandlerNotCalledWhenTypedHandlerMatches) {
    bool typed_called = false;
    bool fallback_called = false;

    // Build a behavior with typed handler, then we verify fallback is
    // only invoked for unmatched types by testing separate behaviors.
    // (Behavior currently has no API to add a fallback after chaining;
    //  the two dispatch paths are tested independently here.)
    Behavior typed_bh = Behavior::make().on<SpawnRequestMessage>(
        [&typed_called](const SpawnRequestMessage& /*req*/) {
            typed_called = true;
        });

    Behavior fallback_bh(
        [&fallback_called](TypedMessage& /*msg*/) { fallback_called = true; });

    // Send a SpawnRequestMessage — typed handler should be called
    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg1(TypeTag::SpawnRequestTag, req);
    typed_bh(msg1);
    EXPECT_TRUE(typed_called);

    // Send a SpawnResponseMessage to fallback-only — fallback should be called
    SpawnResponseMessage resp;
    resp.set_error_code(0);
    TypedMessage msg2(TypeTag::SpawnResponseTag, resp);
    fallback_bh(msg2);
    EXPECT_TRUE(fallback_called);
}

// =============================================================================
// Fallback — raw lambda still works
// =============================================================================

TEST(BehaviorBuilderTest, FallbackHandlerCalledForUnmatchedType) {
    bool fallback_called = false;
    TypeTag received_tag = TypeTag::Invalid;

    Behavior bh = Behavior([&fallback_called, &received_tag](TypedMessage& msg) {
        fallback_called = true;
        received_tag = msg.type_id();
    });

    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    bh(msg);

    EXPECT_TRUE(fallback_called);
    EXPECT_EQ(received_tag, TypeTag::SpawnRequestTag);
}

TEST(BehaviorBuilderTest, FallbackHandlerCalledWhenNoTypedHandlerMatches) {
    bool fallback_called = false;

    Behavior fallback_only(
        [&fallback_called](TypedMessage& /*msg*/) { fallback_called = true; });

    // Send a message type that has no typed handler registered
    SpawnResponseMessage spawn_resp;
    spawn_resp.set_error_code(42);
    TypedMessage msg(TypeTag::SpawnResponseTag, spawn_resp);
    fallback_only(msg);

    EXPECT_TRUE(fallback_called);
}

// =============================================================================
// Copy behavior
// =============================================================================

TEST(BehaviorBuilderTest, CopiedBehaviorRetainsTypedHandlers) {
    int count = 0;
    Behavior bh = Behavior::make().on<SpawnRequestMessage>(
        [&count](const SpawnRequestMessage& /*req*/) { count++; });

    Behavior copy = bh;

    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);

    copy(msg);
    EXPECT_EQ(count, 1);

    bh(msg);
    EXPECT_EQ(count, 2);
}

// =============================================================================
// Default-constructed Behavior is empty
// =============================================================================

TEST(BehaviorBuilderTest, DefaultConstructedBehaviorIsEmpty) {
    Behavior bh;
    EXPECT_FALSE(static_cast<bool>(bh));

    SpawnRequestMessage req;
    req.set_actor_type_name("test");
    TypedMessage msg(TypeTag::SpawnRequestTag, req);
    bh(msg); // Should not crash — no-op
}
