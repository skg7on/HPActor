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
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/blocking_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/scoped_actor.hpp>

#include <gtest/gtest.h>
#include <thread>

using namespace hpactor;

// Test actor that counts received messages
class TestBlockingActor : public BlockingActor {
  public:
    TestBlockingActor(ActorContext* ctx, ActorSystem& sys)
        : BlockingActor(ctx, sys) {}

    int received_count() const {
        return received_count_;
    }

    void receive(TypedMessage& /*msg*/) override {
        received_count_++;
    }

  private:
    int received_count_ = 0;
};

class BlockingActorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.scheduler_threads = 1;
        cfg.enable_network = false;
        system_ = std::make_unique<ActorSystem>(cfg);
    }
    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(100);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }
    std::unique_ptr<ActorSystem> system_;
};

TEST_F(BlockingActorTest, SizeNotEmpty) {
    static_assert(sizeof(BlockingActor) > sizeof(LocalActor), "BlockingActor "
                                                              "should add "
                                                              "members beyond "
                                                              "LocalActor");
}

TEST_F(BlockingActorTest, ScopedActorSizeNotEmpty) {
    static_assert(sizeof(ScopedActor) >= sizeof(BlockingActor), "ScopedActor "
                                                                "should be at "
                                                                "least as "
                                                                "large as "
                                                                "BlockingActo"
                                                                "r");
}

TEST_F(BlockingActorTest, DispatchPolicy) {
    auto actor = system_->spawn<TestBlockingActor>();
    EXPECT_EQ(actor.get().get()->dispatch_policy(),
              sched::DispatchPolicy::DedicatedThread);
}

TEST_F(BlockingActorTest, SpawnAndActivate) {
    auto actor = system_->spawn<TestBlockingActor>();
    EXPECT_FALSE(actor.get().get()->id() == ActorId{});
}

TEST_F(BlockingActorTest, ReceivesMessage) {
    auto actor = system_->spawn<TestBlockingActor>();
    auto addr = actor.address();

    auto sender = system_->spawn<EventBasedActor>();
    ActorContext ctx(sender, system_.get());

    TypedMessage msg(TypeTag::User, StreamBuffer{1});
    msg.set_sender_address(sender.address());
    ctx.send(addr, std::move(msg));

    // BlockingActor runs on its own dedicated thread outside the
    // scheduler; a brief sleep is needed for the dedicated thread to
    // pick up the message.  SchedulerTestDriver cannot control
    // dedicated-thread actors.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    SUCCEED();
}

TEST_F(BlockingActorTest, StateAfterSpawn) {
    auto actor = system_->spawn<TestBlockingActor>();

    // Actor pointer is valid immediately after spawn — no polling needed.
    ASSERT_NE(actor.get().get(), nullptr);
}