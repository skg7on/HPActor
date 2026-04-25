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
#include <hpactor/spawn.hpp>
#include <hpactor/types/serialization.hpp>
#include <hpactor/types/types.hpp>

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

// Test that SpawnRequest supports supervisor_addr field
void test_spawn_request_with_supervisor() {
    hpactor::ActorAddress supervisor{hpactor::endpoint_ops::parse_endpoint("nod"
                                                                           "e1:"
                                                                           "123"
                                                                           "4"
                                                                           "5"),
                                     hpactor::ActorType{10},
                                     hpactor::ActorId{42}, 1};

    hpactor::SpawnRequest req;
    req.actor_type_name = "worker";
    req.args_type = hpactor::TypeTag::User;
    req.serialized_args = {1, 2, 3};
    req.supervisor_addr = supervisor;

    assert(req.actor_type_name == "worker");
    assert(req.supervisor_addr.endpoint ==
           hpactor::endpoint_ops::parse_endpoint("node1:12345"));
    assert(req.supervisor_addr.id.value() == 42);
}

// Test spawn encode/decode via SpawnMessageVariant
void test_spawn_encode_via_spawn_variant() {
    hpactor::DefaultSerializer serializer;

    hpactor::SpawnRequest req;
    req.actor_type_name = "worker";
    req.args_type = hpactor::TypeTag::User;
    req.serialized_args = {1, 2, 3};
    req.supervisor_addr =
        hpactor::ActorAddress{hpactor::endpoint_ops::parse_endpoint("node1:"
                                                                    "12345"),
                              hpactor::ActorType{10}, hpactor::ActorId{42}, 1};

    hpactor::SpawnMessageVariant mv = req;
    hpactor::bytes encoded =
        serializer.encode_spawn(hpactor::TypeTag::SpawnRequestTag, mv);

    // Decode back
    hpactor::SpawnMessageVariant decoded =
        serializer.decode_spawn(hpactor::TypeTag::SpawnRequestTag, encoded);

    assert(std::holds_alternative<hpactor::SpawnRequest>(decoded));
    auto& decoded_req = std::get<hpactor::SpawnRequest>(decoded);
    assert(decoded_req.actor_type_name == "worker");
    assert(decoded_req.supervisor_addr.endpoint ==
           hpactor::endpoint_ops::parse_endpoint("node1:12345"));
    assert(decoded_req.supervisor_addr.id.value() == 42);
}

// Test SpawnResponse encode/decode via SpawnMessageVariant
void test_spawn_response_encode_via_spawn_variant() {
    hpactor::DefaultSerializer serializer;

    hpactor::SpawnResponse resp;
    resp.actor_addr =
        hpactor::ActorAddress{hpactor::endpoint_ops::parse_endpoint("node2:"
                                                                    "12345"),
                              hpactor::ActorType{20}, hpactor::ActorId{100}, 1};
    resp.error_code = hpactor::spawn_errors::success;

    hpactor::SpawnMessageVariant mv = resp;
    hpactor::bytes encoded =
        serializer.encode_spawn(hpactor::TypeTag::SpawnResponseTag, mv);

    // Decode back
    hpactor::SpawnMessageVariant decoded =
        serializer.decode_spawn(hpactor::TypeTag::SpawnResponseTag, encoded);

    assert(std::holds_alternative<hpactor::SpawnResponse>(decoded));
    auto& decoded_resp = std::get<hpactor::SpawnResponse>(decoded);
    assert(decoded_resp.actor_addr.endpoint ==
           hpactor::endpoint_ops::parse_endpoint("node2:12345"));
    assert(decoded_resp.actor_addr.id.value() == 100);
    assert(decoded_resp.error_code == hpactor::spawn_errors::success);
}

int main() {
    test_type_tag_enum_has_spawn_tags();
    test_spawn_request_construction();
    test_spawn_response_construction();
    test_spawn_request_with_supervisor();
    test_spawn_encode_via_spawn_variant();
    test_spawn_response_encode_via_spawn_variant();
    return 0;
}