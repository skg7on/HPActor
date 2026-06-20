# MEM-002: Free Block Coalescing — Implementation Plan

**Date:** 2026-06-20
**Design Spec:** `docs/superpowers/specs/2026-06-20-mem-02-free-block-coalescing-design.md`
**Issue:** #339 (Phase 1.2, P0)
**Depends on:** MEM-001 (Segregated Free Lists)

## Phase Overview

| Phase | Name | Tests | Files Changed |
|-------|------|-------|---------------|
| 1 | `BoundaryFooter` struct + `FooterOverlay` union | 2 | 1 |
| 2 | Stamp/read boundary footer on free | 2 | 2 |
| 3 | Left-neighbor coalescing | 2 | 1 |
| 4 | Right-neighbor coalescing | 2 | 1 |
| 5 | Both-neighbor coalescing + boundary conditions | 3 | 1 |
| 6 | Freelist `remove()` for middle-of-list extraction | 3 | 2 |
| 7 | Coalescing + segregated bins integration | 2 | 1 |
| 8 | Fault injection | 2 | 2 |

All phases follow RED → GREEN → REFACTOR.

---

## Phase 1: `BoundaryFooter` Struct + `FooterOverlay` Union

**Goal:** Define the footer overlay that lets freed blocks store size/status
in the `CanaryFooter` slot without increasing block size.

### RED — Write failing tests

File: `tests/unit/mem/test_free_block_coalescing.cpp` (new)

```cpp
#include <gtest/gtest.h>
#include "hpactor/mem/alloc_header.hpp"

using namespace hpactor::mem;

TEST(Coalescing, BoundaryFooterSizeMatchesCanaryFooter) {
    EXPECT_EQ(sizeof(BoundaryFooter), sizeof(CanaryFooter));
    EXPECT_EQ(sizeof(BoundaryFooter), 8u);
}

TEST(Coalescing, FooterOverlaySizeConstraint) {
    // FooterOverlay must be exactly 8 bytes
    EXPECT_EQ(sizeof(FooterOverlay), 8u);
    // And align with a block's footer slot
    EXPECT_EQ(alignof(FooterOverlay), alignof(uint64_t));
}
```

### GREEN — Implement

1. Add to `include/hpactor/mem/alloc_header.hpp`:
   ```cpp
   struct BoundaryFooter {
       uint32_t block_size;
       uint8_t  flags;
       uint8_t  padding[3];
   };
   static_assert(sizeof(BoundaryFooter) == 8);

   static constexpr uint8_t kFlagFree = 0x01;

   union FooterOverlay {
       CanaryFooter   canary;
       BoundaryFooter boundary;
   };
   static_assert(sizeof(FooterOverlay) == 8);
   ```

### REFACTOR — Verify clean

- Verify `CanaryFooter` struct still 8 bytes after any changes

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*Coalescing*"
```

---

## Phase 2: Stamp/Read Boundary Footer on Free

**Goal:** On `deallocate()`, overwrite the canary with a boundary footer
containing the block size. On coalescing check, read the neighbor's footer.

### RED — Write failing tests

```cpp
TEST(Coalescing, StampBoundaryFooterOnFree) {
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    auto* block = cache.allocate(ActorId{1});
    cache.deallocate(block);

    // Read the footer at the end of the freed block
    auto* header = AllocHeader::from_user_data(block);
    auto* footer = reinterpret_cast<FooterOverlay*>(
        reinterpret_cast<char*>(header) + size_for_class(SizeClass::k64) - 8);
    EXPECT_TRUE(footer->boundary.flags & kFlagFree);
    EXPECT_EQ(footer->boundary.block_size, size_for_class(SizeClass::k64));
}

TEST(Coalescing, BoundaryFooterNotStampedOnLiveBlock) {
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    auto* block = cache.allocate(ActorId{1});
    // Block is live — footer is still CanaryFooter (kAllocMagic)
    auto* header = AllocHeader::from_user_data(block);
    auto* footer = reinterpret_cast<CanaryFooter*>(
        reinterpret_cast<char*>(header) + size_for_class(SizeClass::k64) - 8);
    EXPECT_EQ(footer->magic, kAllocMagic);  // canary intact
}
```

### GREEN — Implement

1. Add `stamp_boundary_footer()` helper in `slab_cache.cpp`:
   ```cpp
   static void stamp_boundary_footer(AllocHeader* header, size_t block_size) {
       auto* footer = reinterpret_cast<FooterOverlay*>(
           reinterpret_cast<char*>(header) + block_size - sizeof(FooterOverlay));
       footer->boundary.block_size = static_cast<uint32_t>(block_size);
       footer->boundary.flags = kFlagFree;
   }
   ```

2. Call from `deallocate()` when coalescing is enabled:
   ```cpp
   if (enable_coalescing_) {
       stamp_boundary_footer(header, block_size_);
   }
   ```

### REFACTOR — Verify clean

- `stamp_boundary_footer` is a static helper, not a method — keeps hot path clean

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*Coalescing*"
```

---

## Phase 3: Left-Neighbor Coalescing

**Goal:** When freeing block B, if block A (immediately left) is free, merge
A+B into a single larger free block.

### RED — Write failing tests

```cpp
TEST(Coalescing, LeftNeighborMerge) {
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* a = cache.allocate(ActorId{1});
    auto* b = cache.allocate(ActorId{1});

    cache.deallocate(a);  // A is free
    cache.deallocate(b);  // B freed → should coalesce with A

    // After coalescing, A's header should be the merged block
    auto* a_header = AllocHeader::from_user_data(a);
    EXPECT_EQ(a_header->magic, kFreedMagic);
    // Footer at end of B's block should now be at the combined boundary
    auto* combined_footer = reinterpret_cast<FooterOverlay*>(
        reinterpret_cast<char*>(a_header) + 2 * size_for_class(SizeClass::k64) - 8);
    EXPECT_TRUE(combined_footer->boundary.flags & kFlagFree);
}

TEST(Coalescing, LeftNeighborNoMergeWhenLive) {
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* a = cache.allocate(ActorId{1});  // A stays live
    auto* b = cache.allocate(ActorId{1});

    cache.deallocate(b);  // B freed — A is live, no coalescing

    // B's footer should show its own (unmerged) size
    auto* b_header = AllocHeader::from_user_data(b);
    auto* b_footer = reinterpret_cast<FooterOverlay*>(
        reinterpret_cast<char*>(b_header) + size_for_class(SizeClass::k64) - 8);
    EXPECT_EQ(b_footer->boundary.block_size, size_for_class(SizeClass::k64));
}
```

### GREEN — Implement

Add left-neighbor logic to `deallocate()`:
```cpp
void* coalesced_block = header;
size_t coalesced_size = block_size_;

// Left neighbor check
if (reinterpret_cast<char*>(header) > slab_base_) {
    auto* left_footer = reinterpret_cast<FooterOverlay*>(
        reinterpret_cast<char*>(header) - sizeof(FooterOverlay));
    if (left_footer->boundary.flags & kFlagFree) {
        size_t left_size = left_footer->boundary.block_size;
        auto* left_header = reinterpret_cast<AllocHeader*>(
            reinterpret_cast<char*>(header) - left_size);
        // Remove left block from freelist
        remove_from_freelist(left_header);
        coalesced_block = left_header;
        coalesced_size += left_size;
    }
}
```

### REFACTOR — Verify clean

- Left-neighbor footer read is O(1) pointer arithmetic — no loop
- Only one level of coalescing per `deallocate()` call

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*Coalescing*"
```

---

## Phase 4: Right-Neighbor Coalescing

**Goal:** When freeing block B, if block C (immediately right) is free, merge
B+C. Together with Phase 3, this handles A+B+C full coalescing.

### RED — Write failing tests

```cpp
TEST(Coalescing, RightNeighborMerge) {
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* b = cache.allocate(ActorId{1});
    auto* c = cache.allocate(ActorId{1});

    cache.deallocate(c);  // C is free
    cache.deallocate(b);  // B freed → should coalesce with C

    auto* b_header = AllocHeader::from_user_data(b);
    auto* combined_footer = reinterpret_cast<FooterOverlay*>(
        reinterpret_cast<char*>(b_header) + 2 * size_for_class(SizeClass::k64) - 8);
    EXPECT_TRUE(combined_footer->boundary.flags & kFlagFree);
    EXPECT_EQ(combined_footer->boundary.block_size, 2 * size_for_class(SizeClass::k64));
}

TEST(Coalescing, RightNeighborNotCheckedAtSlabEnd) {
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    // Allocate the last block in the slab, free it — right neighbor is past slab_end
    // Fill to near end, then test
    std::vector<void*> blocks;
    while (true) {
        auto* b = cache.allocate(ActorId{1});
        if (!b) break;
        blocks.push_back(b);
    }
    // Free last block — should not read past slab_end
    auto* last = blocks.back();
    cache.deallocate(last);  // Should not crash
    SUCCEED();
}
```

### GREEN — Implement

Add right-neighbor logic:
```cpp
// Right neighbor check
char* right_addr = reinterpret_cast<char*>(header) + block_size_;
if (right_addr < slab_end_) {
    auto* right_header = reinterpret_cast<AllocHeader*>(right_addr);
    if (right_header->magic == kFreedMagic) {
        size_t right_size = size_for_class(right_header->size_class);
        remove_from_freelist(right_header);
        coalesced_size += right_size;
    }
}
```

### REFACTOR — Verify clean

- `slab_end_` pointer comparison guards against out-of-bounds access
- Right neighbor check uses `magic == kFreedMagic` (fast)

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*Coalescing*"
```

---

## Phase 5: Both-Neighbor Coalescing + Boundary Conditions

**Goal:** Full A+B+C coalescing + boundary tests (first block, last block in slab).

### RED — Write failing tests

```cpp
TEST(Coalescing, BothNeighborsMerge) {
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* a = cache.allocate(ActorId{1});
    auto* b = cache.allocate(ActorId{1});
    auto* c = cache.allocate(ActorId{1});

    cache.deallocate(a);  // A free
    cache.deallocate(c);  // C free
    cache.deallocate(b);  // B free → coalesce A+B+C

    auto* a_header = AllocHeader::from_user_data(a);
    auto* abc_footer = reinterpret_cast<FooterOverlay*>(
        reinterpret_cast<char*>(a_header) + 3 * size_for_class(SizeClass::k64) - 8);
    EXPECT_TRUE(abc_footer->boundary.flags & kFlagFree);
    EXPECT_EQ(abc_footer->boundary.block_size, 3 * size_for_class(SizeClass::k64));
}

TEST(Coalescing, FirstBlockNoLeftNeighbor) {
    // First block in slab — left neighbor check skipped safely
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* first = cache.allocate(ActorId{1});
    // first is at slab_base_ — left neighbor would be before slab
    cache.deallocate(first);  // Should not crash, no left coalescing
    SUCCEED();
}

TEST(Coalescing, LastBlockNoRightNeighbor) {
    // Free the last block — right neighbor would be past slab_end
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    std::vector<void*> blocks;
    while (true) {
        auto* b = cache.allocate(ActorId{1});
        if (!b) break;
        blocks.push_back(b);
    }
    auto* last = blocks.back();
    auto* second_last = blocks[blocks.size() - 2];

    cache.deallocate(second_last);
    cache.deallocate(last);  // right neighbor is at slab_end_ → skipped
    SUCCEED();
}
```

### GREEN — Implement

Combine left + right logic from Phases 3–4. The boundary conditions are
already handled:
- Left check: `if (reinterpret_cast<char*>(header) > slab_base_)` — guards
  first block.
- Right check: `if (right_addr < slab_end_)` — guards last block.

After coalescing, stamp the combined boundary footer and push to the
appropriate segregated bin.

### REFACTOR — Verify clean

- Extract `try_coalesce_left()` and `try_coalesce_right()` as private helpers
- Disable coalescing for `SizeClass::k32` (no room for `prev` pointer in
  user data for doubly-linked freelist — see design spec Appendix A)

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*Coalescing*"
```

---

## Phase 6: Freelist `remove()` for Middle-Of-List Extraction

**Goal:** Upgrade the freelist from singly-linked CAS LIFO to a doubly-linked
intrusive list, enabling O(1) removal from the middle (needed when coalescing
absorbs a neighbor that was already in a bin).

### RED — Write failing tests

```cpp
TEST(Coalescing, RemoveFromMiddleOfFreelist) {
    // Setup: push 3 blocks to the same bin freelist
    // Remove the middle one, verify list remains consistent
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* x = cache.allocate(ActorId{1});
    auto* y = cache.allocate(ActorId{1});
    auto* z = cache.allocate(ActorId{1});

    cache.deallocate(x);
    cache.deallocate(y);
    cache.deallocate(z);
    // All three in same bin (or possibly different bins depending on offsets)

    // Allocate 3 times — should get all three back (one may be coalesced)
    auto* r1 = cache.allocate(ActorId{1});
    auto* r2 = cache.allocate(ActorId{1});
    auto* r3 = cache.allocate(ActorId{1});
    EXPECT_NE(r1, nullptr);
    EXPECT_NE(r2, nullptr);
    EXPECT_NE(r3, nullptr);
    // All three distinct
    EXPECT_TRUE(r1 != r2 && r2 != r3 && r1 != r3);
}

TEST(Coalescing, FreeBlockLinkageFitsIn32ByteUserData) {
    // FreeBlockLinkage (next + prev) must fit in minimum user data region
    EXPECT_LE(sizeof(FreeBlockLinkage), 32u - sizeof(AllocHeader));
}

TEST(Coalescing, RemovePreservesListIntegrity) {
    // Push A, B, C to freelist. Remove B. Pop → C, A (order preserved except B gone)
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* a = cache.allocate(ActorId{1});
    auto* b = cache.allocate(ActorId{1});
    auto* c = cache.allocate(ActorId{1});

    // All in same bin if offsets align
    cache.deallocate(a);
    cache.deallocate(b);
    cache.deallocate(c);

    // We can't directly test remove() but we can verify:
    // after coalescing a freed block with neighbors, the freelist is consistent
    std::vector<void*> recovered;
    for (int i = 0; i < 10; ++i) {
        auto* r = cache.allocate(ActorId{1});
        if (r) recovered.push_back(r);
    }
    EXPECT_EQ(recovered.size(), 3u);  // A, B, C all recoverable (B not coalesced)
}
```

### GREEN — Implement

1. Add `FreeBlockLinkage` struct in `slab_cache.hpp`:
   ```cpp
   struct FreeBlockLinkage {
       AllocHeader* next;
       AllocHeader* prev;
   };
   ```

2. Upgrade `adt::FreeList` to support `remove()`:
   ```cpp
   // In adt::FreeList (or a new DoublyLinkedFreeList):
   void remove(AllocHeader* block) {
       auto* linkage = reinterpret_cast<FreeBlockLinkage*>(
           reinterpret_cast<char*>(block) + sizeof(AllocHeader));
       auto* prev = linkage->prev;
       auto* next = linkage->next;

       if (prev) {
           auto* prev_link = reinterpret_cast<FreeBlockLinkage*>(
               reinterpret_cast<char*>(prev) + sizeof(AllocHeader));
           prev_link->next = next;
       } else {
           // block was the head — update head
           head_ = next;
       }

       if (next) {
           auto* next_link = reinterpret_cast<FreeBlockLinkage*>(
               reinterpret_cast<char*>(next) + sizeof(AllocHeader));
           next_link->prev = prev;
       }

       linkage->next = nullptr;
       linkage->prev = nullptr;
   }
   ```

3. Update `push` to set both `next` and `prev`.

**Thread-safety note:** `remove()` is only called from the owning thread
during `deallocate()`. Cross-thread frees are routed to the owning thread via
the existing `SegmentProvider::lookup_slab()` mechanism. No CAS needed.

### REFACTOR — Verify clean

- **Coalescing disabled for `SizeClass::k32`**: 32B user data has no room
  for `FreeBlockLinkage` (16B next+prev). Gate with `static constexpr bool
  kCanCoalesce = block_size_ >= 64;` (i.e., 64B block = 32B user data = 16B
  for linkage + 16B for data).
- Extract `DoublyLinkedFreeList` as a distinct type in `adt/` if the upgrade
  is clean.

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*Coalescing*"
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SlabCache*"
```

---

## Phase 7: Coalescing + Segregated Bins Integration

**Goal:** When coalescing removes a neighbor from a bin, the combined block
is inserted into the correct bin for its new (start) address.

### RED — Write failing tests

```cpp
TEST(Coalescing, CoalescedBlockGoesToCorrectBin) {
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);

    // Allocate/free alternating pattern: A(free), B(live), C(free), D(live)
    auto* a = cache.allocate(ActorId{1});
    auto* b = cache.allocate(ActorId{1});
    auto* c = cache.allocate(ActorId{1});
    auto* d = cache.allocate(ActorId{1});

    cache.deallocate(a);
    cache.deallocate(c);

    // Free B → B coalesces with A (left, free) and C (right, free)
    // Combined A+B+C block should be in bin(a), since A is the leftmost
    cache.deallocate(b);

    // Verify: we can re-allocate from the coalesced region
    auto* r1 = cache.allocate(ActorId{1});
    auto* r2 = cache.allocate(ActorId{1});
    auto* r3 = cache.allocate(ActorId{1});
    EXPECT_NE(r1, nullptr);
    EXPECT_NE(r2, nullptr);
    EXPECT_NE(r3, nullptr);
}

TEST(Coalescing, FullSlabCoalescesToOneBlock) {
    // Free all blocks in a slab — should coalesce into a single block
    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    std::vector<void*> blocks;
    for (int i = 0; i < 100; ++i) {
        blocks.push_back(cache.allocate(ActorId{1}));
    }
    for (auto* b : blocks) cache.deallocate(b);

    // All blocks freed and coalesced — entire slab should be one free block
    // Next allocate should succeed from that coalesced block
    auto* r = cache.allocate(ActorId{1});
    EXPECT_NE(r, nullptr);
}
```

### GREEN — Implement

On coalescing completion, push the coalesced block to the correct bin:
```cpp
// After coalescing:
stamp_boundary_footer(coalesced_block, coalesced_size);
uint8_t bin = compute_bin_index(coalesced_block);
freelist_.bins[bin].push_front(coalesced_block);
```

### REFACTOR — Verify clean

- `compute_bin_index()` was implemented in MEM-001, reused here
- Combined block always goes to the bin matching its start address

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*Coalescing*"
```

---

## Phase 8: Fault Injection

**Goal:** Wire `FAULT_INJECT` sites for coalescing error paths.

### RED — Write failing tests

```cpp
TEST(Coalescing, FaultInjectionBoundaryTagCorrupt) {
    FaultSchedule::Builder()
        .at("hpactor.allocator.coalesce.boundary_tag_corrupt",
            FaultAction::Corrupt)
        .build_and_install();
    FaultController::instance().enable();

    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* a = cache.allocate(ActorId{1});
    auto* b = cache.allocate(ActorId{1});
    cache.deallocate(a);
    // Fault corrupts A's boundary footer — B sees "free" neighbor that may
    // be corrupt
    cache.deallocate(b);  // Should handle corruption gracefully
    FaultController::instance().disable();
}

TEST(Coalescing, FaultInjectionRemoveFromListFail) {
    FaultSchedule::Builder()
        .at("hpactor.allocator.coalesce.remove_from_list_fail",
            FaultAction::Fail)
        .build_and_install();
    FaultController::instance().enable();

    SlabCache cache(SizeClass::k64, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit, /*coalescing=*/true);
    auto* a = cache.allocate(ActorId{1});
    auto* b = cache.allocate(ActorId{1});
    cache.deallocate(a);
    cache.deallocate(b);  // remove(a) from freelist fails — skip coalescing
    FaultController::instance().disable();
}
```

### GREEN — Implement

Wire fault points in `coalesce_left()` / `coalesce_right()`:
```cpp
FAULT_INJECT("hpactor.allocator.coalesce.boundary_tag_corrupt",
             left_footer->boundary.flags = kFlagFree | 0xFE);
FAULT_INJECT("hpactor.allocator.coalesce.remove_from_list_fail",
             return /* skip coalescing */);
```

Register in `src/fault/fault_points.cpp`.

### REFACTOR — Verify clean

- Fault injection is cold-branch (`HPACTOR_UNLIKELY`)
- Compiles out when `ENABLE_FAULT_INJECTION=OFF`

### Verification:
```bash
cmake -S . -B build -DENABLE_FAULT_INJECTION=ON
ninja -C build test_unit_mem test_unit_fault
./build/tests/unit/mem/test_unit_mem --gtest_filter="*Coalescing*"
```

---

## Files Changed Summary

| File | Phase | Change |
|------|-------|--------|
| `include/hpactor/mem/alloc_header.hpp` | 1 | `BoundaryFooter`, `FooterOverlay`, `kFlagFree` |
| `include/hpactor/mem/slab_cache.hpp` | 1–7 | `FreeBlockLinkage`, coalescing flag, `enable_coalescing_`, `try_coalesce_left/right()`, `remove_from_freelist()` |
| `src/mem/slab_cache.cpp` | 2–7 | Stamp footer on free, coalescing logic in deallocate, combined block bin insertion |
| `include/hpactor/mem/thread_local_allocator.hpp` | 1 | Pass `enable_coalescing` to SlabCache constructor |
| `src/mem/thread_local_allocator.cpp` | 1 | Coalescing config per region |
| `src/fault/fault_points.cpp` | 8 | Register 2 new fault points |
| `tests/unit/mem/test_free_block_coalescing.cpp` | 1–8 | **New file** — 16 test cases |
| `tests/unit/mem/CMakeLists.txt` | 1 | Add `test_free_block_coalescing` target |

## Verification Checklist

```bash
# Unit tests
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*Coalescing*"
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"

# All existing memory tests — zero regression
ctest -R "test_unit_mem" --output-on-failure --parallel 8

# TSan
cmake -S . -B build-tsan -DENABLE_TSAN=ON
ninja -C build-tsan test_unit_mem
ctest -R "test_unit_mem" --output-on-failure -j4

# Fragmentation simulation
./build/tests/unit/mem/test_unit_mem --gtest_filter="*Coalescing*"
# Manual verification: fragmentation <2% after 100K random alloc/free cycles
```
