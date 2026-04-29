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

// tests/sched/test_coroutine_task.cpp
#include <cassert>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/sched/coroutine_task.hpp>
#include <hpactor/types/types.hpp>

#if HPACTOR_SUPPORT_COROUTINES

#    include <coroutine>

// Simple test coroutine that suspends and resumes
hpactor::sched::CoroutineTask simple_coro() {
    co_return;
}

int main() {
    // Test 1: default construction
    hpactor::sched::CoroutineTask t1;
    assert(!t1);
    assert(t1.done());

    // Test 2: move construction
    auto t2 = simple_coro();
    hpactor::sched::CoroutineTask t3(std::move(t2));
    assert(!t2); // moved-from is nullptr
    assert(t3);
    assert(!t3.done()); // not done until resumed and finishes

    // Test 3: move assignment
    hpactor::sched::CoroutineTask t4;
    t4 = simple_coro();
    hpactor::sched::CoroutineTask t5;
    t5 = std::move(t4);
    assert(!t4);
    assert(t5);

    // Test 4: resume
    hpactor::sched::CoroutineTask t6 = simple_coro();
    assert(!t6.done());
    t6.resume();
    assert(t6.done()); // co_return makes it done

    // Test 5: destroy without resuming
    {
        hpactor::sched::CoroutineTask t7 = simple_coro();
        (void)t7;
        // t7 destroyed without resume — coroutine frame is destroyed
    }

    return 0;
}

#else // !HPACTOR_SUPPORT_COROUTINES

// C++17 fallback: test the stub CoroutineTask
int main() {
    // Test 1: default construction
    hpactor::sched::CoroutineTask t1;
    assert(!t1);
    assert(t1.done());

    // Test 2: move construction (stub is no-op)
    hpactor::sched::CoroutineTask t2;
    hpactor::sched::CoroutineTask t3(std::move(t2));
    assert(!t3);

    // Test 3: move assignment (stub is no-op)
    hpactor::sched::CoroutineTask t4;
    hpactor::sched::CoroutineTask t5;
    t5 = std::move(t4);
    assert(!t5);

    // Test 4: resume and done (stub is no-op)
    hpactor::sched::CoroutineTask t6;
    t6.resume();
    assert(t6.done());

    return 0;
}

#endif // HPACTOR_SUPPORT_COROUTINES
