// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

class IsSystemActorTest : public ::testing::Test {
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

// Test-only system actor that overrides is_system_actor() to return true.
class TestSystemActor : public EventBasedActor {
  public:
    TestSystemActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    bool is_system_actor() const override {
        return true;
    }
};

TEST_F(IsSystemActorTest, UserActorIsNotSystem) {
    auto actor = system_->spawn<EventBasedActor>();
    EXPECT_FALSE(actor.get()->is_system_actor());
}

TEST_F(IsSystemActorTest, SystemActorsReturnTrue) {
    auto actor = system_->spawn<TestSystemActor>();
    EXPECT_TRUE(actor.get()->is_system_actor());
}
