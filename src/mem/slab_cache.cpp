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

#include <hpactor/mem/slab_cache.hpp>
#include <hpactor/hpactor_config.hpp>

#include <cstring>

namespace hpactor::mem {

#if HPACTOR_ENABLE_MEMORY_DEBUG
namespace {
inline constexpr uint8_t kPoisonByte = 0xAA;
} // namespace
#endif

SlabCache::~SlabCache() {
    for (auto* slab : slabs_) {
        SegmentProvider::instance().release_slab(slab, size_class_);
    }
}

void* SlabCache::allocate(ActorId owner) noexcept {
    // 1. Try freelist first
    auto* block = freelist_.pop();
    if (block) {
#if HPACTOR_ENABLE_MEMORY_DEBUG
        // Verify canary was not corrupted while block was freed
        size_t bs = block_size(size_class_);
        if (!CanaryFooter::verify(block, bs)) {
            stats_.alloc_count.fetch_add(1, std::memory_order_relaxed);
            live_count_.fetch_add(1, std::memory_order_relaxed);
            return nullptr; // corruption detected, refuse allocation
        }
        // Check poison pattern — first bytes should be 0xAA
        auto* user = static_cast<uint8_t*>(block->user_data());
        size_t usz = user_size(bs);
        for (size_t i = 0; i < usz && i < 16; ++i) {
            if (user[i] != kPoisonByte) {
                // use-after-free detected
                stats_.alloc_count.fetch_add(1, std::memory_order_relaxed);
                live_count_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
        }
#endif
        block->owner_id = owner.value();
        block->magic = kAllocMagic;
        block->generation = current_generation_;
        stats_.alloc_count.fetch_add(1, std::memory_order_relaxed);
        live_count_.fetch_add(1, std::memory_order_relaxed);
        return block->user_data();
    }

    // 2. Bump allocate from current slab
    if (current_slab_) {
        size_t bs = block_size(size_class_);
        if (bump_offset_ + bs <= slab_size_) {
            auto* raw = current_slab_ + bump_offset_;
            bump_offset_ += bs;
            auto* hdr = AllocHeader::stamp(raw, size_class_, owner);
            hdr->generation = current_generation_;
            CanaryFooter::stamp(hdr, bs);
            stats_.alloc_count.fetch_add(1, std::memory_order_relaxed);
            live_count_.fetch_add(1, std::memory_order_relaxed);
            return hdr->user_data();
        }
    }

    // 3. Need a new slab
    refill();
    if (current_slab_) {
        return allocate(owner);
    }
    return nullptr;
}

void SlabCache::deallocate(void* user_ptr) noexcept {
    auto* hdr = AllocHeader::from_user_data(user_ptr);

#if HPACTOR_ENABLE_MEMORY_DEBUG
    size_t bs = block_size(size_class_);
    // Verify canary before freeing (catch buffer overflow)
    CanaryFooter::verify(hdr, bs);
    // Poison user data to catch use-after-free
    std::memset(user_ptr, kPoisonByte, user_size(bs));
#endif

    hdr->magic = kFreedMagic;
    stats_.free_count.fetch_add(1, std::memory_order_relaxed);
    live_count_.fetch_sub(1, std::memory_order_relaxed);
    freelist_.push(hdr);
}

void SlabCache::refill() {
    current_slab_ = static_cast<std::byte*>(
        SegmentProvider::instance().acquire_slab(size_class_));
    if (current_slab_) {
        slab_size_ = SegmentProvider::instance().slab_size(size_class_);
        bump_offset_ = 0;
        slabs_.push_back(current_slab_);
        stats_.slab_acquire_count.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace hpactor::mem
