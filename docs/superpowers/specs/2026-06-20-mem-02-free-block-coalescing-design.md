# MEM-002: Free Block Coalescing with Boundary Tags — Design Spec

**Date:** 2026-06-20
**Branch:** (future) `feature/mem-002-free-block-coalescing`
**Issue:** #339 (Phase 1.2, P0)
**Parent Doc:** `docs/architecture/memory/memory-management-erlang-gap-analysis.md`
**Depends on:** MEM-001 (Segregated Free Lists)

## 1. Motivation

### 1.1 Problem

HPActor's `SlabCache` pushes each freed block independently onto a freelist.
Adjacent freed blocks remain as separate entries. When the freelist is LIFO
(current behavior), the most-recently-freed blocks are always reallocated first,
but they may be scattered anywhere in the slab. The result:

- **Checkerboard fragmentation:** After thousands of alloc/free cycles, a slab
  contains interleaved live and free blocks. Live blocks pin slabs — even 99%
  free space cannot be returned to `SegmentProvider` if one live block remains.

- **Compaction as sole recovery:** The `CompactionManager` (25% threshold, 5%
  budget) is the **only** mechanism for recovering contiguous free space.
  Compaction is stop-the-world per slab and involves memcpy of live blocks
  plus actor registry pointer updates.

- **Monotonic fragmentation growth:** Between compaction cycles (default 60s
  interval), fragmentation only increases. Under steady-state churn, the
  system oscillates between growing fragmentation and disruptive compaction.

### 1.2 Erlang Reference

Erlang uses **boundary tags** — a header and footer on every free block
containing the block size. When freeing block B:

1. Read the footer of the block immediately before B (at address B - footer_size).
2. If it's free → coalesce A + B into AB.
3. Read the header of the block immediately after B (at address B + B.size).
4. If it's free → coalesce AB + C into ABC.

Both checks are O(1) pointer arithmetic. The result: checkerboard patterns
self-heal immediately. Fragmentation stays below 2% organically.

### 1.3 Expected Impact

| Metric | Before (LIFO, no coalescing) | After (segregated + coalescing) | Delta |
|--------|------------------------------|--------------------------------|-------|
| Compaction frequency | 2–4/day | 0–1/day | -60–80% |
| Internal fragmentation after 7-day uptime | 5% (design target) | <2% | -60% |
| `deallocate()` hot path | <20 ns | <25 ns | +25% (acceptable trade) |
| Fragmentation self-healing | None (monotonic) | Immediate coalescing | Qualitative leap |

---

## 2. Design Goals

1. **Immediate coalescing** — adjacent free blocks merge on `deallocate()`, not
   during a separate compaction pass.
2. **Zero additional memory overhead** — repurpose the `CanaryFooter` slot on
   freed blocks as the boundary tag footer. Live blocks keep their canary.
3. **O(1) deallocation** — at most two neighbor checks (left footer, right
   header), each a constant-time pointer dereference.
4. **Works with segregated free lists** — when a coalesced block size changes
   (merging), it must be removed from its original bin and re-inserted into
   the correct bin for the new combined size. For same-size-class slabs, the
   bin doesn't change but the block must be removed from the middle of the
   freelist.
5. **Opt-in per region** — coalescing is enabled only for regions where
   fragmentation matters (`kActor`); disabled for regions where throughput
   dominates (`kMessage`, `kNetwork`).

---

## 3. Design

### 3.1 Boundary Tag Layout on Free Blocks

When a block is live, its layout is unchanged:

```
┌────────────────────────────────────────────────────┐
│  AllocHeader (32B)  │  user data  │  CanaryFooter (8B)  │
│  magic = kAllocMagic │             │  magic = kAllocMagic │
└────────────────────────────────────────────────────┘
```

When a block is freed, the `CanaryFooter` slot is repurposed as a **boundary
footer** storing the block's total size. The `AllocHeader` already stores the
size class (and thus block size), but the footer provides O(1) access from the
left neighbor without walking backward through the header:

```
┌────────────────────────────────────────────────────┐
│  AllocHeader (32B)  │  unused     │  BoundaryFooter (8B)  │
│  magic = kFreedMagic │  (poisoned) │  block_size (4B)      │
│                     │             │  flags (1B): IS_FREE  │
│                     │             │  padding (3B)         │
└────────────────────────────────────────────────────┘
```

**CanaryFooter overlay (in freed blocks):**

```cpp
// Reinterpret-cast-safe: sizeof(CanaryFooter) == sizeof(BoundaryFooter) == 8
union FooterOverlay {
    CanaryFooter   canary;    // when block is live
    BoundaryFooter boundary;  // when block is freed
};

struct BoundaryFooter {
    uint32_t block_size;   // total block size (header + user + footer)
    uint8_t  flags;        // kFlagFree = 0x01
    uint8_t  padding[3];
};
static_assert(sizeof(BoundaryFooter) == 8);
```

### 3.2 Coalescing Algorithm

```
deallocate(ptr):
    header = AllocHeader::from_user_data(ptr)

    // Corruption checks (unchanged)
    if (header->magic != kAllocMagic) → corruption_panic()

    block_size = size_for_class(header->size_class)

    // Poison user data (debug, unchanged)
    if constexpr (debug) memset(ptr, kPoisonByte, user_size)

    // --- Coalescing logic ---

    // Mark this block as free
    header->magic = kFreedMagic
    stamp_boundary_footer(header, block_size)  // overwrite canary with size

    coalesced_start = header
    coalesced_size  = block_size

    // Check left neighbor
    if (header is not at slab start)
        left_footer = (BoundaryFooter*)((char*)header - sizeof(BoundaryFooter))
        if (left_footer->flags & kFlagFree)
            left_size = left_footer->block_size
            left_header = (AllocHeader*)((char*)header - left_size)
            → remove_from_freelist(left_header)   // O(1) with doubly-linked freelist
            coalesced_start = left_header
            coalesced_size += left_size

    // Check right neighbor
    right_header_addr = (char*)header + block_size
    if (right_header_addr < slab_end)
        right_header = (AllocHeader*)right_header_addr
        if (right_header->magic == kFreedMagic)
            right_size = size_for_class(right_header->size_class)
            → remove_from_freelist(right_header)
            coalesced_size += right_size

    // Stamp the coalesced block
    coalesced_start->magic = kFreedMagic
    // Update boundary footer at end of coalesced block
    stamp_boundary_footer(coalesced_start, coalesced_size)

    // Insert into segregated free list
    bin_idx = (offset_in_slab(coalesced_start) / bin_stride) % N
    bins_[bin_idx].push(coalesced_start)
```

### 3.3 Removing from Middle of Freelist

The key enabling mechanism is the ability to remove a block from the *middle*
of a freelist (when coalescing). The current `adt::FreeList` is a Treiber
stack — CAS LIFO, only supports pop-from-front.

**Solution:** Upgrade the freelist to a **doubly-linked intrusive list** within
the free blocks themselves. The `AllocHeader` already has a `next` pointer used
by the freelist; we add a `prev` pointer using the freed block's user data
region (which is unused and poisoned):

```cpp
// Free-block internal linkage (stored in the user data region)
struct FreeBlockLinkage {
    AllocHeader* next;  // [header + 0]  — reuses AllocHeader::next
    AllocHeader* prev;  // [header + 8]  — NEW, stored in first 8B of user data
};
static_assert(sizeof(FreeBlockLinkage) <= 32);  // fits in minimum block user data
```

This approach has **zero memory overhead** because:
- `AllocHeader::next` already exists (union with `owner_id`).
- `prev` is stored in the freed block's user data, which is unused and poisoned
  anyway.

The freelist operations become:

```cpp
// Push to front (CAS, O(1)):
void push(AllocHeader* block):
    block->next = head_.load(relaxed)
    if (block->next) block->next->prev = block
    while (!head_.compare_exchange_weak(block->next, block, acq_rel))

// Remove from middle (lock-free, O(1)):
void remove(AllocHeader* block):
    // CAS loop: unlink from doubly-linked list
    while (true):
        prev = block->prev
        next = block->next
        if (prev) prev->next = next
        if (next) next->prev = prev
        // If block was head, CAS head to next
        if (head_ == block)
            if (!head_.compare_exchange_weak(block, next, acq_rel))
                continue  // head changed, retry
        break
    block->next = nullptr
    block->prev = nullptr
```

**Concurrency note:** `remove()` is only called from the owning thread during
`deallocate()` — the same thread that owns the `SlabCache`. Cross-thread frees
are routed to the origin thread's cache via `SegmentProvider::lookup_slab()`,
which returns the slab but does NOT touch the freelist directly — the freing
thread pushes to a cross-thread queue, and the owning thread drains it. So
`remove()` is single-threaded and needs no CAS.

**Simplification:** Since `deallocate()` is always on the owning thread (after
cross-thread routing), the freelist can be a simple doubly-linked list with no
CAS — just `push_front()`, `pop_front()`, and `remove(block)`. This eliminates
the lock-free complexity entirely and enables O(1) middle removal.

### 3.4 Integration with Segregated Free Lists

With segregated free lists (MEM-001), each bin is a doubly-linked freelist.
The coalescing logic is bin-aware:

```
// Coalescing removes from whatever bin the neighbor is in:
remove_from_freelist(block):
    bin_idx = (offset_in_slab(block) / bin_stride) % N
    bins_[bin_idx].remove(block)
```

When the coalesced block is re-inserted, it goes into the bin corresponding
to its new start address (the leftmost block's address):

```
bin_idx = (offset_in_slab(coalesced_start) / bin_stride) % N
bins_[bin_idx].push_front(coalesced_start)
```

### 3.5 Configuration

Coalescing is per-region, enabled via the strategy configuration introduced
in MEM-003:

```cpp
struct RegionStrategyConfig {
    AllocationStrategy strategy;     // kCasLifo | kSegregatedFit
    bool enable_coalescing;          // default: false
    uint8_t max_coalesce_depth;      // default: 2 (left + right)
};
```

Enabled by default for `kActor`, disabled for `kMessage`, `kNetwork`, `kCoroutine`.

### 3.6 Fault Injection

| Site | Domain | Action | Purpose |
|------|--------|--------|---------|
| `hpactor.allocator.coalesce.boundary_tag_corrupt` | Allocator | Corrupt | Simulate incorrect boundary footer — appears free when it's not |
| `hpactor.allocator.coalesce.remove_from_list_fail` | Allocator | Fail | Simulate failure to remove from freelist (triggers defensive assertion) |

---

## 4. Implementation Plan

### 4.1 TDDFlow Sequence

| Step | Test | Implementation |
|------|------|----------------|
| 1 | `BoundaryFooterStampRead` | `BoundaryFooter` struct, `stamp_boundary_footer()`, `read_boundary_footer()` |
| 2 | `CoalesceLeftNeighbor` — free block B when A (left) is free | Left-neighbor coalescing: footer read + freelist remove + merge |
| 3 | `CoalesceRightNeighbor` — free block B when C (right) is free | Right-neighbor coalescing: header magic check + freelist remove + merge |
| 4 | `CoalesceBothNeighbors` — free middle block, A and C both free | Both-neighbor coalescing: merge A+B+C |
| 5 | `CoalesceBoundaryNoLeft` — free first block in slab (no left neighbor) | Boundary condition: no left footer read |
| 6 | `CoalesceBoundaryNoRight` — free last block in slab (no right neighbor) | Boundary condition: no right header check |
| 7 | `CoalesceWithSegregatedBins` — coalescing maintains correct bin placement | Segregated bin integration: remove from correct bin, insert into correct bin |
| 8 | `CoalesceSlabFullFree` — free all blocks, verify single coalesced block | Full-slab coalescing: entire slab becomes one free block |
| 9 | `CoalesceFaultInjection` — boundary tag corruption detected | Fault injection sites |

### 4.2 Files Changed

| File | Change |
|------|--------|
| `include/hpactor/mem/alloc_header.hpp` | Add `BoundaryFooter` struct, `FooterOverlay` union |
| `include/hpactor/mem/slab_cache.hpp` | Add `FreeBlockLinkage` struct, upgrade freelist to doubly-linked, add coalescing logic to `deallocate()` |
| `src/mem/slab_cache.cpp` | Implement coalescing algorithm, `remove_from_freelist()` |
| `include/hpactor/mem/memory_config.hpp` | Add `enable_coalescing` to `RegionStrategyConfig` |
| `tests/unit/mem/test_free_block_coalescing.cpp` | **New file** — 9 test cases |

---

## 5. Testing Strategy

### 5.1 Unit Tests

| Test | What It Validates |
|------|-------------------|
| `BoundaryFooterStampRead` | Footer stamped on free, read correctly on left-neighbor check |
| `CoalesceLeftNeighbor` | Left coalesce: A(32B) + B(32B) → AB(64B logically, still 2×32B blocks in same size class) |
| `CoalesceRightNeighbor` | Right coalesce: B(32B) + C(32B) |
| `CoalesceBothNeighbors` | Full coalesce: A + B + C |
| `CoalesceBoundaryNoLeft` | Slab-first block: no left footer read (no crash) |
| `CoalesceBoundaryNoRight` | Slab-last block: no right header check (no crash) |
| `CoalesceWithSegregatedBins` | Coalesced block inserted into correct bin |
| `CoalesceSlabFullFree` | All blocks freed → single coalesced block covering entire slab |
| `CoalesceFaultInjection` | Fault sites trigger correctly; corruption detected |

### 5.2 Determinism Guarantees

- All coalescing tests use single-threaded scheduler (`scheduler_threads = 0`).
- Tests use `mailbox->inject_for_test()` pattern for message-based allocation.
- No timing assumptions.

---

## 6. Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| `prev` pointer in user data conflicts with small blocks (32B user data = 0 bytes after 32B header) | **High** | For 32B size class: skip coalescing (can't fit `prev` pointer). Fragmentation impact minimal — 32B blocks are typically messages, not long-lived. Coalescing only for size classes ≥ 64B block size (≥ 32B user data). |
| `remove()` from middle is not lock-free for cross-thread frees | Medium | Cross-thread frees are already routed to owning thread via cross-thread queue; `remove()` only called from owning thread. Document this invariant. |
| Coalescing adds O(1) overhead to every `deallocate()` for `kActor` | Low | +5ns (two pointer dereferences + two magic checks). Acceptable for long-lived actor allocations. Disabled for `kMessage` (µs-lived). |
| Boundary footer corruption from buffer underflow (write before block start) | Low | Guard pages for fat blocks catch this; for regular blocks, the canary on the *previous* block would detect overflow into the footer region. This risk exists today and is unchanged. |

---

## 7. Acceptance Criteria

1. All existing memory tests pass with coalescing disabled (default).
2. Nine new coalescing test cases pass.
3. `test_memory_stress` passes with coalescing enabled for `kActor` region.
4. Fragmentation simulation: 100K random alloc/free cycles on `kActor` region:
   - With coalescing: <2% internal fragmentation.
   - Without coalescing (baseline): 5–8% internal fragmentation.
5. Coalescing depth never exceeds 2 (left + right) — no cascading coalescing.
6. TSan-clean on all tests.
7. No measurable throughput regression in `test_allocator_benchmark` for
   `kMessage` and `kNetwork` regions (coalescing disabled).

---

## Appendix A: 32B Block Limitation

The 32B size class (total block 72B: 32B header + 32B user data + 8B footer)
has zero usable bytes in the freed user data region for `prev` pointer storage
(32B user data → need 8B for `prev`, leaving 24B unused). Options:

1. **Skip coalescing for 32B blocks.** Acceptable because 32B blocks are
   typically messages with µs-scale lifetimes — they don't cause long-term
   fragmentation.
2. **Store `prev` as a 4-byte offset from slab base** instead of a full 8-byte
   pointer. Fits in 32B user data. Increases `remove()` cost (pointer
   reconstruction) but enables coalescing for all size classes.

**Recommendation:** Option 1 (skip for 32B). Revisit if production profiling
shows 32B-size-class fragmentation as a bottleneck.
