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

// tests/sched/test_edf_queue.cpp
#include <cassert>
#include <hpactor/sched/edf_queue.hpp>
#include <hpactor/types/types.hpp>

int main() {
    hpactor::sched::EDFQueue q;

    // Test: empty queue
    assert(q.empty());
    assert(q.size() == 0);

    hpactor::sched::WorkItem item;
    item.actor = hpactor::ActorId{0};
    item.deadline_ns = 0;
    item.sequence = 0;

    // Test: pop on empty returns false
    assert(!q.pop(item));

    // Test: push/pop round-trip
    hpactor::sched::WorkItem in, out;
    in.actor = hpactor::ActorId{42};
    in.deadline_ns = 1000;
    in.sequence = 1;

    q.push(1000, in);
    assert(!q.empty());
    assert(q.size() == 1);

    assert(q.pop(out));
    assert(out.actor.value() == 42);
    assert(out.deadline_ns == 1000);
    assert(q.empty());

    // Test: EDF ordering - earlier deadline first
    hpactor::sched::WorkItem early, late;
    early.actor = hpactor::ActorId{1};
    early.deadline_ns = 100;
    early.sequence = 1;

    late.actor = hpactor::ActorId{2};
    late.deadline_ns = 200;
    late.sequence = 2;

    q.push(200, late);  // later deadline
    q.push(100, early); // earlier deadline

    assert(q.pop(out));
    assert(out.actor.value() == 1); // early deadline first

    assert(q.pop(out));
    assert(out.actor.value() == 2); // late deadline second

    // Test: FIFO ordering for same deadline
    hpactor::sched::WorkItem first, second;
    first.actor = hpactor::ActorId{10};
    first.deadline_ns = 500;
    first.sequence = 1;

    second.actor = hpactor::ActorId{20};
    second.deadline_ns = 500;
    second.sequence = 2;

    q.push(500, second);
    q.push(500, first);

    assert(q.pop(out));
    assert(out.actor.value() == 10); // first by sequence

    assert(q.pop(out));
    assert(out.actor.value() == 20); // second by sequence

    // Test: peek
    q.clear();
    int64_t deadline_out;
    assert(!q.peek(deadline_out));

    q.push(300, hpactor::sched::WorkItem{hpactor::ActorId{5}, 300, 1});
    assert(q.peek(deadline_out));
    assert(deadline_out == 300);

    return 0;
}