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

// AllocHeader (32 bytes) + CanaryFooter (8 bytes) = 40 bytes overhead
inline constexpr size_t kAllocOverhead = 40;

namespace hpactor::mem {

constexpr size_t block_size(SizeClass sc) noexcept {
    return size_for_class(sc) + kAllocOverhead;
}

constexpr size_t user_size(size_t block_sz) noexcept {
    return block_sz - kAllocOverhead;
}

} // namespace hpactor::mem
