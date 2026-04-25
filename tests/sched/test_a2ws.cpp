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

// tests/sched/test_a2ws.cpp
#include <cassert>
#include <hpactor/sched/a2ws.hpp>

int main() {
    hpactor::sched::A2WS a2ws(8, 4); // 8 workers, pool size 4

    // Test: basic victim selection
    assert(a2ws.num_workers() == 8);

    uint32_t victim = a2ws.get_victim(0);
    assert(victim < 8);

    // Test: same_pool
    assert(a2ws.same_pool(0, 1));
    assert(a2ws.same_pool(0, 3));
    assert(a2ws.same_pool(4, 7));
    assert(!a2ws.same_pool(0, 4));
    assert(!a2ws.same_pool(3, 4));

    // Test: get_victim_pool
    uint32_t start, end;
    a2ws.get_victim_pool(0, start, end);
    assert(start == 0 && end == 4);

    a2ws.get_victim_pool(3, start, end);
    assert(start == 0 && end == 4);

    a2ws.get_victim_pool(4, start, end);
    assert(start == 4 && end == 8);

    a2ws.get_victim_pool(7, start, end);
    assert(start == 4 && end == 8);

    // Test: record_attempt and steal
    a2ws.record_attempt(0, 1, true);
    assert(a2ws.stats(0).steal_attempts.load() == 1);
    assert(a2ws.stats(0).steal_successes.load() == 1);

    a2ws.record_attempt(0, 2, false);
    assert(a2ws.stats(0).steal_attempts.load() == 2);
    assert(a2ws.stats(0).steal_successes.load() == 1);

    // Test: record_steal
    a2ws.record_steal(0, 1);
    assert(a2ws.stats(0).local_steals.load() == 1);

    a2ws.record_steal(0, 5); // different pool
    assert(a2ws.stats(0).local_steals.load() == 1);
    assert(a2ws.stats(0).remote_steals.load() == 1);

    return 0;
}