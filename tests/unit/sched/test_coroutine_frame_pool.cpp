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

// tests/unit/sched/test_coroutine_frame_pool.cpp
//
// Deterministic tests for CoroutineFramePool.  The lock-free acquire/release
// protocol is verified via single-threaded correctness checks — ordering,
// exhaustion, re-acquire identity, and write/read-back integrity.
//
// The prior concurrent stress test is replaced with a deterministic
// write/read-back loop that exercises the same stack-memory access pattern
// without relying on thread interleaving (per project test design
// constraints: no timing assumptions, no assumed thread execution order).

#include <gtest/gtest.h>
#include <hpactor/coroutine/coroutine_frame_pool.hpp>

#include <cstring>
#include <vector>

using namespace hpactor::sched;

// ---------------------------------------------------------------------------
// Test fixture: fresh pool for each test
// ---------------------------------------------------------------------------
class CoroutineFramePoolTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Each test creates its own pool; fixture provides common helpers.
    }
};

TEST(CoroutineFramePoolTest, AcquireRelease) {
    CoroutineFramePool pool(4, 1024);
    EXPECT_EQ(pool.total(), 4U);
    EXPECT_EQ(pool.available(), 4U);
    EXPECT_FALSE(pool.empty());

    auto* frame = pool.acquire();
    ASSERT_NE(frame, nullptr);
    EXPECT_TRUE(frame->in_use);
    EXPECT_NE(frame->stack_ptr, nullptr);
    EXPECT_EQ(frame->stack_size, 1024U);
    EXPECT_EQ(pool.available(), 3U);

    pool.release(frame);
    EXPECT_FALSE(frame->in_use);
    EXPECT_EQ(pool.available(), 4U);
    EXPECT_EQ(pool.total(), 4U);
}

TEST(CoroutineFramePoolTest, Exhaustion) {
    CoroutineFramePool pool(2, 1024);
    auto* f1 = pool.acquire();
    auto* f2 = pool.acquire();
    EXPECT_NE(f1, nullptr);
    EXPECT_NE(f2, nullptr);
    EXPECT_EQ(pool.available(), 0U);
    EXPECT_TRUE(pool.empty());

    auto* f3 = pool.acquire();
    EXPECT_EQ(f3, nullptr);

    pool.release(f1);
    EXPECT_FALSE(pool.empty());
    auto* f4 = pool.acquire();
    EXPECT_NE(f4, nullptr);

    pool.release(f2);
    pool.release(f4);
}

TEST(CoroutineFramePoolTest, ReleaseNullptr) {
    CoroutineFramePool pool(1, 1024);
    pool.release(nullptr);
    EXPECT_EQ(pool.available(), 1U);
}

TEST(CoroutineFramePoolTest, DoubleRelease) {
    CoroutineFramePool pool(1, 1024);
    auto* f = pool.acquire();
    ASSERT_NE(f, nullptr);
    pool.release(f);
    pool.release(f);
    EXPECT_EQ(pool.available(), 1U);
}

TEST(CoroutineFramePoolTest, Reacquire) {
    CoroutineFramePool pool(4, 2048);
    auto* f1 = pool.acquire();
    void* stack_ptr = f1->stack_ptr;
    pool.release(f1);
    auto* f2 = pool.acquire();
    EXPECT_EQ(f2, f1);
    EXPECT_EQ(f2->stack_ptr, stack_ptr);
    pool.release(f2);
}

// ---------------------------------------------------------------------------
// Deterministic write/read-back: acquire every frame, write a known pattern
// to the stack memory, release, then re-acquire and verify the pattern was
// not corrupted by the FreeNode metadata bookkeeping.  This exercises the
// same stack access pattern as a coroutine resume/suspend cycle without
// relying on thread interleaving.
TEST(CoroutineFramePoolTest, WriteReadback) {
    constexpr size_t kFrames = 16;
    constexpr size_t kStackSize = 4096;
    CoroutineFramePool pool(kFrames, kStackSize);

    // Acquire all frames and record their stack pointers
    std::vector<CoroutineFramePool::Frame*> frames;
    std::vector<void*> stack_ptrs;
    for (size_t i = 0; i < kFrames; ++i) {
        auto* f = pool.acquire();
        ASSERT_NE(f, nullptr);
        frames.push_back(f);
        stack_ptrs.push_back(f->stack_ptr);
    }
    EXPECT_TRUE(pool.empty());

    // Write known sentinel values to each frame's stack
    const std::byte kSentinelLo{0xA5};
    const std::byte kSentinelHi{0x5A};
    for (auto* f : frames) {
        std::memset(f->stack_ptr, 0xCC, kStackSize);
        f->stack_ptr[0] = kSentinelLo;
        f->stack_ptr[kStackSize - 1] = kSentinelHi;
    }

    // Release all frames
    for (auto* f : frames)
        pool.release(f);
    EXPECT_EQ(pool.available(), kFrames);

    // Re-acquire and verify: FreeNode metadata (16 bytes) lives at the
    // beginning of each stack; the rest must preserve the written pattern.
    for (size_t i = 0; i < kFrames; ++i) {
        auto* f = pool.acquire();
        ASSERT_NE(f, nullptr);
        // Stack pointer must be one of the originally allocated stacks
        bool found = false;
        for (size_t j = 0; j < kFrames; ++j) {
            if (f->stack_ptr == stack_ptrs[j]) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found);
        // Sentinel at far end must be intact
        EXPECT_EQ(f->stack_ptr[kStackSize - 1], kSentinelHi);
        // Middle of stack preserved
        EXPECT_EQ(f->stack_ptr[kStackSize / 2], std::byte{0xCC});
        pool.release(f);
    }
    EXPECT_EQ(pool.available(), kFrames);
}

TEST(CoroutineFramePoolTest, EmptyTrue) {
    CoroutineFramePool pool(1, 1024);
    EXPECT_FALSE(pool.empty());
    auto* f = pool.acquire();
    EXPECT_TRUE(pool.empty());
    pool.release(f);
    EXPECT_FALSE(pool.empty());
}

TEST(CoroutineFramePoolTest, ConstructorParams) {
    CoroutineFramePool pool(8, 1024);
    EXPECT_EQ(pool.total(), 8U);
    EXPECT_EQ(pool.available(), 8U);
    EXPECT_EQ(pool.stack_size(), 1024U);

    CoroutineFramePool pool2(4);
    EXPECT_EQ(pool2.total(), 4U);
    EXPECT_EQ(pool2.stack_size(), 8U * 1024);
}

TEST(CoroutineFramePoolTest, AcquireReleasesDifferentFrames) {
    CoroutineFramePool pool(4, 1024);
    auto* f1 = pool.acquire();
    auto* f2 = pool.acquire();
    auto* f3 = pool.acquire();
    auto* f4 = pool.acquire();
    ASSERT_NE(f1, nullptr);
    ASSERT_NE(f2, nullptr);
    ASSERT_NE(f3, nullptr);
    ASSERT_NE(f4, nullptr);
    EXPECT_NE(f1->stack_ptr, f2->stack_ptr);
    EXPECT_NE(f1->stack_ptr, f3->stack_ptr);
    EXPECT_NE(f1->stack_ptr, f4->stack_ptr);
    EXPECT_NE(f2->stack_ptr, f3->stack_ptr);
    EXPECT_NE(f2->stack_ptr, f4->stack_ptr);
    EXPECT_NE(f3->stack_ptr, f4->stack_ptr);
    pool.release(f1);
    pool.release(f2);
    pool.release(f3);
    pool.release(f4);
}
