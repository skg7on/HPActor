# MEM-001: Segregated Free Lists (Good Fit Strategy) — Design Spec

**Date:** 2026-06-20
**Branch:** (future) `feature/mem-001-segregated-free-lists`
**Issue:** #339 (Phase 1.1, P0)
**Parent Doc:** `docs/architecture/memory/memory-management-erlang-gap-analysis.md`

## 1. Motivation

### 1.1 Problem

HPActor's `SlabCache` uses a single CAS LIFO freelist for recycled blocks
within each slab. While this provides O(1) push/pop with minimal cache-line
contention, it has no notion of block address ordering:

- **No size awareness:** Within a single size class, all blocks are the same
  size, so the LIFO list is not strictly wrong — every block satisfies every
  request. But LIFO means a block freed from slab position X is always
  reallocated next, even if blocks at positions more "contiguous" with the live
  set would yield better bump-pointer locality in adjacent slabs.

- **No search bounding:** The CAS pop always returns the most-recently-freed
  block. Under heavy churn, freed blocks near the end of the slab (close to the
  bump pointer) are recycled preferentially, leaving earlier slab regions
  sparsely populated. When a compaction cycle runs, it finds many slabs with
  scattered live blocks rather than contiguous live regions.

- **Fragmentation amplification:** Each compaction cycle is stop-the-world per
  slab. Under the LIFO strategy, slabs reach 25% utilization faster because
  freed blocks cluster at the end of the slab rather than distributing
  uniformly. This increases compaction frequency.

### 1.2 Erlang Reference

Erlang's default `gf` (Good Fit) strategy places free blocks into **segregated
free lists** — bins based on address ranges within a carrier. When allocating,
only the bins that could satisfy the request are searched, bounded to depth 3.
This approximates best-fit at O(1) cost while maintaining address-order
allocation within each bin.

### 1.3 Expected Impact

| Metric | Before | After | Delta |
|--------|--------|-------|-------|
| Internal fragmentation under churn | 5% (compaction-bounded) | 2–3% (self-healing) | -40–60% |
| Compaction frequency | 2–4/day | 0.5–1/day | -60–75% |
| `allocate()` freelist path | <25 ns | <20 ns | -20% (fewer CAS on average) |
| Code complexity | Low | Medium (+300–400 lines) | Acceptable |

---

## 2. Design Goals

1. **Reduce internal fragmentation** — target <3% without compaction.
2. **Preserve hot-path latency** — `allocate()` freelist path ≤ 20 ns (p99.99).
3. **No additional memory overhead** — reuse existing `AllocHeader` fields; no
   per-block size increase.
4. **Opt-in per region** — the single CAS LIFO remains the default; segregated
   lists are enabled per-`SlabCache` via a strategy enum.
5. **All existing tests pass** — no regression in allocation correctness,
   canary verification, poisoning, or cross-thread free routing.

---

## 3. Design

### 3.1 Segregated Free List Structure

Within each slab, free blocks are distributed across **N bins** based on their
**byte offset** within the slab. Each bin is a CAS LIFO freelist (reusing the
existing `adt::FreeList` type).

```
┌────────────────────────────────────────────────────────────────┐
│  Slab (e.g., 64KB for 32B blocks)                              │
│                                                                │
│  Bin 0        Bin 1        Bin 2        ...        Bin N-1    │
│  [0KB–8KB)   [8KB–16KB)   [16KB–24KB)            [56KB–64KB) │
│  ┌────┐       ┌────┐       ┌────┐                  ┌────┐     │
│  │free│       │free│       │free│                  │free│     │
│  │list│       │list│       │list│                  │list│     │
│  └────┘       └────┘       └────┘                  └────┘     │
│                                                                │
│  Each bin: CAS LIFO freelist of AllocHeader*                   │
│  Bin index = (block_offset / bin_stride) % N                   │
└────────────────────────────────────────────────────────────────┘
```

**Constants:**

```cpp
// Number of segregated bins per slab
static constexpr uint8_t kNumSegregatedBins = 8;

// Search depth per bin before falling to next bin
static constexpr uint8_t kMaxSearchDepthPerBin = 3;
```

Bin stride is computed at slab creation:
```cpp
bin_stride_bytes_ = (slab_size_bytes + kNumSegregatedBins - 1) / kNumSegregatedBins;
```

### 3.2 Allocation Algorithm

```
allocate(owner_id):
    // 1. Try bump allocation first (fastest path, unchanged)
    if (bump_ptr < slab_end)
        → bump_allocate(owner_id)

    // 2. Try segregated free lists — round-robin search
    for bin = start_bin to start_bin + kNumSegregatedBins:
        depth = 0
        while (block = bins[bin % N].try_pop())
            if (debug) verify_canary(block)
            → stamp_header(block, owner_id)
            start_bin = (bin + 1) % N   // rotate for next alloc
            return block
            depth++
            if (depth >= kMaxSearchDepthPerBin)
                break  // move to next bin

    // 3. All bins exhausted — refill from SegmentProvider
    → refill_slab()
    goto 1
```

Key properties:
- **Round-robin starting bin (`start_bin`)** prevents all allocations from
  clustering in low-address bins.
- **Depth-bounded per bin (3)** prevents long traversals when a bin has many
  free blocks — we prefer to move to the next bin and maintain address diversity.
- **Bump allocation is still the first path** — virgin memory is fastest and
  provides the best locality.

### 3.3 Deallocation Algorithm

```
deallocate(ptr):
    header = AllocHeader::from_user_data(ptr)

    // Corruption checks (unchanged)
    if (header->magic != kAllocMagic) → corruption_panic()
    footer = CanaryFooter::from_block(header)
    if (footer->magic != kAllocMagic) → overflow_panic()

    // Poison (debug builds, unchanged)
    if constexpr (debug) memset(ptr, kPoisonByte, user_size)

    header->magic = kFreedMagic

    // Compute bin index from block offset within slab
    offset = block_offset_in_slab(header)
    bin_idx = (offset / bin_stride_bytes_) % kNumSegregatedBins

    // Push to the appropriate bin
    bins_[bin_idx].push(header)
```

### 3.4 SlabCache Changes

The `SlabCache` class gains a strategy discriminator:

```cpp
enum class AllocationStrategy : uint8_t {
    kCasLifo = 0,          // Current behavior (default, backward-compatible)
    kSegregatedFit = 1,    // New segregated free lists
};

class SlabCache {
    // ... existing members ...

    // NEW: per-strategy free list storage
    union {
        adt::FreeList single_freelist_;                    // kCasLifo
        struct {
            adt::FreeList bins_[kNumSegregatedBins];      // kSegregatedFit
            uint8_t start_bin_;                            // round-robin cursor
            uint32_t bin_stride_bytes_;                    // bytes per bin
        } segregated_;
    };

    AllocationStrategy strategy_{AllocationStrategy::kCasLifo};

    // Set strategy at construction time (immutable after construction)
    void set_strategy(AllocationStrategy s);

    // Deallocate now routes to the correct bin
    void deallocate(void* ptr);

    // Stats per strategy for observability
    struct SegregatedStats {
        uint64_t bin_hits[kNumSegregatedBins];
        uint64_t bin_misses;           // all bins exhausted
        uint64_t bump_allocs;          // allocated from virgin region
    };
};
```

### 3.5 Memory Layout Constraint

The union between `single_freelist_` and `segregated_` is valid because:
1. Strategy is immutable after `SlabCache` construction.
2. Each `adt::FreeList` is a single pointer (lock-free stack head).
3. Total size: `sizeof(adt::FreeList) * 8 + 8 + 4` ≈ 76 bytes for segregated,
   vs 8 bytes for single. This is per-`SlabCache` (48 instances per thread),
   not per allocation — negligible.

### 3.6 Configuration

Per-region strategy selection (wired in MEM-003, designed here for forward
compatibility):

```cpp
// Default strategy per region (in memory_config.hpp)
constexpr AllocationStrategy kDefaultStrategy =
    AllocationStrategy::kCasLifo;  // opt-in

// Override per-RegionType (zero-cost when not used)
struct RegionStrategyConfig {
    AllocationStrategy strategy;
};

// Example future config (from TOML or compile-time):
//   [system.memory.regions.actor]
//   strategy = "segregated_fit"
```

### 3.7 Fault Injection

New fault injection sites:

| Site | Domain | Action | Purpose |
|------|--------|--------|---------|
| `hpactor.allocator.segregated.bin_pop.corrupt` | Allocator | Corrupt | Simulate bin corruption — return block with incorrect magic |
| `hpactor.allocator.segregated.bin_empty` | Allocator | Fail | Simulate all bins appearing empty (forces refill) |

### 3.8 Observability

Extend `SlabCache::Stats`:

```cpp
struct Stats {
    // ... existing fields ...
    // NEW:
    std::atomic<uint64_t> segregated_bump_allocs{0};
    std::atomic<uint64_t> segregated_bin_allocs{0};
    std::atomic<uint64_t> segregated_bin_misses{0};  // refill needed
    // Per-bin hit counters (for tuning number of bins and search depth)
    std::atomic<uint64_t> bin_alloc_counts[kNumSegregatedBins]{};
};
```

CLI visibility: extend `/memory slabs` to show strategy per cache and per-bin
depth distribution.

---

## 4. Implementation Plan

### 4.1 TDDFlow Sequence

Each step follows RED → GREEN → REFACTOR.

| Step | Test | Implementation |
|------|------|----------------|
| 1 | `test_segregated_freelist.cpp` — `SegregatedFreeListPushPop` | `SegregatedFreeList` data structure (array of `adt::FreeList` + bin index computation) |
| 2 | `SegregatedFreeListRoundRobin` — verifies round-robin bin rotation | Round-robin `start_bin_` cursor |
| 3 | `SegregatedFreeListBoundedDepth` — depth=3 bound is honored | Per-bin search depth counter |
| 4 | `SlabCacheSegregatedAllocate` — allocate from segregated cache | Integrate into `SlabCache::allocate()` with strategy dispatch |
| 5 | `SlabCacheSegregatedDeallocate` — free routes to correct bin | `SlabCache::deallocate()` bin computation |
| 6 | `SlabCacheSegregatedRefill` — bump+segregated hybrid | Refill integration (bump first, then segregated, then refill) |
| 7 | `SegregatedFreeListFaultInjection` — fault injection sites | Wire `FAULT_INJECT` points |
| 8 | `SegregatedFreeListConcurrent` — 4-thread churn stress test | Thread safety validation |

### 4.2 Files Changed

| File | Change |
|------|--------|
| `include/hpactor/mem/slab_cache.hpp` | Add `AllocationStrategy` enum, segregated bin storage, strategy dispatch in `allocate()`/`deallocate()` |
| `src/mem/slab_cache.cpp` | Implement segregated allocation/deallocation paths |
| `include/hpactor/mem/thread_local_allocator.hpp` | Pass strategy to `SlabCache` constructor |
| `src/mem/thread_local_allocator.cpp` | Per-region strategy configuration |
| `include/hpactor/mem/memory_config.hpp` | Add `RegionStrategyConfig` struct, default strategies |
| `tests/unit/mem/test_segregated_freelist.cpp` | **New file** — 8 test cases (see 4.1) |
| `tests/unit/mem/test_slab_cache.cpp` | Extend with segregated strategy test cases |
| `tests/unit/mem/CMakeLists.txt` | Add `test_segregated_freelist` target |

### 4.3 Implementation Constraints

- **No RTTI, no exceptions.** Strategy dispatch uses compile-time or
  construction-time enum, not `dynamic_cast` or `typeid`.
- **No memory overhead for default strategy.** The `single_freelist_` path is
  identical to current behavior.
- **Backward compatible.** Existing tests pass without modification — default
  `kCasLifo` strategy is unchanged.
- **Bounded capacity.** Bin count and search depth are compile-time constants.
  No dynamic allocation in the segregated path.

---

## 5. Testing Strategy

### 5.1 Unit Tests

| Test | What It Validates |
|------|-------------------|
| `SegregatedFreeListPushPop` | Push to bin, pop from bin, LIFO within bin, correct bin routing |
| `SegregatedFreeListRoundRobin` | `start_bin_` advances correctly, all bins visited before cycle repeat |
| `SegregatedFreeListBoundedDepth` | Per-bin pop stops at depth=3, falls to next bin |
| `SlabCacheSegregatedAllocate` | Bump → segregated → refill path; verify strategy dispatch |
| `SlabCacheSegregatedDeallocate` | Free routes to correct bin, cross-thread free still works |
| `SlabCacheSegregatedRefill` | Exhaustion triggers refill; new slab inherits strategy |
| `SegregatedFreeListFaultInjection` | Fault sites trigger correctly; corruption detected |
| `SegregatedFreeListConcurrent` | 4-thread churn (10K ops each), TSan-clean |

### 5.2 Integration Tests

| Test | What It Validates |
|------|-------------------|
| `MemSegregatedActorAlloc` | Actors allocate/free through segregated path; no corruption |
| `MemSegregatedMessageAlloc` | Message lifecycle through segregated path; canary intact |

### 5.3 Performance Tests

| Test | What It Measures |
|------|-----------------|
| `PerfSegregatedAllocThroughput` | 1M allocations across segregated vs LIFO; report p50/p99/p999 |
| `PerfSegregatedFragmentation` | Simulated 7-day churn; compare final fragmentation vs LIFO |

### 5.4 Determinism Guarantees

- All tests use `scheduler_threads = 0` (single-threaded scheduler) where
  possible.
- Concurrent tests use condition-based polling with 5s+ timeouts.
- No timing assumptions; fragmentation tests use deterministic sequences.

---

## 6. Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| Cache miss increase from address diversity (LIFO gives cache-hot reuse) | Medium | Default `kCasLifo` for `kMessage` (µs-lived, cache locality matters); segregated only for `kActor` |
| Bin stride misalignment with allocation pattern | Low | Stride computed from actual slab size at construction; tunable `kNumSegregatedBins` |
| CAS contention on popular bins under high concurrency | Low | Round-robin start bin distributes load; per-bin CAS is same contention model as current single list |
| Debug build overhead from canary verification on every bin pop | Low | Canary verify only on the block returned (once), not on every bin search |
| Union complexity confusing static analyzers | Low | Document the invariant; `static_assert` on union size ≤ single_freelist size for default path |

---

## 7. Acceptance Criteria

1. All existing memory tests (19 files) pass with default `kCasLifo` strategy.
2. New segregated free list tests (8 test cases) pass.
3. `test_memory_stress` (8-thread concurrent alloc/free) passes with
   `kSegregatedFit` enabled for `kActor` region.
4. Fragmentation simulation: after 100K alloc/free cycles with random free
   patterns, internal fragmentation ≤ 3% with segregated vs baseline.
5. Allocation latency p99.99 ≤ 20 ns for freelist path (measured via rdtsc in
   benchmark test).
6. TSan-clean on all concurrent tests.
7. ASan-clean on all tests (with `detect_container_overflow=0` for
   canary-footer false positive suppression).

---

## Appendix A: Configuration Knobs

```cpp
// Compile-time constants (in slab_cache.hpp)
static constexpr uint8_t kNumSegregatedBins = 8;
static constexpr uint8_t kMaxSearchDepthPerBin = 3;

// Runtime configuration (per region, future TOML)
// [system.memory.regions.actor]
// strategy = "segregated_fit"  # or "cas_lifo" (default)
```

## Appendix B: Open Questions

1. **Should bin count be per-size-class?** Smaller size classes (32B) have many
   more blocks per slab (2048) than large classes (4KB → 128). 8 bins at 4KB
   means 16 blocks/bin — search depth 3 would cover 19%. At 32B, 8 bins means
   256 blocks/bin — search depth 3 covers 1.2%. Answer: 8 bins is likely
   sufficient for all size classes, but should be validated with benchmarks.

2. **Should bins be sorted by address?** Currently bins are address-ordered
   (bin 0 = lowest addresses). The round-robin start prevents bias toward low
   addresses. Sorting within bins would add overhead. Decision: LIFO within
   bin (no sorting) for now; measure fragmentation impact.
