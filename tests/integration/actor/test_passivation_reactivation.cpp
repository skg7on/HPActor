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

#include <hpactor/actor/actor_route.hpp>
#include <hpactor/actor/durable/in_memory_state_store.hpp>
#include <hpactor/actor/durable_state_store.hpp>
#include <hpactor/actor/passivation_config.hpp>
#include <hpactor/actor/passivation_manager.hpp>
#include <hpactor/core/actor_system.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

class PassivationReactivationIntegrationTest : public ::testing::Test {
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

TEST_F(PassivationReactivationIntegrationTest,
       SystemHasPassivationManagerAfterConstruction) {
    auto* mgr = system_->passivation_manager();
    ASSERT_NE(mgr, nullptr);
    EXPECT_EQ(mgr->default_config().idle_timeout.count(), 0);
    EXPECT_FALSE(mgr->default_config().durable);
}

TEST_F(PassivationReactivationIntegrationTest, DefaultConfigHasDisabledIdleTimeout) {
    auto* mgr = system_->passivation_manager();
    EXPECT_EQ(mgr->default_config().idle_timeout, std::chrono::milliseconds{0});
    EXPECT_FALSE(mgr->default_config().durable);
    EXPECT_TRUE(mgr->default_config().allow_memory_pressure);
}

TEST_F(PassivationReactivationIntegrationTest, DurableStoreIsAvailable) {
    auto* mgr = system_->passivation_manager();
    auto* store = mgr->durable_store();
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->store_type(), "in_memory");
}

TEST_F(PassivationReactivationIntegrationTest, PassivationRecordTriggerValues) {
    EXPECT_EQ(static_cast<uint8_t>(PassivationRecord::Trigger::kIdle), 0);
    EXPECT_EQ(static_cast<uint8_t>(PassivationRecord::Trigger::kSelf), 1);
    EXPECT_EQ(static_cast<uint8_t>(PassivationRecord::Trigger::kMemoryPressure), 2);
    EXPECT_EQ(static_cast<uint8_t>(PassivationRecord::Trigger::kCli), 3);
}

TEST_F(PassivationReactivationIntegrationTest, LocalPassivatedRouteLifecycle) {
    ActorId aid{42};
    PassivationRecord rec;
    rec.passivated_at = std::chrono::steady_clock::now();

    LocalPassivatedRoute route(aid, "test-persist", rec, 8);

    EXPECT_EQ(route.state(), LifecycleState::kPassivated);
    EXPECT_FALSE(route.is_active());
    EXPECT_EQ(route.actor_id().value(), 42u);
    EXPECT_EQ(route.persistence_id(), "test-persist");

    // Claim reactivation
    EXPECT_TRUE(route.claim_reactivation());
    EXPECT_TRUE(route.reactivation_in_progress());
    EXPECT_EQ(route.state(), LifecycleState::kRecovering);

    // Buffer messages during reactivation
    EXPECT_TRUE(route.try_buffer_message());
    EXPECT_TRUE(route.try_buffer_message());
    EXPECT_EQ(route.buffered_count(), 2u);

    // Complete reactivation
    route.set_state(LifecycleState::kActive);
    EXPECT_EQ(route.state(), LifecycleState::kActive);
}

TEST_F(PassivationReactivationIntegrationTest, BufferFullRejectsNewMessages) {
    ActorId aid{1};
    PassivationRecord rec;
    LocalPassivatedRoute route(aid, "", rec, 4);

    // Fill the buffer
    EXPECT_TRUE(route.try_buffer_message());
    EXPECT_TRUE(route.try_buffer_message());
    EXPECT_TRUE(route.try_buffer_message());
    EXPECT_TRUE(route.try_buffer_message());
    EXPECT_EQ(route.buffered_count(), 4u);

    // Buffer full
    EXPECT_FALSE(route.try_buffer_message());
    EXPECT_EQ(route.buffered_count(), 4u);
}

TEST_F(PassivationReactivationIntegrationTest, DurableStateStoreRoundtrip) {
    auto* mgr = system_->passivation_manager();
    auto* store = mgr->durable_store();
    ASSERT_NE(store, nullptr);

    // Write a snapshot
    StreamBuffer data{10, 20, 30, 40};
    auto write = store->write_snapshot("actor-int-test", 2, data);
    ASSERT_TRUE(write.ok());
    EXPECT_EQ(write.value().persistence_id, "actor-int-test");
    EXPECT_EQ(write.value().schema_version, 2u);

    // Read it back
    auto read = store->load_latest_snapshot("actor-int-test");
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().data.size(), 4u);
    EXPECT_EQ(read.value().data[0], 10);
    EXPECT_EQ(read.value().data[3], 40);
    EXPECT_EQ(read.value().schema_version, 2u);

    // Delete and verify gone
    auto del = store->delete_state("actor-int-test");
    EXPECT_TRUE(del.ok());
    auto missing = store->load_latest_snapshot("actor-int-test");
    EXPECT_FALSE(missing.ok());
}

TEST_F(PassivationReactivationIntegrationTest, IndependentPersistenceIds) {
    auto* store = system_->passivation_manager()->durable_store();

    store->write_snapshot("actor-A", 1, StreamBuffer{1, 2});
    store->write_snapshot("actor-B", 3, StreamBuffer{3, 4, 5});

    auto a = store->load_latest_snapshot("actor-A");
    auto b = store->load_latest_snapshot("actor-B");

    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value().data.size(), 2u);
    EXPECT_EQ(b.value().data.size(), 3u);
    EXPECT_EQ(a.value().schema_version, 1u);
    EXPECT_EQ(b.value().schema_version, 3u);
}
