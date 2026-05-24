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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

class LifecycleActorIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.scheduler_threads = 0;
        cfg.enable_network = false;
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

class SimpleLifecycleActor : public EventBasedActor, public LifecycleActor {
  public:
    SimpleLifecycleActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }
};

TEST_F(LifecycleActorIntegrationTest, NoLifecycleReturnsNull) {
    auto actor = system_->spawn<EventBasedActor>();
    EXPECT_EQ(actor.get()->as_lifecycle(), nullptr);
}

TEST_F(LifecycleActorIntegrationTest, LifecycleActorSpawnsActive) {
    auto actor = system_->spawn<SimpleLifecycleActor>();
    auto* lc = actor.get()->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);
}

TEST_F(LifecycleActorIntegrationTest, ToMetadataReportsLifecycleState) {
    auto actor = system_->spawn<SimpleLifecycleActor>();
    auto meta = actor.get()->to_metadata();
    EXPECT_EQ(meta.state, "active");
}

TEST_F(LifecycleActorIntegrationTest, DefaultActorToMetadataUnknown) {
    auto actor = system_->spawn<EventBasedActor>();
    auto meta = actor.get()->to_metadata();
    EXPECT_EQ(meta.state, "unknown");
}