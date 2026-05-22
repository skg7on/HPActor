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

#include <cstdint>
#include <gtest/gtest.h>
#include <hpactor/types/types.hpp>

// Test 1: ActorId default construction (value == 0)
TEST(TypesTest, ActorIdDefaultConstruction) {
    hpactor::ActorId default_actor_id;
    EXPECT_EQ(default_actor_id.value(), 0);
}

// Test 2: ActorId explicit construction from counter_type
TEST(TypesTest, ActorIdExplicitConstruction) {
    hpactor::ActorId explicit_actor_id(42);
    EXPECT_EQ(explicit_actor_id.value(), 42);
}

// Test 3: ActorId equality
TEST(TypesTest, ActorIdEquality) {
    hpactor::ActorId actor_id1(100);
    hpactor::ActorId actor_id2(100);
    EXPECT_EQ(actor_id1, actor_id2);
}

// Test 4: ActorId inequality
TEST(TypesTest, ActorIdInequality) {
    hpactor::ActorId actor_id1(100);
    hpactor::ActorId actor_id3(200);
    EXPECT_NE(actor_id1, actor_id3);
}

// Test 5: ActorId value accessor
TEST(TypesTest, ActorIdValueAccessor) {
    hpactor::ActorId actor_id1(100);
    EXPECT_EQ(actor_id1.value(), 100);
}

// Test 6: LocalEndpoint
TEST(TypesTest, LocalEndpoint) {
    EXPECT_TRUE(hpactor::LocalEndpoint.is_loopback());
    EXPECT_EQ(hpactor::LocalEndpoint.port(), 0);
}

// Test 7: ActorType with InvalidActorType
TEST(TypesTest, ActorTypeInvalid) {
    hpactor::ActorType actor_type = hpactor::InvalidActorType;
    EXPECT_EQ(actor_type, hpactor::InvalidActorType);
}

// Test 8: error class
TEST(TypesTest, ErrorClass) {
    hpactor::error ok_err;
    EXPECT_TRUE(ok_err.ok());
    EXPECT_FALSE(ok_err);

    hpactor::error err(42, "test error");
    EXPECT_FALSE(err.ok());
    EXPECT_TRUE(err);
    EXPECT_EQ(err.code(), 42);
    EXPECT_EQ(err.message(), "test error");
}

// Test 9: errors namespace
TEST(TypesTest, ErrorCodes) {
    EXPECT_EQ(hpactor::errors::unknown, 1);
    EXPECT_EQ(hpactor::errors::actor_down, 2);
    EXPECT_EQ(hpactor::errors::actor_not_found, 3);
    EXPECT_EQ(hpactor::errors::mailbox_full, 4);
    EXPECT_EQ(hpactor::errors::timeout, 5);
    EXPECT_EQ(hpactor::errors::user, 1000);
}

// Test 10: MessageId generate
TEST(TypesTest, MessageIdUnique) {
    hpactor::MessageId id1 = hpactor::generate_message_id();
    hpactor::MessageId id2 = hpactor::generate_message_id();
    EXPECT_NE(id1, id2); // Each call should be unique
}

// Test 11: Clock
TEST(TypesTest, ClockOperations) {
    hpactor::Clock clock;
    hpactor::Clock::time_point tp = clock.now();
    hpactor::Clock::duration dur = hpactor::Clock::duration(100);
    hpactor::Clock::time_point tp2 = tp + dur;
    EXPECT_GT(tp2, tp);
}

// Test 12: AlarmHandle
TEST(TypesTest, AlarmHandle) {
    hpactor::AlarmHandle handle1;
    hpactor::AlarmHandle handle2(42);
    EXPECT_EQ(handle1.value(), 0);
    EXPECT_EQ(handle2.value(), 42);
}

// Test 13: TraceContext
TEST(TypesTest, TraceContext) {
    hpactor::TraceContext ctx;
    ctx.trace_id.bytes[15] = 1;
    ctx.span_id.bytes[7] = 2;
    ctx.flags.value = 3;
    EXPECT_TRUE(ctx.trace_id.valid());
    EXPECT_TRUE(ctx.span_id.valid());
    EXPECT_EQ(ctx.flags.value, 3);
}

// Test 14: StreamBuffer
TEST(TypesTest, StreamBuffer) {
    hpactor::StreamBuffer data = {1, 2, 3, 4, 5};
    EXPECT_EQ(data.size(), 5);
    EXPECT_EQ(data[0], 1);
    EXPECT_EQ(data[4], 5);
}
