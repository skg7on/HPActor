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
#include <hpactor/platform.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace hpactor::mem {

size_t page_size() noexcept {
    static size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return ps;
}

void* guarded_alloc(size_t user_bytes) noexcept {
    size_t ps = page_size();
    // Round user_bytes up to page boundary so trailing guard is page-aligned.
    // Layout: [guard page] [usable (page-aligned)] [guard page]
    size_t user_pages = ((user_bytes + ps - 1) / ps) * ps;
    size_t total = ps + user_pages + ps;

    void* base = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return nullptr;

    // Protect leading guard page
    mprotect(base, ps, PROT_NONE);

    // Protect trailing guard page (page-aligned)
    auto* trailing = static_cast<std::byte*>(base) + ps + user_pages;
    mprotect(trailing, ps, PROT_NONE);

    return static_cast<std::byte*>(base) + ps;
}

void guarded_free(void* user_ptr, size_t user_bytes) noexcept {
    if (!user_ptr)
        return;
    size_t ps = page_size();
    size_t user_pages = ((user_bytes + ps - 1) / ps) * ps;
    void* base = static_cast<std::byte*>(user_ptr) - ps;
    size_t total = ps + user_pages + ps;
    munmap(base, total);
}

// ---------------------------------------------------------------------------
// Corruption signal handler
// ---------------------------------------------------------------------------

namespace {
// Previous signal handler (chained on non-corruption faults)
struct sigaction g_prev_action;
bool g_handler_installed = false;

// Pre-opened fd for signal-safe logging. Use write() instead of the logger
// in signal context — logger CAS atomics may deadlock.
int g_guard_page_fd = -1;

void corruption_sigaction(int sig, siginfo_t* info, void* ctx) {
    if (!info || !info->si_addr) {
        // Chain to previous handler
        if (g_prev_action.sa_sigaction) {
            g_prev_action.sa_sigaction(sig, info, ctx);
        }
        return;
    }

    void* fault_addr = info->si_addr;

    // Try to identify the owning segment
    auto seg_info = SegmentProvider::instance().lookup(fault_addr);
    if (seg_info.base != nullptr) {
        // This is our memory — corruption detected
        // Use direct write() to pre-opened fd — never use the logger in
        // signal context (CAS atomics may deadlock).
        if (g_guard_page_fd >= 0) {
            const char* msg = "HPActor: guard page violation at %p (segment "
                              "base %p, size %zu) — memory corruption\n";
            char buf[256];
            int len = snprintf(buf, sizeof(buf), msg, fault_addr, seg_info.base,
                               seg_info.size);
            write(g_guard_page_fd, buf, static_cast<size_t>(len));
        }
        _exit(EXIT_FAILURE);
    }

    // Not our memory — chain to previous handler
    if (g_prev_action.sa_sigaction) {
        g_prev_action.sa_sigaction(sig, info, ctx);
    } else {
        // No previous handler, restore default and re-raise
        signal(sig, SIG_DFL);
        raise(sig);
    }
}
} // namespace

void set_guard_page_log_fd(int fd) noexcept {
    g_guard_page_fd = fd;
}

void install_corruption_handler() noexcept {
    if (g_handler_installed)
        return;

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = corruption_sigaction;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;

    sigaction(SIGSEGV, &sa, &g_prev_action);
    g_handler_installed = true;
}

void remove_corruption_handler() noexcept {
    if (!g_handler_installed)
        return;

    sigaction(SIGSEGV, &g_prev_action, nullptr);
    g_handler_installed = false;
}

} // namespace hpactor::mem
