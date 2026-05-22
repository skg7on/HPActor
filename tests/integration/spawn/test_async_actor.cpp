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

#include <thread>

#include <gtest/gtest.h>

#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/spawn.hpp>

using namespace hpactor;

TEST(AsyncActorTest, DefaultConstructor) {
    AsyncActor handle;
    EXPECT_FALSE(handle.ready());
    EXPECT_EQ(handle.endpoint(), EndPoint{LocalEndpoint});
}

TEST(AsyncActorTest, ConstructorWithEndpoint) {
    AsyncActor handle(endpoint_ops::parse_endpoint("node42:12345"),
                      std::chrono::milliseconds{1000});
    EXPECT_EQ(handle.endpoint(), endpoint_ops::parse_endpoint("node42:12345"));
    EXPECT_FALSE(handle.ready());
}

TEST(AsyncActorTest, GetTimeout) {
    AsyncActor handle(endpoint_ops::parse_endpoint("node1:12345"),
                      std::chrono::milliseconds{50});
    auto result = handle.get();
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), errors::timeout);
}

TEST(AsyncActorTest, ResponseSet) {
    AsyncActor handle(endpoint_ops::parse_endpoint("node1:12345"),
                      std::chrono::milliseconds{100});

    SpawnResponse resp;
    resp.actor_addr = ActorAddress{endpoint_ops::parse_endpoint("node1:12345"),
                                   ActorType{100}, ActorId{1}, 0};
    resp.error_code = spawn_errors::success;
    handle.set_response(resp);

    EXPECT_TRUE(handle.ready());
    auto result = handle.get();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().endpoint(), endpoint_ops::parse_endpoint("node1:"
                                                                      "12345"));
}

TEST(AsyncActorTest, Cancel) {
    AsyncActor handle(endpoint_ops::parse_endpoint("node1:12345"),
                      std::chrono::milliseconds{1000});
    handle.cancel();
    EXPECT_TRUE(handle.ready());
    auto result = handle.get();
    EXPECT_FALSE(result.has_value());
}
