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

#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/spawn.hpp>

using namespace hpactor;

// AsyncActor is now a type alias for RequestHandle<ActorRef>.
// These tests verify the fundamental handle operations: default
// construction, ready(), get(), resolve(), and cancel().

TEST(AsyncActorTest, DefaultConstructor) {
    AsyncActor handle;
    EXPECT_FALSE(handle.ready());
}

TEST(AsyncActorTest, ReadyAfterResolve) {
    AsyncActor handle;

    // Create a valid ActorRef and resolve
    auto addr = ActorAddress{endpoint_ops::parse_endpoint("node42:12345"),
                             ActorType{100}, ActorId{1}, 0};
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    ActorRef ref(std::move(proxy));
    handle.resolve(result<ActorRef>::make(std::move(ref)));

    EXPECT_TRUE(handle.ready());
    auto r = handle.get();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value().endpoint(), endpoint_ops::parse_endpoint("node42:"
                                                                 "12345"));
}

TEST(AsyncActorTest, ResolveError) {
    AsyncActor handle;
    handle.resolve_error(error(spawn_errors::timeout, "spawn timed out"));

    EXPECT_TRUE(handle.ready());
    auto r = handle.get();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), spawn_errors::timeout);
}

TEST(AsyncActorTest, Cancel) {
    AsyncActor handle;
    handle.cancel();
    EXPECT_TRUE(handle.ready());
    auto r = handle.get();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), errors::cancelled);
}

TEST(AsyncActorTest, ResolveThenCancelIsNoop) {
    AsyncActor handle;
    handle.resolve_error(error(42, "done"));
    EXPECT_TRUE(handle.ready());

    // Cancel after resolve is a no-op — result already set
    handle.cancel();
    auto r = handle.get();
    EXPECT_EQ(r.error().code(), 42);
}
