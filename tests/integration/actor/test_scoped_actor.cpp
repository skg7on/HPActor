// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/scoped_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

class ScopedActorTest : public ::testing::Test {
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

TEST_F(ScopedActorTest, ConstructDestruct) {
    ScopedActor actor(*system_);
    EXPECT_EQ(&actor.system(), system_.get());
}
