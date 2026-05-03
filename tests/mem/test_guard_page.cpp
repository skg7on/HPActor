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
        constexpr size_t kUserSize = 4096;

        pid_t pid = fork();
        assert(pid >= 0);

        if (pid == 0) {
            // Child: allocate guarded memory
            void* p = guarded_alloc(kUserSize);
            if (!p) _exit(1);

            // Touch the valid memory first to ensure it's mapped
            std::memset(p, 0xAB, kUserSize);

            // Write far past the end (multiple pages) to ensure we hit a guard
            auto* byte_ptr = static_cast<uint8_t*>(p);
            volatile uint8_t val = byte_ptr[kUserSize + page_size()]; // well beyond

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

    std::cout << "test_guard_page: PASS\n";
    return 0;
}
