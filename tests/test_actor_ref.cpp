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
#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>

void test_actor_default() {
    hpactor::Actor actor;
    assert(!actor);
    assert(actor.id().value() == 0);
}

void test_actor_conversion_to_address() {
    hpactor::Actor actor;
    hpactor::ActorAddress addr = actor.address();
    assert(!addr);
}

int main() {
    test_actor_default();
    test_actor_conversion_to_address();

    return 0;
}
