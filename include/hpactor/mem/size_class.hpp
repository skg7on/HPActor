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

enum class SizeClass : uint8_t {
    k32B  = 0,
    k64B  = 1,
    k128B = 2,
    k256B = 3,
    k512B = 4,
    k1KB  = 5,
    k2KB  = 6,
    k4KB  = 7,
};

inline constexpr uint8_t kNumSizeClasses = 8;

inline constexpr size_t kSizeClassTable[kNumSizeClasses] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096
};

// AllocHeader (32 bytes) + CanaryFooter (8 bytes) = 40 bytes overhead
inline constexpr size_t kAllocOverhead = 40;

constexpr size_t size_for_class(SizeClass sc) noexcept {
    return kSizeClassTable[static_cast<uint8_t>(sc)];
}

constexpr size_t block_size(SizeClass sc) noexcept {
    return size_for_class(sc) + kAllocOverhead;
}

constexpr size_t user_size(size_t block_sz) noexcept {
    return block_sz - kAllocOverhead;
}

inline SizeClass class_for_size(size_t user_bytes) noexcept {
    for (uint8_t i = 0; i < kNumSizeClasses; ++i) {
        if (user_bytes <= kSizeClassTable[i]) {
            return static_cast<SizeClass>(i);
        }
    }
    return SizeClass::k4KB;
}

} // namespace hpactor::mem
