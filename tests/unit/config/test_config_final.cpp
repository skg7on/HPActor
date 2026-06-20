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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/config/binary_loader.hpp>
#include <hpactor/config/binary_serializer.hpp>
#include <hpactor/config/toml_table_view.hpp>
#include <hpactor/config/topology_model.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hpactor;
using namespace hpactor::config;

// =============================================================================
// Test 1: TomlTableView edge cases
// =============================================================================

TEST(TomlTableViewFinalTest, DefaultConstructedIsInvalid) {
    TomlTableView view;
    EXPECT_FALSE(view.valid());
}

TEST(TomlTableViewFinalTest, InvalidViewReturnsFallbacks) {
    TomlTableView view;
    EXPECT_EQ(view.read_string("key", "fallback"), "fallback");
    EXPECT_EQ(view.read_uint32("key", 42u), 42u);
    EXPECT_EQ(view.read_bool("key", true), true);
    EXPECT_EQ(view.read_double("key", 3.14), 3.14);
    EXPECT_FALSE(view.contains("any_key"));
}

TEST(TomlValueViewTest, DefaultConstructedIsMissing) {
    TomlValueView vv;
    EXPECT_EQ(vv.kind(), TomlValueView::Kind::Missing);
    EXPECT_FALSE(vv.is_string());
    EXPECT_FALSE(vv.is_integer());
    EXPECT_FALSE(vv.is_floating_point());
    EXPECT_FALSE(vv.is_boolean());
    EXPECT_EQ(vv.as_string("default"), "default");
    EXPECT_EQ(vv.as_int64(42), 42);
    EXPECT_EQ(vv.as_double(1.0), 1.0);
    EXPECT_EQ(vv.as_bool(true), true);
}

// =============================================================================
// Test 2: BinaryLoader/BinarySerializer roundtrip
// =============================================================================

TEST(BinarySerializerFinalTest, SerializeEmptyTopology) {
    TopologyModel model;
    model.system.version = "1.0.0-test";

    auto blob = serialize_topology(model);
    EXPECT_FALSE(blob.empty());
}

TEST(BinarySerializerFinalTest, SerializeTopologyWithActors) {
    TopologyModel model;
    model.system.version = "2.0.0";

    DispatcherDef disp;
    disp.name = "default";
    disp.threads = 1;
    model.dispatchers.push_back(disp);

    ActorDef actor;
    actor.id = "test-actor-1";
    actor.behavior = "TestActor";
    actor.supervisor = "root";
    actor.dispatcher = "default";
    model.actors.push_back(actor);

    auto blob = serialize_topology(model);
    EXPECT_FALSE(blob.empty());
}

TEST(BinarySerializerFinalTest, SerializeTopologyWithMultipleActors) {
    TopologyModel model;
    model.system.version = "3.0.0";

    for (int i = 0; i < 5; ++i) {
        ActorDef actor;
        actor.id = "actor-" + std::to_string(i);
        actor.behavior = "WorkerActor";
        model.actors.push_back(actor);
    }

    auto blob = serialize_topology(model);
    EXPECT_FALSE(blob.empty());
    EXPECT_GT(blob.size(), 20u);
}

TEST(BinarySerializerFinalTest, SerializeProducesDeterministicOutput) {
    TopologyModel model;
    model.system.version = "4.0.0";

    ActorDef actor;
    actor.id = "deterministic";
    actor.behavior = "EchoActor";
    model.actors.push_back(actor);

    auto blob1 = serialize_topology(model);
    auto blob2 = serialize_topology(model);
    EXPECT_EQ(blob1, blob2);
}

// =============================================================================
// Test 3: ActorFactoryRegistry operations
// =============================================================================

namespace {

class FinalConfigTestActor : public EventBasedActor {
  public:
    FinalConfigTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

class FinalConfigTestWorker : public EventBasedActor {
  public:
    FinalConfigTestWorker(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

} // namespace

class ActorFactoryRegistryFinalTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto& reg = ActorFactoryRegistry::instance();
        reg.register_factory<FinalConfigTestActor>("FinalTestActor");
        reg.register_factory<FinalConfigTestWorker>("FinalTestWorker");
    }
};

TEST_F(ActorFactoryRegistryFinalTest, KnownNamesContainsRegistered) {
    auto& reg = ActorFactoryRegistry::instance();
    auto names = reg.known_names();

    bool found_actor = false;
    bool found_worker = false;
    for (const auto& name : names) {
        if (name == "FinalTestActor")
            found_actor = true;
        if (name == "FinalTestWorker")
            found_worker = true;
    }
    EXPECT_TRUE(found_actor);
    EXPECT_TRUE(found_worker);
}

TEST_F(ActorFactoryRegistryFinalTest, HasReturnsCorrectly) {
    auto& reg = ActorFactoryRegistry::instance();
    EXPECT_TRUE(reg.has("FinalTestActor"));
    EXPECT_FALSE(reg.has("CompletelyMadeUpActorName"));
}

TEST_F(ActorFactoryRegistryFinalTest, GetFactoryReturnsNonNullForRegistered) {
    auto& reg = ActorFactoryRegistry::instance();
    auto f = reg.get_factory("FinalTestActor");
    // ActorFactory is a std::function — verify it's callable/non-empty
    EXPECT_NE(f, nullptr);
}

// =============================================================================
// Test 4: TopologyModel validation
// =============================================================================

TEST(TopologyModelFinalTest, DefaultConstruction) {
    TopologyModel model;
    EXPECT_TRUE(model.system.version.empty());
    EXPECT_TRUE(model.dispatchers.empty());
    EXPECT_TRUE(model.actors.empty());
}

TEST(TopologyModelFinalTest, ActorDefDefaults) {
    ActorDef def;
    EXPECT_TRUE(def.id.empty());
    EXPECT_TRUE(def.behavior.empty());
    EXPECT_TRUE(def.supervisor.empty());
    EXPECT_TRUE(def.dispatcher.empty());
    EXPECT_EQ(def.dispatch_policy, DispatchPolicy::Cooperative);
    EXPECT_EQ(def.mailbox_capacity, 0u);
}

TEST(TopologyModelFinalTest, DispatcherDefDefaults) {
    DispatcherDef def;
    EXPECT_TRUE(def.name.empty());
    EXPECT_EQ(def.threads, 1u);
    EXPECT_TRUE(def.cpu_affinity.empty());
}

// =============================================================================
// Test 5: System fields defaults (from SystemDef)
// =============================================================================

TEST(SystemDefFinalTest, SystemDefDefaults) {
    SystemDef sys;
    EXPECT_TRUE(sys.version.empty());
    EXPECT_EQ(sys.scheduler_threads, 4u);
    EXPECT_EQ(sys.default_mailbox_size, 1024u);
    EXPECT_FALSE(sys.use_coroutines);
    EXPECT_TRUE(sys.metrics_enabled);
    EXPECT_EQ(sys.default_drain_policy, "Drain");
    EXPECT_EQ(sys.default_drain_timeout_ms, 30000u);
    EXPECT_TRUE(sys.shutdown_force_after_timeout);
}

TEST(SystemDefFinalTest, DeliveryConfigDefaults) {
    SystemDef sys;
    EXPECT_EQ(sys.delivery.default_mode, hpactor::mailbox::DeliveryMode::BestEffort);
    EXPECT_EQ(sys.delivery.max_retries, 3u);
    EXPECT_EQ(sys.delivery.retry_backoff_ms, 100u);
    EXPECT_EQ(sys.delivery.retry_backoff_max_ms, 10000u);
    EXPECT_EQ(sys.delivery.dedup_window_ms, 300000u);
    EXPECT_EQ(sys.delivery.dedup_max_entries, 65536u);
    EXPECT_EQ(sys.delivery.default_message_ttl_ms, 0u);
}

TEST(SystemDefFinalTest, DeadLetterConfigDefaults) {
    SystemDef sys;
    // dead_letters exists with default state (enabled by default)
    EXPECT_TRUE(sys.dead_letters.enabled);
}

// =============================================================================
// Test 6: Config validation edge cases
// =============================================================================

TEST(ConfigFinalTest, DefaultConfigValues) {
    Config cfg;
    EXPECT_EQ(cfg.scheduler_threads, 4u);
    EXPECT_FALSE(cfg.enable_network);
    EXPECT_FALSE(cfg.cli.enabled);
    EXPECT_FALSE(cfg.tracing.enabled);
}

TEST(ResourceSpecFinalTest, Defaults) {
    ResourceSpec rs;
    EXPECT_EQ(rs.slab_class_bytes, 0u);
    EXPECT_EQ(rs.max_memory_kb, 0u);
}

TEST(MailboxPolicyDefFinalTest, Defaults) {
    MailboxPolicyDef mp;
    EXPECT_EQ(mp.policy, hpactor::mailbox::OverflowPolicy::RejectNewest);
    EXPECT_FALSE(mp.priority_aware);
    EXPECT_EQ(mp.priority_levels, 4u);
    EXPECT_EQ(mp.max_overflow_depth, 0u);
    EXPECT_EQ(mp.high_watermark, 0.0);
    EXPECT_EQ(mp.low_watermark, 0.0);
    EXPECT_EQ(mp.critical_watermark, 0.0);
    EXPECT_EQ(mp.signal_min_interval_ms, 0u);
    EXPECT_EQ(mp.backpressure_mode,
              hpactor::mailbox::BackpressureMode::LocalAndRemoteSignal);
}

// =============================================================================
// Test 7: SystemMailboxDef defaults (from X-macro expansion)
// =============================================================================

TEST(SystemMailboxDefFinalTest, Defaults) {
    SystemMailboxDef smb;
    // default_capacity is macro-generated; verify the field exists
    EXPECT_EQ(smb.default_capacity, 1024u);
    EXPECT_EQ(smb.default_policy, hpactor::mailbox::OverflowPolicy::RejectNewest);
    EXPECT_FALSE(smb.priority_aware);
}

// =============================================================================
// Test 8: Parser registration via static macro
// =============================================================================

namespace {
HPACTOR_REGISTER_ACTOR("ConfigFinalRegTest", FinalConfigTestActor);
}

TEST(ParserRegistrationFinalTest, StaticRegistrationIsFound) {
    auto& reg = ActorFactoryRegistry::instance();
    EXPECT_TRUE(reg.has("ConfigFinalRegTest"));
    auto factory = reg.get_factory("ConfigFinalRegTest");
    EXPECT_NE(factory, nullptr);
}
