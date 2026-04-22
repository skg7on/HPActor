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
#include <hpactor/core/actor_system_ids.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <thread>
#include <cassert>

using namespace hpactor;

void test_async_actor_default_constructor() {
    AsyncActor handle;
    assert(!handle.ready());
    assert(handle.node_id() == "");
}

void test_async_actor_constructor() {
    AsyncActor handle("node42:12345", std::chrono::milliseconds{1000});
    assert(handle.node_id() == "node42:12345");
    assert(!handle.ready());
}

void test_async_actor_get_timeout() {
    AsyncActor handle("node1:12345", std::chrono::milliseconds{50});
    auto result = handle.get();
    assert(!result.has_value());  // should timeout
    assert(result.error().code() == errors::timeout);
}

void test_async_actor_response_set() {
    AsyncActor handle("node1:12345", std::chrono::milliseconds{100});

    // Simulate response received
    SpawnResponse resp;
    resp.actor_addr = ActorAddress{"node1:12345", ActorType{100}, ActorId{1}, 0};
    resp.error_code = spawn_errors::success;
    handle.set_response(resp);

    assert(handle.ready());
    auto result = handle.get();
    assert(result.has_value());
    assert(result.value().node_id() == "node1:12345");
}

void test_async_actor_cancel() {
    AsyncActor handle("node1:12345", std::chrono::milliseconds{1000});
    handle.cancel();
    assert(handle.ready());  // cancelled appears as ready
    auto result = handle.get();
    assert(!result.has_value());
}

int main() {
    test_async_actor_default_constructor();
    test_async_actor_constructor();
    test_async_actor_get_timeout();
    test_async_actor_response_set();
    test_async_actor_cancel();
    return 0;
}