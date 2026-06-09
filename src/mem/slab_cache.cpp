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

#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mem/slab_cache.hpp>

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
    FAULT_INJECT("hpactor.allocator.freelist.pop.corrupt") {
        if (block != nullptr) {
            block->magic = 0;
        }
    }
    if (block) {
#if HPACTOR_ENABLE_MEMORY_DEBUG
        // Verify canary was not corrupted while block was freed
        size_t bs = block_size(size_class_);
        if (!CanaryFooter::verify(block, bs)) {
            HPACTOR_LOG_ERROR(
                log::LogCategory::kMemory, ActorId{0},
                static_cast<uint32_t>(log::LogEventId::kMemoryCorruption),
                "memory corruption detected (canary mismatch in allocate)",
                log::field_ptr("block", block));
            stats_.alloc_count.fetch_add(1, std::memory_order_relaxed);
            live_count_.fetch_add(1, std::memory_order_relaxed);
            return nullptr; // corruption detected, refuse allocation
        }
        // Check poison pattern — first bytes should be 0xAA
        auto* user = static_cast<uint8_t*>(block->user_data());
        size_t usz = size_for_class(size_class_);
        for (size_t i = 0; i < usz && i < 16; ++i) {
            if (user[i] != kPoisonByte) {
                // use-after-free detected
                HPACTOR_LOG_ERROR(
                    log::LogCategory::kMemory, ActorId{0},
                    static_cast<uint32_t>(log::LogEventId::kMemoryCorruption),
                    "use-after-free detected (poison byte violated)",
                    log::field_ptr("block", block));
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
            auto* hdr = AllocHeader::stamp(raw, size_class_, owner, region_);
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
    if (!CanaryFooter::verify(hdr, bs)) {
        HPACTOR_LOG_ERROR(log::LogCategory::kMemory, ActorId{0},
                          static_cast<uint32_t>(log::LogEventId::kMemoryCorruption),
                          "memory corruption detected (canary mismatch in "
                          "deallocate)",
                          log::field_ptr("ptr", user_ptr));
    }
    // Poison user data to catch use-after-free
    std::memset(user_ptr, kPoisonByte, size_for_class(size_class_));
#endif

    hdr->magic = kFreedMagic;
    stats_.free_count.fetch_add(1, std::memory_order_relaxed);
    live_count_.fetch_sub(1, std::memory_order_relaxed);
    freelist_.push(hdr);
}

void SlabCache::refill() {
    FAULT_INJECT("hpactor.allocator.slab_cache.refill_fail") {
        return;
    }
    current_slab_ = static_cast<std::byte*>(
        SegmentProvider::instance().acquire_slab(size_class_));
    if (current_slab_) {
        slab_size_ = SegmentProvider::instance().slab_size(size_class_);
        bump_offset_ = 0;
        slabs_.push_back(current_slab_);
        SegmentProvider::instance().register_slab_owner(
            current_slab_, slab_size_, this, region_, size_class_);
        stats_.slab_acquire_count.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace hpactor::mem
