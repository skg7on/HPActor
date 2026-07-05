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

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <string>

using namespace hpactor;
using namespace hpactor::config;

// ---------------------------------------------------------------------------
// A minimal actor for integration testing
// ---------------------------------------------------------------------------
class BootstrapTestActor : public EventBasedActor {
  public:
    BootstrapTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

HPACTOR_REGISTER_ACTOR("BootstrapTestActor", BootstrapTestActor)

// ---------------------------------------------------------------------------
// Helper: write inline TOML to a temp file
// ---------------------------------------------------------------------------
static std::string write_temp(const std::string& content, const std::string& name) {
    std::string path = "/tmp/hpactor_bs_test_" + name + ".toml";
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

// ---------------------------------------------------------------------------
// Test 1: Single actor -- spawn from TOML, verify registration
// ---------------------------------------------------------------------------
TEST(BootstrapEngineTest, SingleActor) {
    std::string toml = R"(
[system]
version = "1.0"

[[actor]]
id = "my_actor"
behavior = "BootstrapTestActor"
)";
    std::string path = write_temp(toml, "single");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    ASSERT_TRUE(result.has_value());

    auto addr = system.registry().get("my_actor");
    EXPECT_NE(addr.id.value(), 0u);

    auto resolved = system.resolve_actor("my_actor");
    ASSERT_TRUE(static_cast<bool>(resolved));
    EXPECT_EQ(resolved.address(), addr);
}

// ---------------------------------------------------------------------------
// Test 2: Parent-child supervisor ordering
// ---------------------------------------------------------------------------
TEST(BootstrapEngineTest, SupervisorOrdering) {
    std::string toml = R"(
[system]
version = "1.0"

[[actor]]
id = "parent"
behavior = "BootstrapTestActor"

[[actor]]
id = "child"
behavior = "BootstrapTestActor"
supervisor = "parent"
)";
    std::string path = write_temp(toml, "supervisor");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    ASSERT_TRUE(result.has_value());

    // Both actors should be registered
    auto parent_addr = system.registry().get("parent");
    auto child_addr = system.registry().get("child");
    EXPECT_NE(parent_addr.id.value(), 0u);
    EXPECT_NE(child_addr.id.value(), 0u);
}

// ---------------------------------------------------------------------------
// Test 3: SystemInit delivery
// ---------------------------------------------------------------------------
TEST(BootstrapEngineTest, SystemInitDelivery) {
    std::string toml = R"(
[system]
version = "1.0"

[[actor]]
id = "init_test"
behavior = "BootstrapTestActor"
)";
    std::string path = write_temp(toml, "sysinit");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    ASSERT_TRUE(result.has_value());

    // The actor is spawned and SystemInit is delivered via mailbox.
    // For now, verify the actor exists (SystemInit delivery requires
    // the scheduler to process messages, which is tested separately).
    auto addr = system.registry().get("init_test");
    EXPECT_NE(addr.id.value(), 0u);
}

// ---------------------------------------------------------------------------
// Test 4: Multiple independent roots
// ---------------------------------------------------------------------------
TEST(BootstrapEngineTest, MultipleRoots) {
    std::string toml = R"(
[system]
version = "1.0"

[[actor]]
id = "root_a"
behavior = "BootstrapTestActor"

[[actor]]
id = "root_b"
behavior = "BootstrapTestActor"

[[actor]]
id = "root_c"
behavior = "BootstrapTestActor"
)";
    std::string path = write_temp(toml, "multiroot");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    ASSERT_TRUE(result.has_value());

    for (const char* id : {"root_a", "root_b", "root_c"}) {
        auto addr = system.registry().get(id);
        EXPECT_NE(addr.id.value(), 0u);
    }
}

// ---------------------------------------------------------------------------
// Test 5: Unknown behavior -> error
// ---------------------------------------------------------------------------
TEST(BootstrapEngineTest, UnknownBehavior) {
    std::string toml = R"(
[system]
version = "1.0"

[[actor]]
id = "bad"
behavior = "NonexistentActor"
)";
    std::string path = write_temp(toml, "unknown_beh");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// Test 6: Dispatcher assignment via config
// ---------------------------------------------------------------------------
TEST(BootstrapEngineTest, DispatcherAssignment) {
    std::string toml = R"(
[system]
version = "1.0"

[[dispatcher]]
name = "test_pool"
threads = 2

[[actor]]
id = "pooled_actor"
behavior = "BootstrapTestActor"
dispatcher = "test_pool"
dispatch_policy = "Cooperative"
)";
    std::string path = write_temp(toml, "dispatcher");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    ASSERT_TRUE(result.has_value());

    auto addr = system.registry().get("pooled_actor");
    EXPECT_NE(addr.id.value(), 0u);
}

// ---------------------------------------------------------------------------
// Test 7: Template-based actor with args
// ---------------------------------------------------------------------------
TEST(BootstrapEngineTest, TemplateWithArgs) {
    std::string toml = R"(
[system]
version = "1.0"

[template.base]
behavior = "BootstrapTestActor"
mailbox_capacity = 8192

[[actor]]
id = "templated"
inherits = "base"
)";
    std::string path = write_temp(toml, "templ_args");

    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto result = system.load_topology(path);
    ASSERT_TRUE(result.has_value());

    auto addr = system.registry().get("templated");
    EXPECT_NE(addr.id.value(), 0u);
}

TEST(BootstrapEngineTest, DeadLetterReconfigurePreservesQueueIdentity) {
    Config config;
    config.scheduler_threads = 0;
    config.dead_letters.capacity = 8;
    ActorSystem system(config);
    auto* original = system.dead_letter_queue();
    ASSERT_NE(original, nullptr);

    const std::string toml = R"(
[system]
version = "1.0"

[system.dead_letters]
enabled = true
capacity = 2
max_payload_sample_bytes = 16
overflow_policy = "drop_oldest"
store_payload = true
)";
    auto result = system.load_topology(write_temp(toml, "dlq_identity"));
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(system.dead_letter_queue(), original);

    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    auto delivery = system.try_deliver_local(ActorId{999999}, std::move(msg));
    EXPECT_EQ(delivery.code, mailbox::EnqueueResultCode::ActorNotFound);
    auto snapshot = system.dead_letter_snapshot();
    EXPECT_EQ(snapshot.capacity, 2u);
    EXPECT_EQ(snapshot.depth, 1u);
}

TEST(BootstrapEngineTest, ConfiguredSpawnMatchesTemplateLifecycle) {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto direct = system.spawn<BootstrapTestActor>();
    auto configured_instance =
        std::make_shared<BootstrapTestActor>(nullptr, system);
    ActorDef def;
    def.behavior = "BootstrapTestActor";
    auto configured = system.spawn_configured(std::move(configured_instance), def);

    ASSERT_TRUE(static_cast<bool>(direct));
    ASSERT_TRUE(static_cast<bool>(configured));
    // Both spawn paths produce actors with working mailboxes.
    EXPECT_NE(system.get_mailbox(direct.id()), nullptr);
    EXPECT_NE(system.get_mailbox(configured.id()), nullptr);
    // Lifecycle state parity: both actors are in the same lifecycle state.
    EXPECT_EQ(direct.get()->as_lifecycle() != nullptr,
              configured.get()->as_lifecycle() != nullptr);
    if (direct.get()->as_lifecycle()) {
        EXPECT_EQ(direct.get()->as_lifecycle()->state(), LifecycleState::kActive);
        EXPECT_EQ(configured.get()->as_lifecycle()->state(),
                  LifecycleState::kActive);
    }
}
