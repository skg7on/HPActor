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
#include <hpactor/net/admin/admin_api_actor.hpp>

namespace hpactor::net::admin {

TEST(AdminApiActorTest, DefaultConstruction) {
    AdminApiActor actor;
    // Should not crash on construction
}

TEST(AdminApiActorTest, HealthCheckReturnsOk) {
    auto resp = AdminApiActor::health_check();
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"status\":\"ok\""), std::string::npos);
}

TEST(AdminApiActorTest, ListActorsReturnsEmptyArray) {
    auto resp = AdminApiActor::list_actors();
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"actors\":[]"), std::string::npos);
}

TEST(AdminApiActorTest, ListClusterNodesReturnsEmptyArray) {
    auto resp = AdminApiActor::list_cluster_nodes();
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"nodes\":[]"), std::string::npos);
}

TEST(AdminApiActorTest, ShutdownReturnsInitiated) {
    auto resp = AdminApiActor::shutdown();
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"shutdown\":\"initiated\""), std::string::npos);
}

TEST(AdminApiActorTest, RegisterHandlerDispatchesCorrectly) {
    AdminApiActor actor;
    actor.register_handler(
        AdminResource::Actors, [](const AdminRequest&) -> AdminResponse {
            return {200, R"({"actors":[{"id":1,"name":"test"}]})"};
        });

    AdminRequest req{AdminResource::Actors, ""};
    auto resp = actor.handle(req);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"actors\":"), std::string::npos);
}

TEST(AdminApiActorTest, UnregisteredResourceReturns404) {
    AdminApiActor actor;
    // Use an out-of-range resource value
    AdminRequest req{static_cast<AdminResource>(99), ""};
    auto resp = actor.handle(req);
    EXPECT_EQ(resp.status_code, 404);
}

TEST(AdminApiActorTest, BuiltInFallbackForHealth) {
    AdminApiActor actor;
    AdminRequest req{AdminResource::Health, ""};
    auto resp = actor.handle(req);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_NE(resp.body.find("\"status\":\"ok\""), std::string::npos);
}

} // namespace hpactor::net::admin
