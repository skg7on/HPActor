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

/// \brief Architecture fitness: validates that ActorSystem remains an opaque
///        facade whose public header does not require private implementation
///        headers.

#include <hpactor/actor/system/actor_system.hpp>

#include <gtest/gtest.h>

#include <type_traits>

// Verify non-copyable / non-movable
static_assert(!std::is_copy_constructible_v<hpactor::ActorSystem>);
static_assert(!std::is_copy_assignable_v<hpactor::ActorSystem>);
static_assert(!std::is_move_constructible_v<hpactor::ActorSystem>);
static_assert(!std::is_move_assignable_v<hpactor::ActorSystem>);

// Only enabled when the final PImpl check is on.
#if HPACTOR_ACTOR_SYSTEM_FINAL_PIMPL_CHECK
static_assert(sizeof(hpactor::ActorSystem) <= 2 * sizeof(void*),
              "ActorSystem must be an opaque runtime facade");
#endif

TEST(ActorSystemPublicHeader, CompilesWithoutPrivateHeaders) {
    // Construction with minimal config compiles.
    hpactor::Config config;
    config.scheduler_threads = 0;
    config.enable_network = false;
    {
        hpactor::ActorSystem system{config};
        EXPECT_TRUE(system.is_running());
        EXPECT_FALSE(system.is_draining());
    }
    SUCCEED();
}

TEST(ActorSystemPublicHeader, SpawnTemplateIsCallable) {
    hpactor::Config config;
    config.scheduler_threads = 0;
    config.enable_network = false;
    hpactor::ActorSystem system{config};

    auto actor = system.spawn<hpactor::EventBasedActor>();
    EXPECT_TRUE(static_cast<bool>(actor));
    EXPECT_NE(actor.get(), nullptr);
}
