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

#include <hpactor/mem/guard_page.hpp>

#include <cassert>
#include <cstring>
#include <iostream>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mem = hpactor::mem;

void test_page_size_positive() {
    size_t ps = mem::page_size();
    assert(ps > 0);
    assert(ps == static_cast<size_t>(sysconf(_SC_PAGESIZE)));
    printf("  PASSED test_page_size_positive\n");
}

void test_guarded_alloc_nonnull() {
    void* ptr = mem::guarded_alloc(64);
    assert(ptr != nullptr);
    auto* bytes = static_cast<std::byte*>(ptr);
    std::memset(bytes, 0xAB, 64);
    assert(bytes[0] == std::byte{0xAB});
    assert(bytes[63] == std::byte{0xAB});
    mem::guarded_free(ptr, 64);
    printf("  PASSED test_guarded_alloc_nonnull\n");
}

void test_guarded_alloc_zero() {
    void* ptr = mem::guarded_alloc(0);
    assert(ptr != nullptr);
    auto* bytes = static_cast<std::byte*>(ptr);
    bytes[0] = std::byte{0x42};
    mem::guarded_free(ptr, 0);
    printf("  PASSED test_guarded_alloc_zero\n");
}

void test_guarded_free_null() {
    mem::guarded_free(nullptr, 64);
    printf("  PASSED test_guarded_free_null\n");
}

void test_guarded_alloc_multiple() {
    void* p1 = mem::guarded_alloc(128);
    void* p2 = mem::guarded_alloc(256);
    assert(p1 != nullptr);
    assert(p2 != nullptr);
    assert(p1 != p2);
    mem::guarded_free(p1, 128);
    mem::guarded_free(p2, 256);
    printf("  PASSED test_guarded_alloc_multiple\n");
}

void test_handler_install_idempotent() {
    mem::install_corruption_handler();
    mem::install_corruption_handler();
    printf("  PASSED test_handler_install_idempotent\n");
}

void test_handler_remove_and_restore() {
    mem::remove_corruption_handler();
    mem::remove_corruption_handler();
    mem::install_corruption_handler();
    printf("  PASSED test_handler_remove_and_restore\n");
}

void test_set_log_fd() {
    mem::set_guard_page_log_fd(-1);
    mem::set_guard_page_log_fd(STDERR_FILENO);
    printf("  PASSED test_set_log_fd\n");
}

void test_guarded_alloc_and_free_cycle() {
    size_t sizes[] = {16, 64, 256, 1024, 4096};
    for (int si = 0; si < 5; si++) {
        for (int i = 0; i < 5; i++) {
            void* p = mem::guarded_alloc(sizes[si]);
            assert(p != nullptr);
            mem::guarded_free(p, sizes[si]);
        }
    }
    printf("  PASSED test_guarded_alloc_and_free_cycle\n");
}

int main() {
    using namespace hpactor::mem;

    // Test page_size()
    size_t ps = page_size();
    assert(ps >= 4096); // minimum 4KB

    // Test basic guarded allocation
    {
        constexpr size_t kUserSize = 8192;
        void* p = guarded_alloc(kUserSize);
        assert(p != nullptr);

        // Write within bounds should succeed
        std::memset(p, 0x42, kUserSize);
        assert(*static_cast<uint8_t*>(p) == 0x42);
        assert(*(static_cast<uint8_t*>(p) + kUserSize - 1) == 0x42);

        guarded_free(p, kUserSize);
    }

    // Test that writing past the guard page causes SIGSEGV
    // We test this by forking — the child attempts the illegal write,
    // and the parent verifies the child died from SIGSEGV.
    {
        // Use page_size() as allocation so rounding is a no-op
        size_t kGuardTestSize = page_size();

        pid_t pid = fork();
        assert(pid >= 0);

        if (pid == 0) {
            // Child: allocate guarded memory
            void* p = guarded_alloc(kGuardTestSize);
            if (!p)
                _exit(1);

            // Touch the valid memory first to ensure it's mapped
            std::memset(p, 0xAB, kGuardTestSize);

            // Read from the trailing guard page (PROT_NONE) — must crash
            auto* byte_ptr = static_cast<uint8_t*>(p);
            volatile uint8_t val = byte_ptr[kGuardTestSize]; // first byte of
                                                             // guard page

            // Should not reach here
            (void)val;
            _exit(0);
        } else {
            int status = 0;
            waitpid(pid, &status, 0);
            // Child should die from SIGSEGV (signal 11)
            bool was_signaled = WIFSIGNALED(status);
            assert(was_signaled);
            // macOS raises SIGBUS for some page violations
            int sig = WTERMSIG(status);
            assert(sig == SIGSEGV || sig == SIGBUS);
        }
    }

    test_page_size_positive();
    test_guarded_alloc_nonnull();
    test_guarded_alloc_zero();
    test_guarded_free_null();
    test_guarded_alloc_multiple();
    test_handler_install_idempotent();
    test_handler_remove_and_restore();
    test_set_log_fd();
    test_guarded_alloc_and_free_cycle();

    std::cout << "test_guard_page: PASS\n";
    return 0;
}
