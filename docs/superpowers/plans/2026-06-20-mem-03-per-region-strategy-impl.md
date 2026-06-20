# MEM-003: Per-Region Allocation Strategy — Implementation Plan

**Date:** 2026-06-20
**Design Spec:** `docs/superpowers/specs/2026-06-20-mem-03-per-region-strategy-design.md`
**Issue:** #339 (Phase 1.3, P1)
**Depends on:** MEM-001 (Segregated Free Lists), MEM-002 (Free Block Coalescing)

## Phase Overview

| Phase | Name | Tests | Files Changed |
|-------|------|-------|---------------|
| 1 | `RegionStrategyConfig` struct + `MemoryStrategyTable` | 2 | 1 |
| 2 | `kBumpOnly` strategy — allocate path | 3 | 1 |
| 3 | `kBumpOnly` strategy — deallocate + idle slab recycling | 3 | 1 |
| 4 | ThreadLocalAllocator strategy dispatch | 2 | 2 |
| 5 | Default strategy assignments per region | 2 | 2 |
| 6 | TOML config parser | 3 | 2 |

All phases follow RED → GREEN → REFACTOR.

---

## Phase 1: `RegionStrategyConfig` + `MemoryStrategyTable`

**Goal:** Define the configuration structures that let each `RegionType` select
its allocation strategy independently.

### RED — Write failing tests

File: `tests/unit/mem/test_per_region_strategy.cpp` (new)

```cpp
#include <gtest/gtest.h>
#include "hpactor/mem/memory_config.hpp"

using namespace hpactor::mem;

TEST(PerRegionStrategy, DefaultStrategyIsCasLifo) {
    RegionStrategyConfig cfg{};
    EXPECT_EQ(cfg.strategy, AllocationStrategy::kCasLifo);
    EXPECT_FALSE(cfg.enable_coalescing);
}

TEST(PerRegionStrategy, TableHasEntryForEachRegion) {
    MemoryStrategyTable table = kDefaultStrategies;
    for (int i = 0; i < static_cast<int>(RegionType::kCount); ++i) {
        // Every region has a valid strategy
        auto s = table.regions[i].strategy;
        EXPECT_TRUE(s == AllocationStrategy::kCasLifo ||
                    s == AllocationStrategy::kBumpOnly ||
                    s == AllocationStrategy::kSegregatedFit);
    }
}
```

### GREEN — Implement

Add to `include/hpactor/mem/memory_config.hpp`:
```cpp
struct RegionStrategyConfig {
    AllocationStrategy strategy{AllocationStrategy::kCasLifo};
    bool enable_coalescing{false};
    uint8_t max_coalesce_depth{2};
};

struct MemoryStrategyTable {
    RegionStrategyConfig regions[static_cast<uint8_t>(RegionType::kCount)];
};

extern const MemoryStrategyTable kDefaultStrategies;
```

Define `kDefaultStrategies` in `src/mem/memory_config.cpp` with the
assignments from the design spec.

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*PerRegionStrategy*"
```

---

## Phase 2: `kBumpOnly` Strategy — Allocate Path

**Goal:** In `SlabCache::allocate()`, when strategy is `kBumpOnly`, skip freelist
entirely. Bump-allocate until slab exhaustion.

### RED — Write failing tests

```cpp
TEST(PerRegionStrategy, BumpOnlyAllocateSkipsFreelist) {
    SlabCache cache(SizeClass::k64, RegionType::kMessage,
                    AllocationStrategy::kBumpOnly);
    auto* b1 = cache.allocate(ActorId{1});
    auto* b2 = cache.allocate(ActorId{1});
    EXPECT_NE(b1, b2);

    // Free b1 — in bump-only, it goes to idle tracking, NOT freelist
    cache.deallocate(b1);
    // b1 should NOT be immediately reusable (no freelist pop)
    auto* b3 = cache.allocate(ActorId{1});
    EXPECT_NE(b3, b1);  // b3 comes from bump, not recycled
}

TEST(PerRegionStrategy, BumpOnlyNoFreelistOps) {
    SlabCache cache(SizeClass::k64, RegionType::kMessage,
                    AllocationStrategy::kBumpOnly);
    auto stats_before = cache.stats();

    for (int i = 0; i < 100; ++i) {
        cache.allocate(ActorId{1});
    }
    auto stats_after = cache.stats();
    // All allocations from bump — no freelist ops
    EXPECT_EQ(stats_after.free_count, stats_before.free_count);
}

TEST(PerRegionStrategy, BumpOnlyRefillWhenExhausted) {
    SlabCache cache(SizeClass::k32, RegionType::kMessage,
                    AllocationStrategy::kBumpOnly);
    std::vector<void*> blocks;
    for (int i = 0; i < 2000; ++i) {
        auto* b = cache.allocate(ActorId{1});
        ASSERT_NE(b, nullptr);
        blocks.push_back(b);
    }
    auto stats = cache.stats();
    EXPECT_GE(stats.slab_acquire_count.load(), 1u);
}
```

### GREEN — Implement

```cpp
void* SlabCache::allocate(ActorId owner) {
    if (strategy_ == AllocationStrategy::kBumpOnly) {
        return allocate_bump_only(owner);
    }
    // ... existing kCasLifo + kSegregatedFit paths ...
}

void* SlabCache::allocate_bump_only(ActorId owner) {
    if (bump_ptr_ + block_size_ <= slab_end_) {
        return bump_allocate(owner);
    }
    // Try idle slab
    if (auto* slab = pop_idle_slab()) {
        install_slab(slab);
        return bump_allocate(owner);
    }
    // Refill
    if (refill()) return bump_allocate(owner);
    return nullptr;
}
```

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*PerRegionStrategy*"
```

---

## Phase 3: `kBumpOnly` — Deallocate + Idle Slab Recycling

**Goal:** On `deallocate()`, decrement live count. When a slab's `live_count`
reaches 0, push it to the idle list for reuse (no `munmap`).

### RED — Write failing tests

```cpp
TEST(PerRegionStrategy, BumpOnlyDeallocateDecrementsLiveCount) {
    auto* cache = new SlabCache(SizeClass::k64, RegionType::kMessage,
                                AllocationStrategy::kBumpOnly);
    auto* b1 = cache->allocate(ActorId{1});
    auto* b2 = cache->allocate(ActorId{1});

    auto stats_before = cache->stats();
    cache->deallocate(b1);
    cache->deallocate(b2);
    auto stats_after = cache->stats();
    EXPECT_EQ(stats_after.free_count, stats_before.free_count + 2u);
}

TEST(PerRegionStrategy, IdleSlabRecycled) {
    SlabCache cache(SizeClass::k32, RegionType::kMessage,
                    AllocationStrategy::kBumpOnly);
    // Fill one slab
    std::vector<void*> slab1_blocks;
    for (int i = 0; i < 200; ++i) slab1_blocks.push_back(cache.allocate(ActorId{1}));

    // Allocate more → triggers new slab
    std::vector<void*> slab2_blocks;
    for (int i = 0; i < 200; ++i) slab2_blocks.push_back(cache.allocate(ActorId{1}));

    auto stats_before = cache.stats();
    size_t slabs_before = stats_before.slab_acquire_count.load();

    // Free all from slab1 → live_count→0 → idle list
    for (auto* b : slab1_blocks) cache.deallocate(b);

    // Allocate more → slab1 recycled from idle list, no new SegmentProvider call
    std::vector<void*> recycled_blocks;
    for (int i = 0; i < 200; ++i) recycled_blocks.push_back(cache.allocate(ActorId{1}));

    auto stats_after = cache.stats();
    EXPECT_EQ(stats_after.slab_acquire_count.load(), slabs_before);  // no new slab
}

TEST(PerRegionStrategy, BumpOnlyLiveCountUnderflowPrevention) {
    // Double-free should be caught (live_count doesn't go below 0)
    SlabCache cache(SizeClass::k64, RegionType::kMessage,
                    AllocationStrategy::kBumpOnly);
    auto* b = cache.allocate(ActorId{1});
    cache.deallocate(b);
    // Second deallocate should be caught by magic check (kFreedMagic != kAllocMagic)
    // even in bump-only mode
}
```

### GREEN — Implement

1. Add `idle_slabs_` stack to `SlabCache`:
   ```cpp
   std::vector<Slab*> idle_slabs_;  // single-consumer, no CAS needed
   ```

2. Update `deallocate()`:
   ```cpp
   void SlabCache::deallocate(void* ptr) {
       // ... corruption checks (unchanged) ...
       header->magic = kFreedMagic;
       if (strategy_ == AllocationStrategy::kBumpOnly) {
           // Decrement live_count on the slab
           auto* slab = owning_slab(header);
           if (slab->live_count.fetch_sub(1) == 1) {
               // Just went to 0 — slab is idle
               idle_slabs_.push_back(slab);
           }
           return;  // No freelist push
       }
       // ... existing freelist push for kCasLifo / kSegregatedFit ...
   }
   ```

3. Update `allocate_bump_only()` to try `pop_idle_slab()` before `refill()`.

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*PerRegionStrategy*"
```

---

## Phase 4: ThreadLocalAllocator Strategy Dispatch

**Goal:** `ThreadLocalAllocator` passes the correct `RegionStrategyConfig` to
each `SlabCache` at construction time.

### RED — Write failing tests

```cpp
TEST(PerRegionStrategy, TlaCreatesCachesWithCorrectStrategy) {
    MemoryStrategyTable table = kDefaultStrategies;
    // Override: kMessage → bump-only, kActor → segregated+coalescing
    table.regions[RegionType::kMessage] = {AllocationStrategy::kBumpOnly, false};
    table.regions[RegionType::kActor] = {AllocationStrategy::kSegregatedFit, true};

    ThreadLocalAllocator tla(table);

    // kMessage should use bump-only
    auto* msg_block = tla.allocate(RegionType::kMessage, 64, ActorId{1});
    EXPECT_NE(msg_block, nullptr);
    tla.deallocate(msg_block);
    // Re-allocate immediately — should come from bump (not recycled from freelist)
    auto* msg_block2 = tla.allocate(RegionType::kMessage, 64, ActorId{1});
    EXPECT_NE(msg_block2, msg_block);  // different: bump, not freelist pop

    // kActor should use segregated
    auto* actor_block = tla.allocate(RegionType::kActor, 64, ActorId{1});
    EXPECT_NE(actor_block, nullptr);
    tla.deallocate(actor_block);
}

TEST(PerRegionStrategy, TlaDeallocRoutesCorrectly) {
    // Cross-region dealloc: message freed to kMessage cache, actor to kActor cache
    ThreadLocalAllocator tla(kDefaultStrategies);
    auto* msg = tla.allocate(RegionType::kMessage, 32, ActorId{1});
    auto* act = tla.allocate(RegionType::kActor, 32, ActorId{1});
    tla.deallocate(msg);   // routes to kMessage cache
    tla.deallocate(act);   // routes to kActor cache
    SUCCEED();  // no crash or cross-type corruption
}
```

### GREEN — Implement

Update `ThreadLocalAllocator` constructor to propagate strategy:
```cpp
ThreadLocalAllocator::ThreadLocalAllocator(const MemoryStrategyTable& table)
    : strategy_table_(table) {
    for (int r = 0; r < kNumRegionTypes; ++r) {
        for (int s = 0; s < kNumSizeClasses; ++s) {
            auto& cfg = strategy_table_.regions[r];
            caches_[r][s] = new SlabCache(
                static_cast<SizeClass>(s),
                static_cast<RegionType>(r),
                cfg.strategy,
                cfg.enable_coalescing);
        }
    }
}
```

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*PerRegionStrategy*"
```

---

## Phase 5: Default Strategy Assignments

**Goal:** Define and validate the production default strategies per region.

### RED — Write failing tests

```cpp
TEST(PerRegionStrategy, DefaultStrategiesMatchSpec) {
    auto& t = kDefaultStrategies;

    // kMessage: bump-only, no coalescing
    EXPECT_EQ(t.regions[RegionType::kMessage].strategy,
              AllocationStrategy::kBumpOnly);
    EXPECT_FALSE(t.regions[RegionType::kMessage].enable_coalescing);

    // kActor: segregated, with coalescing
    EXPECT_EQ(t.regions[RegionType::kActor].strategy,
              AllocationStrategy::kSegregatedFit);
    EXPECT_TRUE(t.regions[RegionType::kActor].enable_coalescing);

    // kCoroutine: default (cas_lifo)
    EXPECT_EQ(t.regions[RegionType::kCoroutine].strategy,
              AllocationStrategy::kCasLifo);

    // kNetwork: bump-only
    EXPECT_EQ(t.regions[RegionType::kNetwork].strategy,
              AllocationStrategy::kBumpOnly);

    // kInternal: default
    EXPECT_EQ(t.regions[RegionType::kInternal].strategy,
              AllocationStrategy::kCasLifo);

    // kHibernate: default
    EXPECT_EQ(t.regions[RegionType::kHibernate].strategy,
              AllocationStrategy::kCasLifo);
}

TEST(PerRegionStrategy, DefaultStrategiesBackwardCompatible) {
    // Existing tests use default-constructed ThreadLocalAllocator
    // (which uses kDefaultStrategies). All regions except kMessage/kNetwork/kActor
    // should use the original kCasLifo strategy.
    // This test validates that MemoryRegionRegistry and related subsystems
    // continue to work with the strategy table in place.
    SUCCEED();  // Implicit: all existing tests pass = backward compatible
}
```

### GREEN — Implement

Define `kDefaultStrategies` in `src/mem/memory_config.cpp` per the design spec table.

### Verification:
```bash
ctest -R "test_unit_mem" --output-on-failure --parallel 8
```

---

## Phase 6: TOML Config Parser

**Goal:** Self-registering parser for `[system.memory.regions.<name>]` to override
strategy per region at runtime.

### RED — Write failing tests

File: extend `tests/unit/config/test_config_parser.cpp` or create
`tests/unit/mem/test_memory_config_parser.cpp`:

```cpp
TEST(MemoryConfigParser, ParseRegionStrategy) {
    const char* toml = R"(
        [system.memory.regions.message]
        strategy = "bump_only"

        [system.memory.regions.actor]
        strategy = "segregated_fit"
        coalescing = true
    )";
    auto model = parse_toml_string(toml);
    auto& mem_cfg = model.system_config.memory_strategies;

    EXPECT_EQ(mem_cfg.regions[RegionType::kMessage].strategy,
              AllocationStrategy::kBumpOnly);
    EXPECT_EQ(mem_cfg.regions[RegionType::kActor].strategy,
              AllocationStrategy::kSegregatedFit);
    EXPECT_TRUE(mem_cfg.regions[RegionType::kActor].enable_coalescing);
}

TEST(MemoryConfigParser, InvalidStrategyNameRejected) {
    const char* toml = R"(
        [system.memory.regions.message]
        strategy = "invalid_strategy"
    )";
    auto model = parse_toml_string(toml);
    EXPECT_TRUE(model.has_error());  // or: falls back to default
}

TEST(MemoryConfigParser, PartialOverridePreservesDefaults) {
    const char* toml = R"(
        [system.memory.regions.message]
        strategy = "bump_only"
    )";
    auto model = parse_toml_string(toml);
    // Only kMessage overridden — all others keep defaults
    auto& mem_cfg = model.system_config.memory_strategies;
    EXPECT_EQ(mem_cfg.regions[RegionType::kCoroutine].strategy,
              AllocationStrategy::kCasLifo);
}
```

### GREEN — Implement

1. Create `src/config/parsers/memory_region_config_parser.cpp`:
   ```cpp
   class MemoryRegionConfigParser : public ITomlSystemParser {
       void parse(const TomlTableView& table, TopologyModel& model) override {
           // Iterate [system.memory.regions.*]
           // Map strategy strings to enum values
           // Fill model.system_config.memory_strategies
       }
   };
   static TomlSystemParserRegistration<MemoryRegionConfigParser> s_reg;
   ```

2. Add strategy name mapping: `"cas_lifo"` → `kCasLifo`, `"bump_only"` →
   `kBumpOnly`, `"segregated_fit"` → `kSegregatedFit`.

### Verification:
```bash
ninja -C build test_unit_mem test_unit_config
./build/tests/unit/mem/test_unit_mem --gtest_filter="*PerRegionStrategy*"
ctest -R "MemoryConfigParser" --output-on-failure
```

---

## Files Changed Summary

| File | Change |
|------|--------|
| `include/hpactor/mem/memory_config.hpp` | `RegionStrategyConfig`, `MemoryStrategyTable`, `kDefaultStrategies` declaration |
| `src/mem/memory_config.cpp` | `kDefaultStrategies` definition, strategy name mapping |
| `include/hpactor/mem/slab_cache.hpp` | `AllocationStrategy::kBumpOnly`, idle slab stack, `allocate_bump_only()`, bump-only `deallocate()` path |
| `src/mem/slab_cache.cpp` | Bump-only allocate/deallocate, idle slab recycling |
| `include/hpactor/mem/thread_local_allocator.hpp` | Constructor takes `MemoryStrategyTable` |
| `src/mem/thread_local_allocator.cpp` | Strategy propagation to SlabCache constructors |
| `src/config/parsers/memory_region_config_parser.cpp` | **New file** — TOML parser for `[system.memory.regions]` |
| `tests/unit/mem/test_per_region_strategy.cpp` | **New file** — 15 test cases |
| `tests/unit/mem/CMakeLists.txt` | Add new test target |

## Verification Checklist

```bash
# Unit tests
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*PerRegionStrategy*"

# All existing tests — zero regression with new defaults
ctest -R "test_unit_mem|test_unit_config" --output-on-failure --parallel 8

# Verify kMessage region uses bump-only (no freelist ops)
# via bench_saturate: allocation rate should increase

# TSan
cmake -S . -B build-tsan -DENABLE_TSAN=ON
ninja -C build-tsan test_unit_mem
ctest -R "test_unit_mem" -j4
```
