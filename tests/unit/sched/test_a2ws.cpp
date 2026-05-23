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

// tests/unit/sched/test_a2ws.cpp
#include <gtest/gtest.h>
#include <hpactor/sched/a2ws.hpp>

TEST(A2WSTest, BasicVictimSelection) {
    hpactor::sched::A2WS a2ws(8, 4); // 8 workers, pool size 4
    EXPECT_EQ(a2ws.num_workers(), 8U);
    uint32_t victim = a2ws.get_victim(0);
    EXPECT_LT(victim, 8U);
}

TEST(A2WSTest, SamePool) {
    hpactor::sched::A2WS a2ws(8, 4);
    EXPECT_TRUE(a2ws.same_pool(0, 1));
    EXPECT_TRUE(a2ws.same_pool(0, 3));
    EXPECT_TRUE(a2ws.same_pool(4, 7));
    EXPECT_FALSE(a2ws.same_pool(0, 4));
    EXPECT_FALSE(a2ws.same_pool(3, 4));
}

TEST(A2WSTest, GetVictimPool) {
    hpactor::sched::A2WS a2ws(8, 4);
    uint32_t start, end;

    a2ws.get_victim_pool(0, start, end);
    EXPECT_EQ(start, 0U);
    EXPECT_EQ(end, 4U);

    a2ws.get_victim_pool(3, start, end);
    EXPECT_EQ(start, 0U);
    EXPECT_EQ(end, 4U);

    a2ws.get_victim_pool(4, start, end);
    EXPECT_EQ(start, 4U);
    EXPECT_EQ(end, 8U);

    a2ws.get_victim_pool(7, start, end);
    EXPECT_EQ(start, 4U);
    EXPECT_EQ(end, 8U);
}

TEST(A2WSTest, RecordAttempt) {
    hpactor::sched::A2WS a2ws(8, 4);
    a2ws.record_attempt(0, 1, true);
    EXPECT_EQ(a2ws.stats(0).steal_attempts.load(), 1);
    EXPECT_EQ(a2ws.stats(0).steal_successes.load(), 1);

    a2ws.record_attempt(0, 2, false);
    EXPECT_EQ(a2ws.stats(0).steal_attempts.load(), 2);
    EXPECT_EQ(a2ws.stats(0).steal_successes.load(), 1);
}

TEST(A2WSTest, RecordSteal) {
    hpactor::sched::A2WS a2ws(8, 4);

    a2ws.record_steal(0, 1);
    EXPECT_EQ(a2ws.stats(0).local_steals.load(), 1);

    a2ws.record_steal(0, 5); // different pool
    EXPECT_EQ(a2ws.stats(0).local_steals.load(), 1);
    EXPECT_EQ(a2ws.stats(0).remote_steals.load(), 1);
}
