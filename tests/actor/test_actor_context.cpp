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

#include <hpactor/actor_context.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <cassert>
#include <type_traits>

using namespace hpactor;

void test_actor_context_children() {
    static_assert(sizeof(hpactor::ActorContext) > 0, "should not be empty");

    // ActorContext should be constructible from an Actor
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    // Children management
    assert(ctx.children().empty());

    Actor child_actor;
    ctx.add_child(child_actor);
    assert(ctx.children().size() == 1);

    ctx.remove_child(child_actor);
    assert(ctx.children().empty());
}

void test_actor_context_linked_actors() {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    // Linked actors should be empty initially
    assert(ctx.linked_actors().empty());
}

void test_actor_context_monitor() {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    ActorAddress addr{1, 2, ActorId{3}, 4};
    ctx.monitor(addr);

    auto monitored = ctx.linked_actors(); // Note: linked_actors, not monitored
    (void)monitored;
}

int main() {
    test_actor_context_children();
    test_actor_context_linked_actors();
    test_actor_context_monitor();

    return 0;
}