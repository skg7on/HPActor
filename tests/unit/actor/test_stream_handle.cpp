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
#include <hpactor/actor/stream_handle.hpp>
#include <hpactor/core/actor_id.hpp>

using namespace hpactor;

TEST(StreamHandleTest, DefaultConstruction) {
    StreamHandle h;
    EXPECT_FALSE(h.is_open());
    EXPECT_EQ(h.stream_id(), 0u);
}

TEST(StreamHandleTest, ConstructedHandleIsOpen) {
    ActorId sender_id{42};
    StreamHandle h(sender_id, 100);
    EXPECT_TRUE(h.is_open());
    EXPECT_EQ(h.stream_id(), 100u);
}

TEST(StreamHandleTest, MoveConstructTransfersOwnership) {
    ActorId sender_id{42};
    StreamHandle h1(sender_id, 100);
    StreamHandle h2(std::move(h1));
    EXPECT_TRUE(h2.is_open());
    EXPECT_EQ(h2.stream_id(), 100u);
    EXPECT_FALSE(h1.is_open());
}

TEST(StreamHandleTest, MoveAssignTransfersOwnership) {
    ActorId sender_id{42};
    StreamHandle h1(sender_id, 100);
    StreamHandle h2;
    h2 = std::move(h1);
    EXPECT_TRUE(h2.is_open());
    EXPECT_FALSE(h1.is_open());
}

TEST(StreamHandleTest, CloseSetsNotOpen) {
    ActorId sender_id{42};
    StreamHandle h(sender_id, 100);
    bool result = h.close();
    EXPECT_TRUE(result);
    EXPECT_FALSE(h.is_open());
}

TEST(StreamHandleTest, DoubleCloseReturnsFalse) {
    ActorId sender_id{42};
    StreamHandle h(sender_id, 100);
    h.close();
    bool result = h.close();
    EXPECT_FALSE(result);
}

TEST(StreamHandleTest, WriteOnClosedReturnsFalse) {
    ActorId sender_id{42};
    StreamHandle h(sender_id, 100);
    h.close();
    StreamBuffer buf;
    bool result = h.write(TypeTag::User, std::move(buf));
    EXPECT_FALSE(result);
}

TEST(StreamHandleTest, ErrorSetsNotOpen) {
    ActorId sender_id{42};
    StreamHandle h(sender_id, 100);
    bool result = h.error(42, "test error");
    EXPECT_TRUE(result);
    EXPECT_FALSE(h.is_open());
}

TEST(StreamHandleTest, DefaultConstructedHandleIsNotOpen) {
    StreamHandle h;
    EXPECT_FALSE(h.is_open());
    EXPECT_EQ(h.stream_id(), 0u);
}

TEST(StreamHandleTest, MoveAssignFromOpenToDefault) {
    ActorId sid{42};
    StreamHandle h1(sid, 100);
    StreamHandle h2;
    h2 = std::move(h1);
    EXPECT_TRUE(h2.is_open());
    EXPECT_EQ(h2.stream_id(), 100u);
    EXPECT_FALSE(h1.is_open());
}
