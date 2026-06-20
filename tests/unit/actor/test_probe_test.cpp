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

#include <hpactor/actor/testing/test_probe.hpp>

#include <hpactor/core/actor_system.hpp>
#include <hpactor/core/proto_type_registry.hpp>

#include <gtest/gtest.h>

#include "hpactor/messages.pb.h"
#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

using namespace hpactor;
using namespace hpactor::testing;

namespace {

template <typename T> TypedMessage make_typed_msg(const T& proto) {
    return TypedMessage(MessageTraits<T>::tag(), proto);
}

} // namespace

class TestProbeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto cfg = test::config_with_scheduler(1);
        system_ = std::make_unique<ActorSystem>(cfg);
        driver_ = std::make_unique<test::SchedulerTestDriver>(*system_);
    }

    std::unique_ptr<ActorSystem> system_;
    std::unique_ptr<test::SchedulerTestDriver> driver_;
};

TEST_F(TestProbeTest, ConstructsAndProvidesAddress) {
    TestProbe probe(*system_);
    auto addr = probe.address();
    EXPECT_NE(addr.id, ActorId{});
}

TEST_F(TestProbeTest, StartsWithEmptyQueue) {
    TestProbe probe(*system_);
    EXPECT_EQ(probe.queue_size(), 0u);
    EXPECT_TRUE(probe.queue().empty());
}

TEST_F(TestProbeTest, ReceivesMessageViaTryDeliverLocal) {
    TestProbe probe(*system_);
    SpawnRequestMessage proto_msg;
    proto_msg.set_actor_type_name("test-actor");

    system_->try_deliver_local(probe.address().id, make_typed_msg(proto_msg));
    driver_->drain(10);

    EXPECT_GE(probe.queue_size(), 1u);
}

TEST_F(TestProbeTest, ClearEmptiesQueue) {
    TestProbe probe(*system_);
    SpawnRequestMessage proto_msg;

    system_->try_deliver_local(probe.address().id, make_typed_msg(proto_msg));
    driver_->drain(10);
    EXPECT_GE(probe.queue_size(), 1u);

    probe.clear();
    EXPECT_EQ(probe.queue_size(), 0u);
}

TEST_F(TestProbeTest, ExpectNoMessagePassesForEmptyQueue) {
    TestProbe probe(*system_);
    probe.expect_no_message(MessageTraits<SpawnRequestMessage>::tag());
}

TEST_F(TestProbeTest, ExpectNoMessagePassesForDifferentType) {
    TestProbe probe(*system_);
    SpawnRequestMessage proto_msg;

    system_->try_deliver_local(probe.address().id, make_typed_msg(proto_msg));
    driver_->drain(10);
    EXPECT_GE(probe.queue_size(), 1u);

    // Check for a DIFFERENT tag — should pass.
    probe.expect_no_message(MessageTraits<SpawnResponseMessage>::tag());
}

TEST_F(TestProbeTest, FishForMessageFindsMatching) {
    TestProbe probe(*system_);
    SpawnResponseMessage proto_msg;
    proto_msg.set_error_code(42);

    system_->try_deliver_local(probe.address().id, make_typed_msg(proto_msg));
    driver_->drain(10);

    auto* found = probe.fish_for_message<SpawnResponseMessage>(
        [](const SpawnResponseMessage& r) { return r.error_code() == 42; },
        MessageTraits<SpawnResponseMessage>::tag());
    EXPECT_NE(found, nullptr);
    EXPECT_EQ(found->error_code(), 42);
}

TEST_F(TestProbeTest, FishForMessageReturnsNullOnWrongType) {
    TestProbe probe(*system_);
    SpawnRequestMessage proto_msg;

    system_->try_deliver_local(probe.address().id, make_typed_msg(proto_msg));
    driver_->drain(10);

    auto* found = probe.fish_for_message<SpawnResponseMessage>(
        [](const SpawnResponseMessage&) { return true; },
        MessageTraits<SpawnResponseMessage>::tag());
    EXPECT_EQ(found, nullptr);
}

TEST_F(TestProbeTest, FishForMessageReturnsNullOnNonMatchingPredicate) {
    TestProbe probe(*system_);
    SpawnResponseMessage proto_msg;
    proto_msg.set_error_code(0);

    system_->try_deliver_local(probe.address().id, make_typed_msg(proto_msg));
    driver_->drain(10);

    auto* found = probe.fish_for_message<SpawnResponseMessage>(
        [](const SpawnResponseMessage& r) { return r.error_code() == 99; },
        MessageTraits<SpawnResponseMessage>::tag());
    EXPECT_EQ(found, nullptr);
}

TEST_F(TestProbeTest, MultipleMessagesStackUp) {
    TestProbe probe(*system_);
    SpawnRequestMessage p1, p2, p3;

    system_->try_deliver_local(probe.address().id, make_typed_msg(p1));
    system_->try_deliver_local(probe.address().id, make_typed_msg(p2));
    system_->try_deliver_local(probe.address().id, make_typed_msg(p3));
    driver_->drain(10);

    EXPECT_EQ(probe.queue_size(), 3u);
}
