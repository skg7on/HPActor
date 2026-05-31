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

#include <cstddef>
#include <cstdint>

namespace hpactor::mem {

/// \brief Power-of-two size classes for the slab allocator, from 32 B to 4 KB.
enum class SizeClass : uint8_t {
    k32B = 0,
    k64B = 1,
    k128B = 2,
    k256B = 3,
    k512B = 4,
    k1KB = 5,
    k2KB = 6,
    k4KB = 7,
};

/// \brief Total number of supported size classes.
inline constexpr uint8_t kNumSizeClasses = 8;

/// \brief Lookup table mapping each SizeClass to its user-payload size in
/// bytes.
inline constexpr size_t kSizeClassTable[kNumSizeClasses] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096};

/// \brief Per-block fixed overhead in bytes: AllocHeader (32 B) + CanaryFooter
/// (8 B).
inline constexpr size_t kAllocOverhead = 40;

/// \brief Return the user-payload size for a given size class.
///
/// \param[in] sc The size class.
/// \return User-usable bytes in a block of this class.
constexpr size_t size_for_class(SizeClass sc) noexcept {
    return kSizeClassTable[static_cast<uint8_t>(sc)];
}

/// \brief Return the total block size (user payload + allocator overhead) for a
/// given size class.
///
/// \param[in] sc The size class.
/// \return Total bytes occupied by one block of this class.
constexpr size_t block_size(SizeClass sc) noexcept {
    return size_for_class(sc) + kAllocOverhead;
}

/// \brief Return the user-payload size given a total block size.
///
/// \param[in] block_sz Total block size (as returned by block_size()).
/// \return User-usable bytes within the block.
constexpr size_t user_size(size_t block_sz) noexcept {
    return block_sz - kAllocOverhead;
}

/// \brief Map a user-requested byte count to the smallest SizeClass that fits.
///
/// Requests larger than 4 KB return \c SizeClass::k4KB.
///
/// \param[in] user_bytes Number of bytes the caller needs.
/// \return The size class whose user-payload size is >= \p user_bytes.
inline SizeClass class_for_size(size_t user_bytes) noexcept {
    for (uint8_t i = 0; i < kNumSizeClasses; ++i) {
        if (user_bytes <= kSizeClassTable[i]) {
            return static_cast<SizeClass>(i);
        }
    }
    return SizeClass::k4KB;
}

} // namespace hpactor::mem
