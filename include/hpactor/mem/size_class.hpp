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

#include <hpactor/adt/size_class.hpp>

namespace hpactor::mem {
using hpactor::adt::class_for_size;
using hpactor::adt::kNumSizeClasses;
using hpactor::adt::kSizeClassTable;
using hpactor::adt::size_for_class;
using hpactor::adt::SizeClass;
} // namespace hpactor::mem

/// \brief Per-block fixed overhead in bytes: AllocHeader (32 B) + CanaryFooter
/// (8 B).
inline constexpr size_t kAllocOverhead = 40;

/// \brief Minimum alignment for every managed block, matching
/// \c AllocHeader::alignas(32).  Bump-allocated blocks within a slab must
/// all start at addresses satisfying this alignment.
inline constexpr size_t kAllocAlignment = 32;

namespace hpactor::mem {

/// \brief Return the total block size (user payload + allocator overhead +
///        alignment padding) for a given size class.
///
/// Rounded up to \c kAllocAlignment so that consecutive bump-allocated
/// blocks within a slab each start at a properly aligned address,
/// satisfying \c AllocHeader's \c alignas(32).
///
/// \param[in] sc The size class.
/// \return Total bytes occupied by one aligned block of this class.
constexpr size_t block_size(SizeClass sc) noexcept {
    size_t raw = size_for_class(sc) + kAllocOverhead;
    return (raw + kAllocAlignment - 1) & ~(kAllocAlignment - 1);
}

/// \brief Return the user-payload size given a total block size.
///
/// The block size may include alignment padding beyond the fixed overhead,
/// so this function looks up the correct size class rather than simply
/// subtracting \c kAllocOverhead.
///
/// \param[in] block_sz Total block size (as returned by \c block_size()).
/// \return User-usable bytes within the block.
constexpr size_t user_size(size_t block_sz) noexcept {
    for (uint8_t i = 0; i < kNumSizeClasses; ++i) {
        if (block_size(static_cast<SizeClass>(i)) == block_sz) {
            return size_for_class(static_cast<SizeClass>(i));
        }
    }
    return block_sz > kAllocOverhead ? block_sz - kAllocOverhead : 0;
}

} // namespace hpactor::mem
