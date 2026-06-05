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
#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/spawn.hpp>
#include <hpactor/types/types.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

TEST(SpawnSerializationTest, TypeTagEnumHasSpawnTags) {
    EXPECT_EQ(static_cast<uint32_t>(hpactor::TypeTag::SpawnRequestTag), 0x10u);
    EXPECT_EQ(static_cast<uint32_t>(hpactor::TypeTag::SpawnResponseTag), 0x11u);
}

TEST(SpawnSerializationTest, SpawnRequestConstruction) {
    hpactor::SpawnRequest req;
    req.actor_type_name = "worker";
    req.args_type = hpactor::TypeTag::User;
    req.serialized_args = {};
    EXPECT_EQ(req.actor_type_name, "worker");
    EXPECT_EQ(req.args_type, hpactor::TypeTag::User);
}

TEST(SpawnSerializationTest, SpawnResponseConstruction) {
    hpactor::SpawnResponse resp;
    resp.error_code = hpactor::spawn_errors::success;
    EXPECT_EQ(resp.error_code, hpactor::spawn_errors::success);
}

TEST(SpawnSerializationTest, SpawnRequestEncodeViaProtobuf) {
    hpactor::ProtoTypeRegistry registry;
    registry.register_system_types();

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name("worker");
    pb_req.set_args_type(static_cast<uint32_t>(hpactor::TypeTag::User));
    pb_req.set_serialized_args("abc");
    auto* sup = pb_req.mutable_supervisor();
    auto* sup_global = sup->mutable_global_addr();
    sup_global->mutable_endpoint()->mutable_ipv4()->set_addr(0x7F000001);
    sup_global->mutable_endpoint()->mutable_ipv4()->set_port(8080);
    auto* sup_local = sup_global->mutable_local_addr();
    sup_local->set_actor_type(10);
    sup_local->set_actor_id(42);
    sup_local->set_incarnation(1);

    hpactor::StreamBuffer encoded = registry.serialize(pb_req);
    EXPECT_FALSE(encoded.empty());

    auto decoded = registry.deserialize(hpactor::TypeTag::SpawnRequestTag, encoded);
    EXPECT_NE(decoded, nullptr);
    auto* decoded_req =
        static_cast<::hpactor::SpawnRequestMessage*>(decoded.get());
    EXPECT_EQ(decoded_req->actor_type_name(), "worker");
    EXPECT_EQ(decoded_req->supervisor().global_addr().local_addr().actor_id(), 42u);
}

TEST(SpawnSerializationTest, SpawnResponseEncodeViaProtobuf) {
    hpactor::ProtoTypeRegistry registry;
    registry.register_system_types();

    ::hpactor::SpawnResponseMessage pb_resp;
    auto* addr = pb_resp.mutable_actor_addr();
    auto* addr_global = addr->mutable_global_addr();
    addr_global->mutable_endpoint()->mutable_ipv4()->set_addr(0x7F000002);
    addr_global->mutable_endpoint()->mutable_ipv4()->set_port(9090);
    auto* addr_local = addr_global->mutable_local_addr();
    addr_local->set_actor_type(20);
    addr_local->set_actor_id(100);
    addr_local->set_incarnation(1);
    pb_resp.set_error_code(hpactor::spawn_errors::success);

    hpactor::StreamBuffer encoded = registry.serialize(pb_resp);
    EXPECT_FALSE(encoded.empty());

    auto decoded =
        registry.deserialize(hpactor::TypeTag::SpawnResponseTag, encoded);
    EXPECT_NE(decoded, nullptr);
    auto* decoded_resp =
        static_cast<::hpactor::SpawnResponseMessage*>(decoded.get());
    EXPECT_EQ(decoded_resp->actor_addr().global_addr().local_addr().actor_id(),
              100u);
    EXPECT_EQ(decoded_resp->error_code(), hpactor::spawn_errors::success);
}

TEST(SpawnSerializationTest, RequestPreservesArgsBytes) {
    const std::string payload = "alpha=7";

    hpactor::ProtoTypeRegistry registry;
    registry.register_system_types();

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name("ArgsEchoActor");
    pb_req.set_args_type(static_cast<uint32_t>(hpactor::TypeTag::User));
    pb_req.set_serialized_args(payload);

    hpactor::StreamBuffer encoded = registry.serialize(pb_req);
    ASSERT_FALSE(encoded.empty());

    auto decoded = registry.deserialize(hpactor::TypeTag::SpawnRequestTag, encoded);
    ASSERT_NE(decoded, nullptr);
    auto* decoded_req =
        static_cast<::hpactor::SpawnRequestMessage*>(decoded.get());
    EXPECT_EQ(decoded_req->actor_type_name(), "ArgsEchoActor");
    EXPECT_EQ(decoded_req->args_type(),
              static_cast<uint32_t>(hpactor::TypeTag::User));
    EXPECT_EQ(decoded_req->serialized_args(), payload);
}
