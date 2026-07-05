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
#include <hpactor/actor/durable/durable_actor.hpp>
#include <hpactor/actor/durable/durable_state_store.hpp>
#include <hpactor/actor/durable/in_memory_state_store.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/passivation_config.hpp>
#include <hpactor/actor/lifecycle/passivation_manager.hpp>
#include <hpactor/actor/system/actor_route.hpp>
#include <hpactor/actor/system/actor_system.hpp>

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

// ── Capstone: Durable actor for full passivation protocol test ────────

namespace {

/// \brief Test actor that supports both lifecycle and durable passivation.
///
/// Implements IDurableActor so PassivationManager can snapshot and restore
/// its state. Inherits LifecycleActor for the lifecycle state machine.
class TestDurablePassivationActor : public EventBasedActor,
                                    public LifecycleActor,
                                    public IDurableActor {
  public:
    TestDurablePassivationActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    // ── LifecycleActor overrides ─────────────────────────────
    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    // ── IDurableActor overrides ──────────────────────────────
    IDurableActor* as_durable() override {
        return this;
    }
    const IDurableActor* as_durable() const override {
        return this;
    }

    std::string_view persistence_id() const override {
        return persistence_id_;
    }

    result<StreamBuffer> snapshot_state() const override {
        StreamBuffer buf(counter_state_.begin(), counter_state_.end());
        return result<StreamBuffer>::make(std::move(buf));
    }

    result<void> restore_snapshot(const StreamBuffer& data) override {
        counter_state_.assign(data.begin(), data.end());
        return result<void>::make();
    }

    void set_persistence_id(std::string id) {
        persistence_id_ = std::move(id);
    }

    void set_counter(uint64_t c) {
        counter_state_.clear();
        counter_state_.push_back(static_cast<uint8_t>(c & 0xFF));
        counter_state_.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
        counter_state_.push_back(static_cast<uint8_t>((c >> 16) & 0xFF));
        counter_state_.push_back(static_cast<uint8_t>((c >> 24) & 0xFF));
    }

    uint64_t counter() const {
        if (counter_state_.size() < 4)
            return 0;
        return static_cast<uint64_t>(counter_state_[0]) |
               (static_cast<uint64_t>(counter_state_[1]) << 8) |
               (static_cast<uint64_t>(counter_state_[2]) << 16) |
               (static_cast<uint64_t>(counter_state_[3]) << 24);
    }

  private:
    std::string persistence_id_;
    std::vector<uint8_t> counter_state_;
};

} // anonymous namespace

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

// ── Capstone: Full passivation protocol tests ──────────────────────

class PassivationProtocolEndToEndTest : public ::testing::Test {
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

TEST_F(PassivationProtocolEndToEndTest,
       DurableActorExposesLifecycleAndDurableInterfaces) {
    auto spawned = system_->spawn<TestDurablePassivationActor>();
    auto shared = spawned.get();

    // Set up identity and state via concrete type access
    auto* concrete = static_cast<TestDurablePassivationActor*>(shared.get());
    concrete->set_persistence_id("e2e-test-1");
    concrete->set_counter(42);

    // RTTI-free: verify lifecycle interface
    auto* lc = shared->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);

    // RTTI-free: verify durable interface
    auto* d = shared->as_durable();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->persistence_id(), "e2e-test-1");

    // Snapshot via durable interface
    auto snap = d->snapshot_state();
    ASSERT_TRUE(snap.ok());
    EXPECT_EQ(snap.value().size(), 4u);
}

TEST_F(PassivationProtocolEndToEndTest, PassivationManagerPassivatesDurableActor) {
    auto* mgr = system_->passivation_manager();
    ASSERT_NE(mgr, nullptr);

    auto spawned = system_->spawn<TestDurablePassivationActor>();
    auto shared = spawned.get();
    auto* concrete = static_cast<TestDurablePassivationActor*>(shared.get());
    concrete->set_persistence_id("e2e-passivate");
    concrete->set_counter(99);

    ActorId actor_id = shared->id();
    auto* lc = shared->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);

    // Passivate via the manager
    bool result =
        mgr->begin_passivation(actor_id, PassivationRecord::Trigger::kCli);
    EXPECT_TRUE(result);

    // Actor should be in kPassivated state now
    EXPECT_EQ(lc->state(), LifecycleState::kPassivated);

    // Snapshot should be persisted in the durable store
    auto* store = mgr->durable_store();
    ASSERT_NE(store, nullptr);
    auto snap = store->load_latest_snapshot("e2e-passivate");
    ASSERT_TRUE(snap.ok());
    EXPECT_EQ(snap.value().data.size(), 4u);

    // State roundtrip: create a fresh actor, restore from snapshot
    TestDurablePassivationActor restored(nullptr, *system_);
    restored.set_persistence_id("e2e-passivate");
    auto restore_result = restored.restore_snapshot(snap.value().data);
    ASSERT_TRUE(restore_result.ok());
    EXPECT_EQ(restored.counter(), 99u);
}

TEST_F(PassivationProtocolEndToEndTest, BeginPassivationRejectsNonActiveActor) {
    auto* mgr = system_->passivation_manager();
    auto spawned = system_->spawn<TestDurablePassivationActor>();
    auto shared = spawned.get();
    ActorId actor_id = shared->id();

    // First passivation succeeds (actor is kActive)
    bool first = mgr->begin_passivation(actor_id, PassivationRecord::Trigger::kCli);
    EXPECT_TRUE(first);
    EXPECT_EQ(shared->as_lifecycle()->state(), LifecycleState::kPassivated);

    // Second passivation on already-passivated actor fails
    bool second =
        mgr->begin_passivation(actor_id, PassivationRecord::Trigger::kCli);
    EXPECT_FALSE(second);
}

TEST_F(PassivationProtocolEndToEndTest,
       SnapshotRoundtripPreservesStateAcrossInstances) {
    auto* store = system_->passivation_manager()->durable_store();

    // Actor A: set state and snapshot
    auto spawned_a = system_->spawn<TestDurablePassivationActor>();
    auto shared_a = spawned_a.get();
    auto* concrete_a = static_cast<TestDurablePassivationActor*>(shared_a.get());
    concrete_a->set_persistence_id("roundtrip-test");
    concrete_a->set_counter(0xDEADBEEF);

    auto snap = concrete_a->snapshot_state();
    ASSERT_TRUE(snap.ok());
    store->write_snapshot("roundtrip-test", 1, snap.value());

    // Actor B (fresh instance): restore from snapshot
    TestDurablePassivationActor actor_b(nullptr, *system_);
    actor_b.set_persistence_id("roundtrip-test");

    auto loaded = store->load_latest_snapshot("roundtrip-test");
    ASSERT_TRUE(loaded.ok());
    actor_b.restore_snapshot(loaded.value().data);

    EXPECT_EQ(actor_b.counter(), 0xDEADBEEFu);
}

TEST_F(PassivationProtocolEndToEndTest,
       ReactivationProtocolTransitionsThroughRecovering) {
    ActorId aid{100};
    PassivationRecord rec;
    rec.passivated_at = std::chrono::steady_clock::now();

    LocalPassivatedRoute route(aid, "recover-test", rec, 8);

    // Initial: Passivated
    EXPECT_EQ(route.state(), LifecycleState::kPassivated);

    // Claim reactivation → Recovering
    EXPECT_TRUE(route.claim_reactivation());
    EXPECT_EQ(route.state(), LifecycleState::kRecovering);

    // Buffer messages during recovery
    route.try_buffer_message();
    route.try_buffer_message();
    EXPECT_EQ(route.buffered_count(), 2u);

    // Success: route transitions to Active
    route.set_state(LifecycleState::kActive);
    EXPECT_EQ(route.state(), LifecycleState::kActive);
    EXPECT_FALSE(route.reactivation_in_progress());
}

TEST_F(PassivationProtocolEndToEndTest, FailedReactivationTransitionsToFailed) {
    ActorId aid{200};
    PassivationRecord rec;
    LocalPassivatedRoute route(aid, "", rec, 8);

    route.claim_reactivation();
    EXPECT_EQ(route.state(), LifecycleState::kRecovering);

    // Simulate recovery failure
    route.set_state(LifecycleState::kFailed);
    EXPECT_EQ(route.state(), LifecycleState::kFailed);
    EXPECT_FALSE(route.reactivation_in_progress());
}

TEST_F(PassivationProtocolEndToEndTest, RouteShutdownFromPassivatedGoesToStopped) {
    ActorId aid{300};
    PassivationRecord rec;
    LocalPassivatedRoute route(aid, "", rec, 8);

    EXPECT_EQ(route.state(), LifecycleState::kPassivated);

    // Shutdown path: Passivated → Stopped
    route.set_state(LifecycleState::kStopped);
    EXPECT_EQ(route.state(), LifecycleState::kStopped);
    EXPECT_FALSE(route.reactivation_in_progress());
}
