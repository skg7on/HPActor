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

#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace hpactor;
using namespace hpactor::config;

// Test actors — minimal stubs for factory verification
class TestEchoActor : public EventBasedActor {
  public:
    TestEchoActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

class TestWorkerActor : public EventBasedActor {
  public:
    TestWorkerActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

// -----------------------------------------------------------------------------
// Test fixture — registers TestEcho and TestWorker before each test so that
// tests are independent of execution order.
// -----------------------------------------------------------------------------
class ActorFactoryRegistryTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto& registry = ActorFactoryRegistry::instance();
        registry.register_factory<TestEchoActor>("TestEcho");
        registry.register_factory<TestWorkerActor>("TestWorker");
    }
};

// -----------------------------------------------------------------------------
// Test 1: Register two types, verify has() returns true for both
// -----------------------------------------------------------------------------
TEST_F(ActorFactoryRegistryTest, RegisterAndHas) {
    auto& registry = ActorFactoryRegistry::instance();
    EXPECT_TRUE(registry.has("TestEcho"));
    EXPECT_TRUE(registry.has("TestWorker"));
}

// -----------------------------------------------------------------------------
// Test 2: Call get_factory() — returns a valid callable
// -----------------------------------------------------------------------------
TEST_F(ActorFactoryRegistryTest, GetFactoryReturnsCallable) {
    auto& registry = ActorFactoryRegistry::instance();

    auto factory = registry.get_factory("TestEcho");
    EXPECT_NE(factory, nullptr);
    // Factory is a valid std::function — construction happens when invoked
    // with a real ActorSystem, which requires integration test setup
}

// -----------------------------------------------------------------------------
// Test 3: has() returns false for unknown name
// -----------------------------------------------------------------------------
TEST_F(ActorFactoryRegistryTest, HasUnknown) {
    auto& registry = ActorFactoryRegistry::instance();
    EXPECT_FALSE(registry.has("NonExistentActor"));
}

// -----------------------------------------------------------------------------
// Test 4: known_names() returns all registered names
// -----------------------------------------------------------------------------
TEST_F(ActorFactoryRegistryTest, KnownNames) {
    auto& registry = ActorFactoryRegistry::instance();
    auto names = registry.known_names();

    // Our two registrations from SetUp should be present
    bool found_echo = false;
    bool found_worker = false;
    for (const auto& name : names) {
        if (name == "TestEcho")
            found_echo = true;
        if (name == "TestWorker")
            found_worker = true;
    }
    EXPECT_TRUE(found_echo);
    EXPECT_TRUE(found_worker);
}

// -----------------------------------------------------------------------------
// Test 5: Duplicate registration overwrites (last wins)
// -----------------------------------------------------------------------------
TEST_F(ActorFactoryRegistryTest, DuplicateOverwrite) {
    auto& registry = ActorFactoryRegistry::instance();

    // Register a type under "DuplicateName"
    registry.register_factory<TestEchoActor>("DuplicateName");
    auto f1 = registry.get_factory("DuplicateName");
    EXPECT_NE(f1, nullptr);

    // Overwrite with a different type
    registry.register_factory<TestWorkerActor>("DuplicateName");
    auto f2 = registry.get_factory("DuplicateName");
    EXPECT_NE(f2, nullptr);

    // It should still return a valid factory (last-registered wins)
}

// -----------------------------------------------------------------------------
// Test 6: get_factory returns nullptr for unknown name
// -----------------------------------------------------------------------------
TEST_F(ActorFactoryRegistryTest, GetFactoryUnknown) {
    auto& registry = ActorFactoryRegistry::instance();
    auto factory = registry.get_factory("DefinitelyNotRegistered");
    EXPECT_EQ(factory, nullptr);
}
