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

#include <gtest/gtest.h>
#include <hpactor/mem/guard_page.hpp>

#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mem = hpactor::mem;

TEST(GuardPageTest, PageSizePositive) {
    size_t ps = mem::page_size();
    EXPECT_GT(ps, 0U);
    // page_size() should be >= 4096 (minimum 4KB)
    EXPECT_GE(ps, 4096U);
}

TEST(GuardPageTest, PageSizeMatchesSysconf) {
    size_t ps = mem::page_size();
    EXPECT_EQ(ps, static_cast<size_t>(sysconf(_SC_PAGESIZE)));
}

TEST(GuardPageTest, GuardedAllocBasic) {
    constexpr size_t kUserSize = 8192;
    void* p = mem::guarded_alloc(kUserSize);
    ASSERT_NE(p, nullptr);

    // Write within bounds should succeed
    std::memset(p, 0x42, kUserSize);
    EXPECT_EQ(*static_cast<uint8_t*>(p), 0x42);
    EXPECT_EQ(*(static_cast<uint8_t*>(p) + kUserSize - 1), 0x42);

    mem::guarded_free(p, kUserSize);
}

TEST(GuardPageTest, GuardedAllocNonnull) {
    void* ptr = mem::guarded_alloc(64);
    ASSERT_NE(ptr, nullptr);
    auto* bytes = static_cast<std::byte*>(ptr);
    std::memset(bytes, 0xAB, 64);
    EXPECT_EQ(bytes[0], std::byte{0xAB});
    EXPECT_EQ(bytes[63], std::byte{0xAB});
    mem::guarded_free(ptr, 64);
}

TEST(GuardPageTest, GuardedAllocZero) {
    void* ptr = mem::guarded_alloc(0);
    ASSERT_NE(ptr, nullptr);
    auto* bytes = static_cast<std::byte*>(ptr);
    bytes[0] = std::byte{0x42};
    mem::guarded_free(ptr, 0);
}

TEST(GuardPageTest, GuardedFreeNull) {
    mem::guarded_free(nullptr, 64);
    // Should not crash
    SUCCEED();
}

TEST(GuardPageTest, GuardedAllocMultiple) {
    void* p1 = mem::guarded_alloc(128);
    void* p2 = mem::guarded_alloc(256);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);
    mem::guarded_free(p1, 128);
    mem::guarded_free(p2, 256);
}

TEST(GuardPageTest, HandlerInstallIdempotent) {
    mem::install_corruption_handler();
    mem::install_corruption_handler();
    // Should not crash
    SUCCEED();
}

TEST(GuardPageTest, HandlerRemoveAndRestore) {
    mem::remove_corruption_handler();
    mem::remove_corruption_handler();
    mem::install_corruption_handler();
    // Should not crash
    SUCCEED();
}

TEST(GuardPageTest, SetLogFd) {
    mem::set_guard_page_log_fd(-1);
    mem::set_guard_page_log_fd(STDERR_FILENO);
    // Should not crash
    SUCCEED();
}

TEST(GuardPageTest, GuardedAllocAndFreeCycle) {
    size_t sizes[] = {16, 64, 256, 1024, 4096};
    for (int si = 0; si < 5; si++) {
        for (int i = 0; i < 5; i++) {
            void* p = mem::guarded_alloc(sizes[si]);
            ASSERT_NE(p, nullptr);
            mem::guarded_free(p, sizes[si]);
        }
    }
}

TEST(GuardPageTest, WritingPastGuardPageCausesSignal) {
    // Test that writing past the guard page causes SIGSEGV
    // We test this by forking — the child attempts the illegal read,
    // and the parent verifies the child died from a signal.
    size_t kGuardTestSize = mem::page_size();

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        // Child: allocate guarded memory
        void* p = mem::guarded_alloc(kGuardTestSize);
        if (!p)
            _exit(1);

        // Touch the valid memory first to ensure it's mapped
        std::memset(p, 0xAB, kGuardTestSize);

        // Read from the trailing guard page (PROT_NONE) — must crash
        auto* byte_ptr = static_cast<uint8_t*>(p);
        volatile uint8_t val = byte_ptr[kGuardTestSize]; // first byte of guard
                                                         // page

        // Should not reach here
        (void)val;
        _exit(0);
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        // Child should die from a signal
        EXPECT_TRUE(WIFSIGNALED(status));
        // macOS raises SIGBUS for some page violations
        int sig = WTERMSIG(status);
        EXPECT_TRUE(sig == SIGSEGV || sig == SIGBUS);
    }
}
