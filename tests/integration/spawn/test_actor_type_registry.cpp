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

#include <gtest/gtest.h>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_type_registry.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>

using namespace hpactor;

namespace {

class TestActor : public EventBasedActor {
  public:
    TestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    Behavior make_behavior() override {
        return {};
    }
};

} // namespace

TEST(ActorTypeRegistryTest, RegisterAndLookup) {
    ActorTypeRegistry registry;
    registry.register_type<TestActor>("test-actor");

    EXPECT_TRUE(registry.has("test-actor"));
    EXPECT_FALSE(registry.has("non-existent"));
    EXPECT_NE(registry.type_id("test-actor"), ActorType{0});
}

TEST(ActorTypeRegistryTest, SpawnUnknownType) {
    ActorTypeRegistry registry;
    Config config;
    ActorSystem system{config};

    auto result =
        registry.spawn(system, "non-existent", StreamBuffer{}, TypeTag::Invalid);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), spawn_errors::unknown_type);
}

TEST(ActorTypeRegistryTest, SpawnValidType) {
    ActorTypeRegistry registry;
    Config config;
    ActorSystem system{config};

    registry.register_type<TestActor>("test-actor");
    auto result =
        registry.spawn(system, "test-actor", StreamBuffer{}, TypeTag::Invalid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().endpoint, EndPoint{LocalEndpoint});
}
