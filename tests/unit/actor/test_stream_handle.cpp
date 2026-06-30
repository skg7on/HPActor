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
#include <hpactor/actor/stream_types.hpp>
#include <hpactor/core/actor_id.hpp>

using namespace hpactor;

namespace {

// No-op delivery callback for unit tests that don't exercise the delivery path.
bool test_deliver(void* /*ctx*/, ActorId /*target*/, TypedMessage /*msg*/) {
    return true;
}

// Helper to create a StreamHandle with a no-op callback for move-semantics
// and close/error flag tests.
StreamHandle make_test_handle(ActorId sender_id, uint64_t stream_id) {
    return StreamHandle(sender_id, stream_id, test_deliver, nullptr);
}

} // namespace

TEST(StreamHandleTest, DefaultConstruction) {
    StreamHandle h;
    EXPECT_FALSE(h.is_open());
    EXPECT_EQ(h.stream_id(), 0u);
}

TEST(StreamHandleTest, ConstructedHandleIsOpen) {
    auto h = make_test_handle(ActorId{42}, 100);
    EXPECT_TRUE(h.is_open());
    EXPECT_EQ(h.stream_id(), 100u);
}

TEST(StreamHandleTest, MoveConstructTransfersOwnership) {
    auto h1 = make_test_handle(ActorId{42}, 100);
    StreamHandle h2(std::move(h1));
    EXPECT_TRUE(h2.is_open());
    EXPECT_EQ(h2.stream_id(), 100u);
    EXPECT_FALSE(h1.is_open());
}

TEST(StreamHandleTest, MoveAssignTransfersOwnership) {
    auto h1 = make_test_handle(ActorId{42}, 100);
    StreamHandle h2;
    h2 = std::move(h1);
    EXPECT_TRUE(h2.is_open());
    EXPECT_FALSE(h1.is_open());
}

TEST(StreamHandleTest, CloseSetsNotOpen) {
    auto h = make_test_handle(ActorId{42}, 100);
    bool result = h.close();
    EXPECT_TRUE(result);
    EXPECT_FALSE(h.is_open());
}

TEST(StreamHandleTest, DoubleCloseReturnsFalse) {
    auto h = make_test_handle(ActorId{42}, 100);
    h.close();
    bool result = h.close();
    EXPECT_FALSE(result);
}

TEST(StreamHandleTest, WriteOnClosedReturnsFalse) {
    auto h = make_test_handle(ActorId{42}, 100);
    h.close();
    StreamBuffer buf;
    bool result = h.write(TypeTag::User, std::move(buf));
    EXPECT_FALSE(result);
}

TEST(StreamHandleTest, ErrorSetsNotOpen) {
    auto h = make_test_handle(ActorId{42}, 100);
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
    auto h1 = make_test_handle(ActorId{42}, 100);
    StreamHandle h2;
    h2 = std::move(h1);
    EXPECT_TRUE(h2.is_open());
    EXPECT_EQ(h2.stream_id(), 100u);
    EXPECT_FALSE(h1.is_open());
}

TEST(StreamHandleTest, WriteOnOpenHandleReturnsTrue) {
    auto h = make_test_handle(ActorId{42}, 100);
    StreamBuffer buf;
    EXPECT_TRUE(h.write(TypeTag::User, std::move(buf)));
}

TEST(StreamHandleTest, DefaultHandleWriteReturnsFalse) {
    StreamHandle h;
    StreamBuffer buf;
    EXPECT_FALSE(h.write(TypeTag::User, std::move(buf)));
}

TEST(StreamHandleTest, DefaultHandleCloseReturnsFalse) {
    StreamHandle h;
    EXPECT_FALSE(h.close());
}

TEST(StreamHandleTest, DefaultHandleErrorReturnsFalse) {
    StreamHandle h;
    EXPECT_FALSE(h.error(1, "test"));
}
