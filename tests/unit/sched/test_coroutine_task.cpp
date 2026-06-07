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

// tests/unit/sched/test_coroutine_task.cpp
#include <gtest/gtest.h>
#include <hpactor/coroutine/coroutine_task.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/types/types.hpp>

#if HPACTOR_SUPPORT_COROUTINES

#    include <coroutine>

// Simple test coroutine that suspends and resumes
hpactor::sched::CoroutineTask simple_coro() {
    co_return;
}

TEST(CoroutineTaskTest, DefaultConstruction) {
    hpactor::sched::CoroutineTask t1;
    EXPECT_FALSE(t1);
    EXPECT_TRUE(t1.done());
}

TEST(CoroutineTaskTest, MoveConstruction) {
    auto t2 = simple_coro();
    hpactor::sched::CoroutineTask t3(std::move(t2));
    EXPECT_FALSE(t2); // NOLINT(bugprone-use-after-move) — testing moved-from
                      // state
    EXPECT_TRUE(t3);
    EXPECT_FALSE(t3.done()); // not done until resumed and finishes
}

TEST(CoroutineTaskTest, MoveAssignment) {
    hpactor::sched::CoroutineTask t4;
    t4 = simple_coro();
    hpactor::sched::CoroutineTask t5;
    t5 = std::move(t4);
    EXPECT_FALSE(t4); // NOLINT(bugprone-use-after-move) — testing moved-from
                      // state
    EXPECT_TRUE(t5);
}

TEST(CoroutineTaskTest, Resume) {
    hpactor::sched::CoroutineTask t6 = simple_coro();
    EXPECT_FALSE(t6.done());
    t6.resume();
    EXPECT_TRUE(t6.done()); // co_return makes it done
}

TEST(CoroutineTaskTest, DestroyWithoutResuming) {
    {
        hpactor::sched::CoroutineTask t7 = simple_coro();
        (void)t7;
        // t7 destroyed without resume — coroutine frame is destroyed
    }
    SUCCEED();
}

#else // !HPACTOR_SUPPORT_COROUTINES

// C++17 fallback: test the stub CoroutineTask
TEST(CoroutineTaskStubTest, DefaultConstruction) {
    hpactor::sched::CoroutineTask t1;
    EXPECT_FALSE(t1);
    EXPECT_TRUE(t1.done());
}

TEST(CoroutineTaskStubTest, MoveConstruction) {
    hpactor::sched::CoroutineTask t2;
    hpactor::sched::CoroutineTask t3(std::move(t2));
    EXPECT_FALSE(t3);
}

TEST(CoroutineTaskStubTest, MoveAssignment) {
    hpactor::sched::CoroutineTask t4;
    hpactor::sched::CoroutineTask t5;
    t5 = std::move(t4);
    EXPECT_FALSE(t5);
}

TEST(CoroutineTaskStubTest, ResumeAndDone) {
    hpactor::sched::CoroutineTask t6;
    t6.resume();
    EXPECT_TRUE(t6.done());
}

#endif // HPACTOR_SUPPORT_COROUTINES
