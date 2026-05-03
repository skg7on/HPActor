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

#pragma once

#include <hpactor/platform.hpp>

#include <cstddef>

namespace hpactor::mem {

// ZRAM support detection and page reclamation hints.
class ZramManager {
  public:
    // Check if ZRAM is available on this system.
    // Linux: checks /sys/block/zram0/comp_algorithm
    // macOS: always returns false (uses MADV_FREE instead)
    static bool is_available() noexcept;

    // Hint that a range of memory should be reclaimed to ZRAM.
    // On Linux: calls madvise(ptr, size, MADV_PAGEOUT) to push pages to swap/ZRAM.
    // On macOS: calls madvise(ptr, size, MADV_FREE) as a best-effort alternative.
    static void reclaim_pages(void* ptr, size_t size) noexcept;

    // Hint that pages are cold (candidate for reclamation).
    static void mark_cold(void* ptr, size_t size) noexcept;

    // Hint that pages will be needed soon (prefetch from ZRAM).
    static void mark_will_need(void* ptr, size_t size) noexcept;
};

} // namespace hpactor::mem
