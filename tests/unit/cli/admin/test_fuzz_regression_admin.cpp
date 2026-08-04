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

/// \file test_fuzz_regression_admin.cpp
/// \brief Regression tests for Admin API fuzz findings.

#include <gtest/gtest.h>
#include <hpactor/cli/admin/admin_api_actor.hpp>
#include <hpactor/cli/admin/admin_messages.hpp>

using namespace hpactor;
using namespace hpactor::cli::admin;

TEST(FuzzRegressionAdmin, NullSystemHealth) {
    AdminApiActor admin(nullptr);
    auto resp = admin.health_check();
    EXPECT_EQ(resp.status_code, 500);
    EXPECT_NE(resp.body.find("no_system"), std::string::npos);
}

TEST(FuzzRegressionAdmin, NullSystemActors) {
    AdminApiActor admin(nullptr);
    auto resp = admin.list_actors();
    EXPECT_EQ(resp.status_code, 500);
}

TEST(FuzzRegressionAdmin, NullSystemClusterNodes) {
    AdminApiActor admin(nullptr);
    auto resp = admin.list_cluster_nodes();
    // nullptr system → 500 error (same as other handlers)
    EXPECT_EQ(resp.status_code, 500);
}

TEST(FuzzRegressionAdmin, NullSystemShutdown) {
    AdminApiActor admin(nullptr);
    auto resp = admin.do_shutdown();
    EXPECT_EQ(resp.status_code, 500);
}

TEST(FuzzRegressionAdmin, HandleHealth) {
    AdminApiActor admin(nullptr);
    AdminRequest req{AdminResource::Health, ""};
    auto resp = admin.handle(req);
    EXPECT_EQ(resp.status_code, 500);
}

TEST(FuzzRegressionAdmin, HandleActors) {
    AdminApiActor admin(nullptr);
    AdminRequest req{AdminResource::Actors, ""};
    auto resp = admin.handle(req);
    EXPECT_EQ(resp.status_code, 500);
}

TEST(FuzzRegressionAdmin, HandleDefaultResource) {
    // Out-of-range resource — must hit default case, not crash
    AdminApiActor admin(nullptr);
    AdminRequest req{static_cast<AdminResource>(255), ""};
    auto resp = admin.handle(req);
    EXPECT_EQ(resp.status_code, 404);
    EXPECT_NE(resp.body.find("not_found"), std::string::npos);
}

TEST(FuzzRegressionAdmin, CustomHandlerDispatch) {
    AdminApiActor admin(nullptr);
    admin.register_handler(
        AdminResource::Health,
        [](const AdminRequest& r) -> AdminResponse { return {200, r.body}; });
    AdminRequest req{AdminResource::Health, "custom_body"};
    auto resp = admin.handle(req);
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.body, "custom_body");
}

TEST(FuzzRegressionAdmin, BinaryBodyInRequest) {
    AdminApiActor admin(nullptr);
    std::string binary_body;
    for (int i = 0; i < 256; ++i)
        binary_body.push_back(static_cast<char>(i));
    AdminRequest req{AdminResource::Shutdown, binary_body};
    auto resp = admin.handle(req);
    EXPECT_EQ(resp.status_code, 500);
}

TEST(FuzzRegressionAdmin, LargeBodyInRequest) {
    AdminApiActor admin(nullptr);
    std::string large_body(100000, 'x');
    AdminRequest req{AdminResource::Actors, large_body};
    auto resp = admin.handle(req);
    EXPECT_EQ(resp.status_code, 500);
}
