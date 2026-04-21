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

#include <hpactor/spawn.hpp>
#include <hpactor/types/types.hpp>
#include <cassert>

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

int main() {
    test_type_tag_enum_has_spawn_tags();
    test_spawn_request_construction();
    test_spawn_response_construction();
    return 0;
}