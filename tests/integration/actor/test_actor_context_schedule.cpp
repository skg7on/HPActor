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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace hpactor;

namespace {

class ScheduleTestActor : public EventBasedActor {
  public:
    ScheduleTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    AlarmHandle trigger_schedule(std::chrono::milliseconds delay) {
        return context()->schedule(delay,
                                   TypedMessage(TypeTag::User, StreamBuffer{42}));
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[](TypedMessage&) {
            // No-op — delivery is verified via mailbox inspection.
        }};
    }
};

} // namespace

// Fixture for schedule tests
class ActorContextScheduleTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config;
        config.scheduler_threads = 0;
        system_ = std::make_unique<ActorSystem>(config);
    }
    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(10);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }
    std::unique_ptr<ActorSystem> system_;
};

TEST_F(ActorContextScheduleTest, ScheduleReturnsValidHandle) {
    auto handle = system_->spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());

    auto alarm = actor->trigger_schedule(std::chrono::milliseconds(5000));
    EXPECT_NE(alarm.value(), 0u);

    actor->context()->cancel_schedule(alarm);
}

TEST_F(ActorContextScheduleTest, CancelInvalidHandlesSafe) {
    auto handle = system_->spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());

    // Must not crash or assert.
    actor->context()->cancel_schedule(AlarmHandle{});
    actor->context()->cancel_schedule(AlarmHandle{0});
    SUCCEED();
}

TEST_F(ActorContextScheduleTest, CancelPreventsMailboxDelivery) {
    auto handle = system_->spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());

    auto alarm = actor->trigger_schedule(std::chrono::milliseconds(200));
    EXPECT_NE(alarm.value(), 0u);

    // Cancel before the timer thread advances past the expiry.
    actor->context()->cancel_schedule(alarm);
    actor->context()->cancel_schedule(alarm); // double cancel is harmless

    // Wait long enough for the timer to have fired had it NOT been cancelled.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Direct mailbox inspection — no worker thread involved.
    auto* mailbox = system_->get_mailbox(actor->id());
    ASSERT_NE(mailbox, nullptr);

    TypedMessage msg;
    bool popped = mailbox->try_pop(msg);
    EXPECT_FALSE(popped);
}
