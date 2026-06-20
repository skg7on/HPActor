# MEM-003: Per-Region Allocation Strategy Selection — Design Spec

**Date:** 2026-06-20
**Branch:** (future) `feature/mem-003-per-region-strategy`
**Issue:** #339 (Phase 1.3, P1)
**Parent Doc:** `docs/architecture/memory/memory-management-erlang-gap-analysis.md`
**Depends on:** MEM-001 (Segregated Free Lists), MEM-002 (Free Block Coalescing)

## 1. Motivation

### 1.1 Problem

All six HPActor memory regions (`kActor`, `kMessage`, `kCoroutine`, `kNetwork`,
`kInternal`, `kHibernate`) use the identical allocation strategy: bump
allocation for virgin memory + CAS LIFO freelist for recycled blocks. This
one-size-fits-all approach is suboptimal because regions have radically
different allocation patterns:

| Region | Typical Lifetime | Allocation Rate | Fragmentation Risk | Sensitivity |
|--------|-----------------|-----------------|--------------------|-------------|
| `kMessage` | µs-scale | Very high | Low (blocks freed quickly) | Latency-critical |
| `kActor` | Process lifetime | Low | **High** (long-lived with intermittent free) | Fragmentation-critical |
| `kCoroutine` | Mixed | Medium | Medium | Balanced |
| `kNetwork` | I/O request lifetime | Medium–high | Low | Latency-critical |
| `kInternal` | Indefinite | Low | Low | Balanced |
| `kHibernate` | Infrequent | Very low | N/A (different alloc pattern) | Balanced |

### 1.2 Erlang Reference

Erlang allows per-allocator strategy selection: `gf` (Good Fit) for general
purpose, `af` (A Fit) for temporary allocations, `bf` (Best Fit) for
fragmentation-sensitive workloads, and address-ordered variants for NUMA.

### 1.3 Expected Impact

| Metric | Before | After | Delta |
|--------|--------|-------|-------|
| `kMessage` allocate() p99 | <25 ns (freelist pop) | <10 ns (bump-only) | -60% |
| `kActor` fragmentation under churn | 5% | <2% (coalescing) | -60% |
| `kMessage` freelist contention | Present (CAS on pop) | Eliminated (no freelist) | Qualitative |
| `kNetwork` allocate() p99 | <25 ns | <10 ns (bump-only) | -60% |

---

## 2. Design Goals

1. **Per-region strategy dispatch** — each `RegionType` can independently
   select its allocation strategy.
2. **Zero overhead for default path** — when all regions use the same strategy,
   there is no per-allocation dispatch branch (compile-time constant).
3. **Compile-time gating** — strategies not used at all are compiled out (no
   dead code in the hot path).
4. **Runtime configurability** — strategies are set at `SlabCache` construction
   time; immutable thereafter (no per-allocation branch).
5. **TOML-configurable** — per-region strategy overrides via
   `[system.memory.regions.<name>]` in the topology config.

---

## 3. Design

### 3.1 Strategy Enum and Configuration

```cpp
// include/hpactor/mem/memory_config.hpp

namespace hpactor::mem {

enum class AllocationStrategy : uint8_t {
    kCasLifo = 0,           // Current: bump + CAS LIFO freelist (default)
    kBumpOnly = 1,          // Bump-only, no freelist (messages, network buffers)
    kSegregatedFit = 2,     // Segregated free lists (MEM-001)
};

struct RegionStrategyConfig {
    AllocationStrategy strategy{AllocationStrategy::kCasLifo};
    bool enable_coalescing{false};      // MEM-002
    uint8_t max_coalesce_depth{2};      // MEM-002
};

// Per-region strategy table
struct MemoryStrategyTable {
    RegionStrategyConfig regions[static_cast<uint8_t>(RegionType::kCount)];
};

} // namespace hpactor::mem
```

### 3.2 Default Strategy Assignments

```cpp
// Default strategies (compile-time constants, overridable at runtime)
inline constexpr MemoryStrategyTable kDefaultStrategies = []{
    MemoryStrategyTable t{};

    // kMessage: bump-only — µs-lived, freelist overhead wasted
    t.regions[RegionType::kMessage] = {
        .strategy = AllocationStrategy::kBumpOnly,
        .enable_coalescing = false
    };

    // kActor: segregated + coalescing — long-lived, fragmentation matters
    t.regions[RegionType::kActor] = {
        .strategy = AllocationStrategy::kSegregatedFit,
        .enable_coalescing = true
    };

    // kCoroutine: default — mixed lifetimes, balanced approach
    t.regions[RegionType::kCoroutine] = {
        .strategy = AllocationStrategy::kCasLifo,
        .enable_coalescing = false
    };

    // kNetwork: bump-only — buffer lifetimes match I/O request lifetimes
    t.regions[RegionType::kNetwork] = {
        .strategy = AllocationStrategy::kBumpOnly,
        .enable_coalescing = false
    };

    // kInternal: default — low allocation volume
    t.regions[RegionType::kInternal] = {
        .strategy = AllocationStrategy::kCasLifo,
        .enable_coalescing = false
    };

    // kHibernate: default — infrequent alloc/free
    t.regions[RegionType::kHibernate] = {
        .strategy = AllocationStrategy::kCasLifo,
        .enable_coalescing = false
    };

    return t;
}();
```

### 3.3 Bump-Only Strategy

`kBumpOnly` eliminates the freelist entirely. When a block is freed, it is
NOT pushed to a freelist — it becomes unusable garbage within the slab.

**This only works because:**
1. `kMessage` blocks are µs-lived — by the time a slab is exhausted, all
   previously allocated blocks have been freed (or will be freed before the
   next slab is needed). The slab is entirely free when the bump pointer wraps.
2. `kNetwork` buffer lifetimes match I/O request lifetimes — same property.

**Slab lifecycle under bump-only:**

```
Slab lifetime:
  [bump → allocate until exhausted]
  [wait for all blocks to be freed (tracked via live_count)]
  [when live_count == 0: return slab to SegmentProvider (munmap)]
  [never push to freelist]
```

**Implementation:**

```cpp
// SlabCache::allocate() — kBumpOnly path
allocate(owner_id):
    if (bump_ptr + block_size <= slab_end)
        → bump_allocate(owner_id)
    else
        // Slab exhausted — try to recycle from fully-freed slabs
        if (auto* slab = pop_idle_slab())
            install_slab(slab)
            → retry
        else
            → refill_slab()

// SlabCache::deallocate() — kBumpOnly path
deallocate(ptr):
    header = AllocHeader::from_user_data(ptr)
    // Corruption checks (unchanged)
    // Poison (unchanged)
    header->magic = kFreedMagic
    slab->live_count--  // atomic decrement
    // If live_count == 0: push slab to idle_slabs_ list
    // NEVER push to freelist
```

**Key optimization: idle slab recycling.** When a slab's `live_count` reaches
0, it is pushed to an `idle_slabs_` list (single-consumer stack, no CAS needed
since only the owning thread pushes/pops). The next `allocate()` that
exhausts the current slab pops from `idle_slabs_` before calling
`SegmentProvider::acquire_slab()`. Under steady-state throughput, slabs cycle
between idle and active without any `mmap`/`munmap`.

### 3.4 ThreadLocalAllocator Changes

```cpp
class ThreadLocalAllocator {
    // ... existing ...

    // Strategy table (copy of global config at construction time)
    MemoryStrategyTable strategy_table_;

    // SlabCache constructor now takes strategy config
    void init_cache(RegionType region, SizeClass sc) {
        auto& cfg = strategy_table_.regions[region];
        caches_[region][sc] = new SlabCache(sc, region, cfg);
    }
};
```

### 3.5 TOML Configuration

```toml
[system.memory.regions.message]
strategy = "bump_only"        # "cas_lifo" | "bump_only" | "segregated_fit"

[system.memory.regions.actor]
strategy = "segregated_fit"
coalescing = true

[system.memory.regions.network]
strategy = "bump_only"
```

**Parser:** New self-registering parser in `src/config/parsers/memory_region_config_parser.cpp`.

### 3.6 Fault Injection

| Site | Domain | Action | Purpose |
|------|--------|--------|---------|
| `hpactor.allocator.bump_only.idle_slab_corrupt` | Allocator | Corrupt | Simulate idle slab with corrupted header |
| `hpactor.allocator.bump_only.live_count_underflow` | Allocator | Corrupt | Simulate live_count dropping below 0 (double-free detection) |

---

## 4. Implementation Plan

### 4.1 TDDFlow Sequence

| Step | Test | Implementation |
|------|------|----------------|
| 1 | `StrategyConfigDefaults` — verify default strategies per region | `MemoryStrategyTable`, `kDefaultStrategies` constexpr table |
| 2 | `BumpOnlyAllocate` — allocate from bump-only cache | `SlabCache::allocate()` bump-only path |
| 3 | `BumpOnlyDeallocate` — free in bump-only, live_count decrements | `SlabCache::deallocate()` bump-only path |
| 4 | `BumpOnlySlabRecycle` — slab returns to idle list when live_count=0, reused on exhaustion | Idle slab recycling |
| 5 | `StrategyDispatchCorrectness` — each region uses the correct strategy | `ThreadLocalAllocator` strategy dispatch |
| 6 | `StrategyConfigToml` — TOML overrides work | Memory region config parser |

### 4.2 Files Changed

| File | Change |
|------|--------|
| `include/hpactor/mem/memory_config.hpp` | Add `AllocationStrategy` enum, `RegionStrategyConfig`, `MemoryStrategyTable` |
| `include/hpactor/mem/slab_cache.hpp` | Add strategy-dependent allocate/deallocate paths |
| `src/mem/slab_cache.cpp` | Implement bump-only path, idle slab recycling |
| `include/hpactor/mem/thread_local_allocator.hpp` | Pass strategy table to SlabCache construction |
| `src/mem/thread_local_allocator.cpp` | Initialize caches with per-region strategy |
| `src/config/parsers/memory_region_config_parser.cpp` | **New file** — TOML parser for `[system.memory.regions]` |
| `tests/unit/mem/test_per_region_strategy.cpp` | **New file** — 6 test cases |
| `tests/unit/mem/CMakeLists.txt` | Add new test target |

---

## 5. Testing Strategy

### 5.1 Unit Tests

| Test | What It Validates |
|------|-------------------|
| `StrategyConfigDefaults` | Default strategies match the specification table |
| `BumpOnlyAllocate` | Bump allocation works; no freelist ops occur |
| `BumpOnlyDeallocate` | Free decrements live_count; no freelist push |
| `BumpOnlySlabRecycle` | Idle slab reused on exhaustion; no redundant SegmentProvider call |
| `StrategyDispatchCorrectness` | Each region allocates through correct strategy |
| `StrategyConfigToml` | TOML overrides apply correctly; invalid strategy names rejected |

### 5.2 Integration Tests

| Test | What It Validates |
|------|-------------------|
| `MemStrategyMessageLifecycle` | Messages allocate via bump-only, free correctly, slab recycles |
| `MemStrategyActorLifecycle` | Actors allocate via segregated+coalescing, long-lived fragmentation stays low |

---

## 6. Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| Bump-only slab exhaustion before all blocks freed → memory leak | **Medium** | Track `live_count` per slab. If slab is exhausted but `live_count > 0`, allocate a new slab from SegmentProvider (not idle list). The "leaked" slab remains until `live_count` reaches 0, then joins idle list. Worst case: 1 extra slab per size class per thread. |
| Bump-only not suitable for all workloads | Low | Default strategies are conservative (only `kMessage` and `kNetwork` use bump-only). Configurable at runtime. |
| Strategy enum dispatch adds branch to hot path | Low | Strategy is cached in `SlabCache` member variable; branch predictor saturates after initial allocations. No measurable overhead expected. |

---

## 7. Acceptance Criteria

1. All existing memory tests pass with default strategies.
2. `kMessage` region bump-only: allocation p99.99 < 10 ns (benchmark).
3. `kActor` region segregated+coalescing: fragmentation < 2% after simulated
   7-day churn.
4. `kNetwork` region bump-only: no freelist operations occur (verified via
   `SlabCache::Stats`).
5. TOML strategy overrides work end-to-end.
6. TSan-clean and ASan-clean.
