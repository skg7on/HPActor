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

// tests/integration/sched/test_worker_thread.cpp
#include <hpactor/coroutine/coroutine_frame_pool.hpp>
#include <hpactor/sched/worker_thread.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using hpactor::ActorId;
using namespace hpactor::sched;

TEST(WorkerThreadTest, ConfigDefaults) {
    WorkerThread::Config cfg;
    EXPECT_EQ(cfg.worker_index, 0);
    EXPECT_EQ(cfg.priority_levels, 4);
    EXPECT_EQ(cfg.steal_threshold, 10);
    EXPECT_EQ(cfg.victim_scan_limit, 4);
}

TEST(WorkerThreadTest, Index) {
    WorkerThread::Config cfg;
    cfg.worker_index = 42;
    WorkerThread worker(cfg);
    EXPECT_EQ(worker.index(), 42);
}

TEST(WorkerThreadTest, StartStop) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    EXPECT_FALSE(worker.is_running());
    worker.start();
    EXPECT_TRUE(worker.is_running());
    worker.stop();
    EXPECT_FALSE(worker.is_running());
}

TEST(WorkerThreadTest, DoubleStart) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    worker.start();
    EXPECT_TRUE(worker.is_running());
    worker.start(); // double start is safe
    EXPECT_TRUE(worker.is_running());
    worker.stop();
}

TEST(WorkerThreadTest, PushPop) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem item;
    item.actor = ActorId{0};
    worker.push(0, item);
    WorkItem out;
    EXPECT_TRUE(worker.pop(out));
    EXPECT_EQ(out.actor, ActorId{0});
}

TEST(WorkerThreadTest, PopEmpty) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem out;
    EXPECT_FALSE(worker.pop(out));
}

TEST(WorkerThreadTest, Steal) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem item1;
    item1.actor = ActorId{1};
    worker.push(0, item1);
    WorkItem item2;
    item2.actor = ActorId{2};
    worker.push(1, item2);
    WorkItem stolen;
    EXPECT_TRUE(worker.steal(stolen));
    EXPECT_EQ(stolen.actor, ActorId{1});
    WorkItem popped;
    EXPECT_TRUE(worker.pop(popped));
    EXPECT_EQ(popped.actor, ActorId{2});
}

TEST(WorkerThreadTest, StealEmpty) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem out;
    EXPECT_FALSE(worker.steal(out));
}

TEST(WorkerThreadTest, Depth) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    WorkItem item;
    item.actor = ActorId{0};
    worker.push(0, item);
    EXPECT_GE(worker.depth(), 1);
}

TEST(WorkerThreadTest, DonationCount) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    EXPECT_EQ(worker.donation_count(), 0);
    worker.increment_donations();
    EXPECT_EQ(worker.donation_count(), 1);
    worker.increment_donations();
    worker.increment_donations();
    EXPECT_EQ(worker.donation_count(), 3);
}

TEST(WorkerThreadTest, AcquireReleaseFrame) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    EXPECT_EQ(worker.acquire_frame(), nullptr);
    CoroutineFramePool pool(4, 1024);
    worker.set_frame_pool(&pool);
    auto* f = worker.acquire_frame();
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(f->in_use);
    worker.release_frame(f);
    EXPECT_FALSE(f->in_use);
    worker.release_frame(nullptr); // safety: nullptr release is no-op
}

TEST(WorkerThreadTest, ThreadIdIsReasonable) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);

    // Before start, thread_id may return 0 or a hash of the default-constructed
    // std::thread::id (which represents "not a thread").
    uint64_t tid_before = worker.thread_id();

    worker.start();
    uint64_t tid_after = worker.thread_id();
    worker.stop();

    EXPECT_NE(tid_after, uint64_t(0));
    // A real platform thread ID should be well under 2^48.
    // The old std::hash<std::thread::id> approach on Linux produces values
    // in the 2^62–2^64 range (e.g., 13886910359752328644), which is a clear
    // indicator of a hashed/garbage value rather than a kernel TID.
    EXPECT_LT(tid_after, uint64_t(1) << 48)
        << "Thread ID " << tid_after << " looks like a hashed/garbage value — "
        << "expected a real kernel thread ID, got a hash";
    // After start, the thread ID should differ from the pre-start value (which
    // represents "not a thread").
    EXPECT_NE(tid_after, tid_before)
        << "Thread ID did not change after start() — still using the "
        << "default-constructed std::thread::id";
}

TEST(WorkerThreadTest, WorkProcessor) {
    WorkerThread::Config cfg;
    WorkerThread worker(cfg);
    std::atomic<bool> called{false};
    WorkItem sent_item;
    sent_item.actor = ActorId{0};
    worker.set_work_processor([&](const WorkItem& item) {
        called.store(true);
        EXPECT_EQ(item.actor, ActorId{0});
    });
    // Push work so the loop can pop and invoke the processor
    worker.push(0, sent_item);
    worker.start();
    // Poll until the processor is invoked
    auto start = std::chrono::steady_clock::now();
    while (!called.load() &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    worker.stop();
    EXPECT_TRUE(called.load());
}
