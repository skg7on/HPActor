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

// tests/sched/test_actor_state.cpp
#include <cassert>
#include <hpactor/actor/actor_state.hpp>

int main() {
    hpactor::ActorState state;

    // Test 1: initial state is Idle
    assert(state.get() == hpactor::ActorState::kIdle);
    assert(state.is_idle());

    // Test 2: valid transitions
    assert(state.cas(hpactor::ActorState::kIdle, hpactor::ActorState::kReady));
    assert(state.is_ready());

    assert(state.cas(hpactor::ActorState::kReady, hpactor::ActorState::kRunning));
    assert(state.is_running());

    assert(state.cas(hpactor::ActorState::kRunning, hpactor::ActorState::kIdle));
    assert(state.is_idle());

    // Test 3: invalid transition (no change)
    bool ok = state.cas(hpactor::ActorState::kRunning, hpactor::ActorState::kIdle);
    assert(!ok);  // was Idle, not Running
    assert(state.is_idle());

    // Test 4: set overrides regardless of current state
    state.set(hpactor::ActorState::kTerminated);
    assert(state.is_terminated());

    // Test 5: IOWaiting
    state.set(hpactor::ActorState::kIOWaiting);
    assert(state.is_io_waiting());
    assert(!state.is_running());
    assert(!state.is_idle());

    return 0;
}