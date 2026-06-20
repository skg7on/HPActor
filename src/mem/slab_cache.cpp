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
    for (auto* slab : idle_slabs_) {
        SegmentProvider::instance().release_slab(slab, size_class_);
    }
    for (auto* slab : slabs_) {
        SegmentProvider::instance().release_slab(slab, size_class_);
    }
}

uint8_t SlabCache::compute_bin_index(const AllocHeader* header) const noexcept {
    // O(1): bin index depends only on byte offset within a same-size slab,
    // not on which slab the block belongs to. All slabs have the same size.
    auto* hdr_addr = reinterpret_cast<const char*>(header);
    auto* slab_base = reinterpret_cast<const char*>(current_slab_);
    size_t offset = static_cast<size_t>(hdr_addr - slab_base);
    if (bin_stride_bytes_ == 0)
        return 0;
    return static_cast<uint8_t>((offset / bin_stride_bytes_) % kNumSegregatedBins);
}

// Shared tail: stamp and return a block popped from any freelist.
static void*
stamp_freelist_block(AllocHeader* block, ActorId owner, uint8_t generation,
                     SizeClass sc, std::atomic<uint64_t>& alloc_cnt,
                     std::atomic<uint32_t>& live_cnt) noexcept {
    FAULT_INJECT("hpactor.allocator.freelist.pop.corrupt") {
        if (block != nullptr) {
            block->magic = 0;
        }
    }
    block->owner_id = owner.value();
    block->magic = kAllocMagic;
    block->generation = generation;
    // Fix #7: re-stamp canary footer on recycled blocks (was missing, causing
    // false-positive corruption on coalesced blocks later deallocated)
    CanaryFooter::stamp(block, block_size(sc));
    alloc_cnt.fetch_add(1, std::memory_order_relaxed);
    live_cnt.fetch_add(1, std::memory_order_relaxed);
    return block->user_data();
}

void* SlabCache::allocate(ActorId owner) noexcept {
    // kSegregatedFit: bump-first for best locality (MEM-001 §3.2).
    // Virgin memory provides contiguous allocation — the segregated bins
    // are only used when the current slab is exhausted.
    if (strategy_ == AllocationStrategy::kSegregatedFit) {
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
        // Bump exhausted — try segregated bins (depth-bounded per bin)
        for (uint8_t i = 0; i < kNumSegregatedBins; ++i) {
            uint8_t bin_idx =
                static_cast<uint8_t>((start_bin_ + i) % kNumSegregatedBins);
            uint8_t depth = 0;
            while (depth < kMaxSearchDepthPerBin) {
                AllocHeader* block = dll_pop(bins_[bin_idx]);
                if (!block)
                    break;
                ++depth;
#if HPACTOR_ENABLE_MEMORY_DEBUG
                size_t cbs = block_size(size_class_);
                if (!CanaryFooter::verify(block, cbs))
                    continue;
                auto* cuser = static_cast<uint8_t*>(block->user_data());
                size_t cusz = size_for_class(size_class_);
                bool poisoned = true;
                for (size_t j = 0; j < cusz && j < 16; ++j) {
                    if (cuser[j] != kPoisonByte) {
                        poisoned = false;
                        break;
                    }
                }
                if (!poisoned)
                    continue;
#endif
                start_bin_ =
                    static_cast<uint8_t>((bin_idx + 1) % kNumSegregatedBins);
                return stamp_freelist_block(block, owner, current_generation_,
                                            size_class_, stats_.alloc_count,
                                            live_count_);
            }
        }
    } else {
        // kCasLifo (default) and kBumpOnly: keep existing behavior
        if (strategy_ != AllocationStrategy::kBumpOnly) {
            auto* block = freelist_.pop();
            // Fix #10: restore debug checks for kCasLifo path
#if HPACTOR_ENABLE_MEMORY_DEBUG
            if (block) {
                size_t cbs = block_size(size_class_);
                if (CanaryFooter::verify(block, cbs)) {
                    auto* cuser = static_cast<uint8_t*>(block->user_data());
                    size_t cusz = size_for_class(size_class_);
                    bool ok = true;
                    for (size_t j = 0; j < cusz && j < 16; ++j) {
                        if (cuser[j] != kPoisonByte) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok)
                        block = nullptr;
                } else {
                    block = nullptr;
                }
            }
#endif
            if (block) {
                return stamp_freelist_block(block, owner, current_generation_,
                                            size_class_, stats_.alloc_count,
                                            live_count_);
            }
        }
        // Try bump allocate (all strategies including kBumpOnly)
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
    }

    // Refill from SegmentProvider (or recycle idle slab for bump-only)
    if (strategy_ == AllocationStrategy::kBumpOnly && !idle_slabs_.empty()) {
        auto* recycled = idle_slabs_.back();
        idle_slabs_.pop_back();
        current_slab_ = recycled;
        bump_offset_ = 0;
        return allocate(owner);
    }
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

    // Coalesce with adjacent free blocks (MEM-002)
    if (coalescing_) {
        hdr = try_coalesce(hdr);
    }

    stats_.free_count.fetch_add(1, std::memory_order_relaxed);
    live_count_.fetch_sub(1, std::memory_order_relaxed);

    // Bump-only: no freelist push. Recycle slab when ALL blocks across all
    // slabs are freed (uses per-cache live_count_ instead of per-slab counter
    // to avoid cross-slab accounting bugs — fixes code review finding #1).
    if (strategy_ == AllocationStrategy::kBumpOnly) {
        // When no blocks are live anywhere in this cache, all slabs are idle
        if (live_count_.load(std::memory_order_relaxed) == 0 && current_slab_ &&
            bump_offset_ >= slab_size_) {
            idle_slabs_.push_back(current_slab_);
            current_slab_ = nullptr;
            slab_size_ = 0;
            bump_offset_ = 0;
        }
        return;
    }

    if (strategy_ == AllocationStrategy::kSegregatedFit) {
        uint8_t bin = compute_bin_index(hdr);
        dll_push(bins_[bin], hdr);
    } else {
        freelist_.push(hdr);
    }
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

        if (strategy_ == AllocationStrategy::kSegregatedFit && slab_size_ > 0) {
            bin_stride_bytes_ =
                static_cast<uint32_t>(slab_size_) / kNumSegregatedBins;
        }
        slabs_.push_back(current_slab_);
        SegmentProvider::instance().register_slab_owner(
            current_slab_, slab_size_, this, region_, size_class_);
        stats_.slab_acquire_count.fetch_add(1, std::memory_order_relaxed);
    }
}

// ── Doubly-linked free list helpers (MEM-002 §3.3) ──────────────
// Bins are thread-confined (single owning SlabCache thread), so we use
// relaxed atomics instead of CAS for push/pop — no contention possible.

// Access the FreeBlockLinkage stored in a freed block's user data.
static FreeBlockLinkage* linkage_of(AllocHeader* block) noexcept {
    return reinterpret_cast<FreeBlockLinkage*>(block->user_data());
}

void SlabCache::dll_push(FreeList<AllocHeader>& bin, AllocHeader* block) noexcept {
    auto* link = linkage_of(block);
    // Read the CAS head without atomic RMW — single-threaded, no contention
    AllocHeader* old_head = bin.pop(); // must pop to maintain CAS invariants
    link->next = old_head;
    link->prev = nullptr;
    if (old_head) {
        linkage_of(old_head)->prev = block;
        bin.push(old_head); // restore old head
    }
    bin.push(block);
}

AllocHeader* SlabCache::dll_pop(FreeList<AllocHeader>& bin) noexcept {
    AllocHeader* popped = bin.pop();
    if (popped) {
        auto* popped_link = linkage_of(popped);
        if (popped_link->next) {
            linkage_of(popped_link->next)->prev = nullptr;
        }
    }
    return popped;
}

void SlabCache::dll_remove(FreeList<AllocHeader>& bin, AllocHeader* block) noexcept {
    auto* link = linkage_of(block);
    AllocHeader* prev = link->prev;
    AllocHeader* next = link->next;

    // Update neighbor DLL links
    if (prev) {
        linkage_of(prev)->next = next;
    }
    if (next) {
        linkage_of(next)->prev = prev;
    }

    // Update CAS head.  We must pop the head to check then re-establish
    // the correct top without creating a self-loop (code review finding #3).
    // Since dll_remove is single-threaded (owning thread only), we can
    // directly compute what the new head should be rather than pop-then-push.
    AllocHeader* head = bin.pop();
    if (head == block) {
        // Block was the head — next becomes the new head (may be null)
        if (next) {
            // Push next directly: its next is already correct from the DLL.
            // The bin was empty after pop, so push(next) just stores it.
            bin.push(next);
        }
        // else: bin becomes empty (no push needed)
    } else if (head) {
        // Block was not the head — put the original head back unchanged
        bin.push(head);
    }

    link->next = nullptr;
    link->prev = nullptr;
}

// ── Coalescing (MEM-002 §3.2) ───────────────────────────────────

void SlabCache::stamp_boundary_footer(AllocHeader* header, size_t block_sz) noexcept {
    auto* overlay = reinterpret_cast<FooterOverlay*>(
        CanaryFooter::from_header(header, block_sz));
    overlay->boundary.block_size = static_cast<uint32_t>(block_sz);
    overlay->boundary.flags = kBoundaryFlagFree;
    overlay->boundary._pad[0] = 0;
    overlay->boundary._pad[1] = 0;
    overlay->boundary._pad[2] = 0;
}

AllocHeader* SlabCache::try_coalesce(AllocHeader* header) noexcept {
    size_t bs = block_size(size_class_);

    // Skip coalescing for blocks too small to store prev pointer (MEM-002
    // Appendix A)
    if (!coalescing_ || !current_slab_ || bs < kMinCoalesceBlockSize)
        return header;

    AllocHeader* result = header;
    size_t result_size = bs;

    // Stamp boundary footer on this block
    stamp_boundary_footer(header, bs);

    // Find which slab this block belongs to (fix #4: avoid cross-slab UB)
    auto* header_addr = reinterpret_cast<char*>(header);
    const char* slab_base = nullptr;
    size_t slab_sz = 0;
    for (auto* s : slabs_) {
        auto* sb = reinterpret_cast<const char*>(s);
        if (header_addr >= sb && header_addr < sb + slab_size_) {
            slab_base = sb;
            slab_sz = slab_size_;
            break;
        }
    }
    if (!slab_base) {
        // Fallback to current_slab_ (should not happen for properly tracked
        // blocks)
        slab_base = reinterpret_cast<const char*>(current_slab_);
        slab_sz = slab_size_;
    }

    // Check left neighbor (constant-time: read footer immediately before
    // header)
    if (header_addr > slab_base) {
        auto* left_footer =
            reinterpret_cast<FooterOverlay*>(header_addr - sizeof(FooterOverlay));
        if (left_footer->boundary.flags & kBoundaryFlagFree) {
            size_t left_sz = left_footer->boundary.block_size;
            auto* left_header =
                reinterpret_cast<AllocHeader*>(header_addr - left_sz);
            // Validate left_header is within the same slab
            auto* left_hdr_addr = reinterpret_cast<const char*>(left_header);
            if (left_hdr_addr >= slab_base && left_hdr_addr < slab_base + slab_sz &&
                left_header->magic == kFreedMagic) {
                uint8_t left_bin = compute_bin_index(left_header);
                dll_remove(bins_[left_bin], left_header);
                result = left_header;
                result_size += left_sz;
            }
        }
    }

    // Check right neighbor (constant-time: read header immediately after block)
    auto* right_addr = header_addr + bs;
    auto* slab_end = slab_base + slab_sz;
    if (right_addr + sizeof(AllocHeader) <= slab_end) {
        auto* right_header = reinterpret_cast<AllocHeader*>(right_addr);
        if (right_header->magic == kFreedMagic) {
            size_t right_sz = block_size(size_class_);
            uint8_t right_bin = compute_bin_index(right_header);
            dll_remove(bins_[right_bin], right_header);
            result_size += right_sz;
        }
    }

    // Re-stamp the coalesced block with updated footer
    if (result != header || result_size != bs) {
        stamp_boundary_footer(result, result_size);
    }

    return result;
}

} // namespace hpactor::mem
