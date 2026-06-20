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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/local_actor.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

// Concrete subclass that exposes LocalActor's protected constructors.
class TestLocalActor : public LocalActor {
  public:
    TestLocalActor(ActorContext* ctx, ActorSystem& sys)
        : LocalActor(ctx, sys) {}
    TestLocalActor(ActorId id, ActorContext* ctx, ActorSystem& sys)
        : LocalActor(id, ctx, sys) {}

    void receive(TypedMessage& /*msg*/) override {}
};

class LocalActorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.scheduler_threads = 0;
        system_ = std::make_unique<ActorSystem>(cfg);
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

TEST_F(LocalActorTest, TwoArgConstructor) {
    TestLocalActor actor(nullptr, *system_);
    EXPECT_EQ(&actor.system(), system_.get());
}

TEST_F(LocalActorTest, ThreeArgConstructor) {
    TestLocalActor actor(ActorId{42}, nullptr, *system_);
    EXPECT_EQ(&actor.system(), system_.get());
    EXPECT_EQ(actor.id().value(), 42u);
}