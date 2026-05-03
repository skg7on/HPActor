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

#include <hpactor/mem/thread_local_allocator.hpp>

namespace hpactor::mem {

ThreadLocalAllocator::ThreadLocalAllocator() {
    for (uint8_t i = 0; i < kNumSizeClasses; ++i) {
        caches_[i] = new SlabCache(static_cast<SizeClass>(i));
    }
}

ThreadLocalAllocator::~ThreadLocalAllocator() {
    for (uint8_t i = 0; i < kNumSizeClasses; ++i) {
        delete caches_[i];
    }
}

void* ThreadLocalAllocator::allocate(SizeClass sc, ActorId owner) noexcept {
    return caches_[static_cast<uint8_t>(sc)]->allocate(owner);
}

void* ThreadLocalAllocator::allocate_bytes(size_t user_bytes, ActorId owner) noexcept {
    SizeClass sc = class_for_size(user_bytes);
    return allocate(sc, owner);
}

void ThreadLocalAllocator::deallocate(void* user_ptr) noexcept {
    auto* hdr = AllocHeader::from_user_data(user_ptr);
    SizeClass sc = static_cast<SizeClass>(hdr->size_class);
    caches_[static_cast<uint8_t>(sc)]->deallocate(user_ptr);
}

const SlabCache::Stats& ThreadLocalAllocator::stats(SizeClass sc) const noexcept {
    return caches_[static_cast<uint8_t>(sc)]->stats();
}

} // namespace hpactor::mem
