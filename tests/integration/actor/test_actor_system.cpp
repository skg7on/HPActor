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

#include <gtest/gtest.h>
#include <type_traits>

TEST(ActorSystemTest, DefaultConstructNotCopyable) {
    static_assert(!std::is_copy_constructible_v<hpactor::ActorSystem>);
}

TEST(ActorSystemTest, ResolveActorReturnsRegisteredNamedActor) {
    hpactor::Config config;
    hpactor::ActorSystem system{config};

    auto actor = system.spawn<hpactor::EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(actor));
    system.register_actor("named-worker", actor);

    auto resolved = system.resolve_actor("named-worker");
    EXPECT_TRUE(static_cast<bool>(resolved));
    EXPECT_EQ(resolved.id(), actor.id());
    EXPECT_EQ(resolved.address(), actor.address());
}

TEST(ActorSystemTest, UnregisterActorRemovesNameFromResolution) {
    hpactor::Config config;
    config.scheduler_threads = 0;
    hpactor::ActorSystem system{config};

    auto actor = system.spawn<hpactor::EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(actor));
    system.register_actor("ephemeral-worker", actor);
    ASSERT_TRUE(static_cast<bool>(system.resolve_actor("ephemeral-worker")));

    system.unregister_actor("ephemeral-worker");

    EXPECT_FALSE(static_cast<bool>(system.resolve_actor("ephemeral-worker")));
    EXPECT_EQ(system.registry().get("ephemeral-worker").id, hpactor::ActorId{0});
}

TEST(ActorSystemTest, DuplicateNameKeepsFirstActor) {
    hpactor::Config config;
    config.scheduler_threads = 0;
    hpactor::ActorSystem system{config};

    auto first = system.spawn<hpactor::EventBasedActor>();
    auto second = system.spawn<hpactor::EventBasedActor>();
    system.register_actor("stable-name", first);
    system.register_actor("stable-name", second);

    auto resolved = system.resolve_actor("stable-name");
    ASSERT_TRUE(static_cast<bool>(resolved));
    EXPECT_EQ(resolved.id(), first.id());
    EXPECT_EQ(system.registry().get("stable-name").id, first.id());
}
