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

#include <cassert>
#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/spawn.hpp>
#include <hpactor/types/types.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

// Test that TypeTag enum includes spawn protocol tags
void test_type_tag_enum_has_spawn_tags() {
    assert(static_cast<uint32_t>(hpactor::TypeTag::SpawnRequestTag) == 5);
    assert(static_cast<uint32_t>(hpactor::TypeTag::SpawnResponseTag) == 6);
}

// Test that SpawnRequest is constructible
void test_spawn_request_construction() {
    hpactor::SpawnRequest req;
    req.actor_type_name = "worker";
    req.args_type = hpactor::TypeTag::User;
    req.serialized_args = {};
    assert(req.actor_type_name == "worker");
    assert(req.args_type == hpactor::TypeTag::User);
}

// Test that SpawnResponse is constructible
void test_spawn_response_construction() {
    hpactor::SpawnResponse resp;
    resp.error_code = hpactor::spawn_errors::success;
    assert(resp.error_code == hpactor::spawn_errors::success);
}

// Test spawn encode/decode via protobuf SpawnRequestMessage
void test_spawn_request_encode_via_protobuf() {
    hpactor::ProtoTypeRegistry registry;
    registry.register_system_types();

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name("worker");
    pb_req.set_args_type(static_cast<uint32_t>(hpactor::TypeTag::User));
    pb_req.set_serialized_args("abc");
    auto* sup = pb_req.mutable_supervisor();
    sup->mutable_endpoint()->mutable_ipv4()->set_addr(0x7F000001);
    sup->mutable_endpoint()->mutable_ipv4()->set_port(8080);
    sup->set_type(10);
    sup->set_actor_id(42);
    sup->set_incarnation(1);

    hpactor::bytes encoded = registry.serialize(pb_req);
    assert(!encoded.empty());

    // Decode back
    auto decoded = registry.deserialize(hpactor::TypeTag::SpawnRequestTag, encoded);
    assert(decoded != nullptr);
    auto* decoded_req = static_cast<::hpactor::SpawnRequestMessage*>(decoded.get());
    assert(decoded_req->actor_type_name() == "worker");
    assert(decoded_req->supervisor().actor_id() == 42);
}

// Test SpawnResponse encode/decode via protobuf
void test_spawn_response_encode_via_protobuf() {
    hpactor::ProtoTypeRegistry registry;
    registry.register_system_types();

    ::hpactor::SpawnResponseMessage pb_resp;
    auto* addr = pb_resp.mutable_actor_addr();
    addr->mutable_endpoint()->mutable_ipv4()->set_addr(0x7F000002);
    addr->mutable_endpoint()->mutable_ipv4()->set_port(9090);
    addr->set_type(20);
    addr->set_actor_id(100);
    addr->set_incarnation(1);
    pb_resp.set_error_code(hpactor::spawn_errors::success);

    hpactor::bytes encoded = registry.serialize(pb_resp);
    assert(!encoded.empty());

    auto decoded = registry.deserialize(hpactor::TypeTag::SpawnResponseTag, encoded);
    assert(decoded != nullptr);
    auto* decoded_resp = static_cast<::hpactor::SpawnResponseMessage*>(decoded.get());
    assert(decoded_resp->actor_addr().actor_id() == 100);
    assert(decoded_resp->error_code() == hpactor::spawn_errors::success);
}

int main() {
    test_type_tag_enum_has_spawn_tags();
    test_spawn_request_construction();
    test_spawn_response_construction();
    test_spawn_request_encode_via_protobuf();
    test_spawn_response_encode_via_protobuf();
    return 0;
}
