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

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <hpactor/sched/dedicated_thread_pool.hpp>
#include <hpactor/sched/work_queue.hpp>
#include <hpactor/types/types.hpp>
#include <thread>

using namespace hpactor;
using namespace hpactor::sched;

TEST(DedicatedThreadPoolTest, StartStopLifecycle) {
    DedicatedThreadPool pool(2);
    EXPECT_FALSE(pool.is_running());
    pool.start();
    EXPECT_TRUE(pool.is_running());
    pool.stop();
    EXPECT_FALSE(pool.is_running());
}

TEST(DedicatedThreadPoolTest, EnqueueAndProcessWork) {
    DedicatedThreadPool pool(2);
    pool.start();

    std::atomic<int> counter{0};
    ActorId test_id(42);

    pool.enqueue(test_id, [&counter]() { counter.fetch_add(1); });
    pool.enqueue(test_id, [&counter]() { counter.fetch_add(1); });

    // Give workers time to process
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && counter.load() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(counter.load(), 2);

    pool.stop();
}

TEST(DedicatedThreadPoolTest, MultipleActorsOnSamePool) {
    DedicatedThreadPool pool(2);
    pool.start();

    std::atomic<int> a_count{0};
    std::atomic<int> b_count{0};
    ActorId actor_a(1);
    ActorId actor_b(2);

    for (int i = 0; i < 10; i++) {
        pool.enqueue(actor_a, [&a_count]() { a_count.fetch_add(1); });
        pool.enqueue(actor_b, [&b_count]() { b_count.fetch_add(1); });
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           (a_count.load() < 10 || b_count.load() < 10)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(a_count.load(), 10);
    EXPECT_EQ(b_count.load(), 10);

    pool.stop();
}
