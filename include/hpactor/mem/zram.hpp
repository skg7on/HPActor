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

/// \brief ZRAM support detection and page reclamation hints.
///
/// Provides platform-appropriate \c madvise hints for pushing cold actor
/// memory to compressed swap (ZRAM on Linux) and prefetching it back.
///
/// \note All methods are static and safe to call from any thread.
class ZramManager {
  public:
    /// \brief Check whether ZRAM is available on this system.
    ///
    /// On Linux, checks for the presence of \c /sys/block/zram0/comp_algorithm.
    /// On macOS, always returns \c false (uses \c MADV_FREE instead).
    ///
    /// \return \c true if the platform supports ZRAM page reclamation.
    static bool is_available() noexcept;

    /// \brief Hint that a range of memory should be reclaimed to ZRAM/swap.
    ///
    /// On Linux: calls \c madvise(ptr, size, MADV_PAGEOUT).
    /// On macOS: calls \c madvise(ptr, size, MADV_FREE) as a best-effort
    /// alternative.
    ///
    /// \param[in] ptr Start of the memory range.
    /// \param[in] size Length of the range in bytes.
    static void reclaim_pages(void* ptr, size_t size) noexcept;

    /// \brief Hint that pages are cold and candidates for reclamation.
    ///
    /// On Linux: \c MADV_COLD. On macOS: \c MADV_FREE.
    ///
    /// \param[in] ptr Start of the memory range.
    /// \param[in] size Length of the range in bytes.
    static void mark_cold(void* ptr, size_t size) noexcept;

    /// \brief Hint that pages will be needed soon (prefetch from ZRAM/swap).
    ///
    /// On Linux: \c MADV_WILLNEED. On macOS: no-op.
    ///
    /// \param[in] ptr Start of the memory range.
    /// \param[in] size Length of the range in bytes.
    static void mark_will_need(void* ptr, size_t size) noexcept;
};

} // namespace hpactor::mem
