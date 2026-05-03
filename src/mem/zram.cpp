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

#include <hpactor/mem/zram.hpp>

#include <sys/mman.h>

#if HPACTOR_PLATFORM_LINUX
#    include <fstream>
#    include <string>
#endif

namespace hpactor::mem {

bool ZramManager::is_available() noexcept {
#if HPACTOR_PLATFORM_LINUX
    // Check if zram0 device exists and has a compression algorithm configured
    std::ifstream f("/sys/block/zram0/comp_algorithm");
    if (!f.is_open()) return false;
    std::string line;
    std::getline(f, line);
    return !line.empty() && line.find("none") == std::string::npos;
#else
    return false;
#endif
}

void ZramManager::reclaim_pages(void* ptr, size_t size) noexcept {
    if (!ptr || size == 0) return;
#if HPACTOR_PLATFORM_LINUX
    madvise(ptr, size, MADV_PAGEOUT);
#elif HPACTOR_PLATFORM_MACOS
    // macOS: MADV_FREE is the closest equivalent — marks pages as
    // reusable, kernel will zero-fill on next access.
    madvise(ptr, size, MADV_FREE);
#endif
}

void ZramManager::mark_cold(void* ptr, size_t size) noexcept {
    if (!ptr || size == 0) return;
#if HPACTOR_PLATFORM_LINUX
    madvise(ptr, size, MADV_COLD);
#elif HPACTOR_PLATFORM_MACOS
    // macOS 10.15+ supports MADV_COLD
    madvise(ptr, size, MADV_FREE);
#endif
}

void ZramManager::mark_will_need(void* ptr, size_t size) noexcept {
    if (!ptr || size == 0) return;
    madvise(ptr, size, MADV_WILLNEED);
}

} // namespace hpactor::mem
