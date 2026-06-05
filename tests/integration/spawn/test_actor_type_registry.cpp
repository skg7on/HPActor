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

class ArgsEchoActor final : public EventBasedActor {
  public:
    ArgsEchoActor(ActorContext* ctx, ActorSystem& sys, std::string value)
        : EventBasedActor(ctx, sys), value_(std::move(value)) {}

    const std::string& value() const noexcept {
        return value_;
    }

  private:
    std::string value_;
};

StreamBuffer make_string_args(std::string_view value) {
    return StreamBuffer{value.begin(), value.end()};
}

std::string read_string_args(const StreamBuffer& args) {
    return std::string(reinterpret_cast<const char*>(args.data()), args.size());
}

TEST(ActorTypeRegistryTest, SpawnPassesArgsToFactory) {
    ActorTypeRegistry registry;
    registry.register_factory(
        "ArgsEchoActor",
        [](ActorSystem& system, const StreamBuffer& args, TypeTag args_type) -> Actor {
            EXPECT_EQ(args_type, TypeTag::User);
            return system.spawn<ArgsEchoActor>(read_string_args(args));
        });

    Config config;
    ActorSystem system{config};
    auto actor_result =
        registry.spawn(system, "ArgsEchoActor",
                       make_string_args("remote-payload"), TypeTag::User);

    ASSERT_TRUE(actor_result.has_value());
    auto spawned = system.get_actor(actor_result.value().id);
    ASSERT_NE(spawned, nullptr);
    auto* echo = static_cast<ArgsEchoActor*>(spawned.get());
    EXPECT_EQ(echo->value(), "remote-payload");
}

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
