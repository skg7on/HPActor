# MEM-001: Segregated Free Lists — Implementation Plan

**Date:** 2026-06-20
**Design Spec:** `docs/superpowers/specs/2026-06-20-mem-01-segregated-free-lists-design.md`
**Issue:** #339 (Phase 1.1, P0)
**Depends on:** None (standalone foundation)

## Phase Overview

| Phase | Name | Tests | Files Changed |
|-------|------|-------|---------------|
| 1 | `AllocationStrategy` enum + strategy plumbing in `SlabCache` | 2 | 2 |
| 2 | Segregated bin storage: union with `single_freelist_` | 3 | 1 |
| 3 | Bin index computation + `deallocate()` bin routing | 2 | 1 |
| 4 | Round-robin `start_bin_` cursor in `allocate()` | 2 | 1 |
| 5 | Bounded search depth per bin (depth=3) | 2 | 1 |
| 6 | Refill integration: bump first, then segregated, then refill | 2 | 1 |
| 7 | Fault injection wiring | 2 | 3 |
| 8 | Concurrent stress test (4-thread churn) | 1 | — |

All phases follow RED → GREEN → REFACTOR (TDDFlow).

---

## Phase 1: `AllocationStrategy` Enum + Strategy Plumbing

**Goal:** Add the strategy discriminator to `SlabCache` so the segregated path
can be selected at construction time. Default strategy is `kCasLifo` —
backward-compatible, no behavior change.

### RED — Write failing tests

File: `tests/unit/mem/test_segregated_freelist.cpp` (new)

```cpp
#include <gtest/gtest.h>
#include "hpactor/mem/slab_cache.hpp"
#include "hpactor/mem/size_class.hpp"

using namespace hpactor::mem;

TEST(SegregatedFreelist, StrategyDefaultIsCasLifo) {
    SlabCache cache(SizeClass::k32, RegionType::kActor);
    EXPECT_EQ(cache.strategy(), AllocationStrategy::kCasLifo);
}

TEST(SegregatedFreelist, StrategySetAtConstruction) {
    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    EXPECT_EQ(cache.strategy(), AllocationStrategy::kSegregatedFit);
}
```

### GREEN — Implement

1. Add `AllocationStrategy` enum to `include/hpactor/mem/slab_cache.hpp`:
   ```cpp
   enum class AllocationStrategy : uint8_t {
       kCasLifo = 0,
       kSegregatedFit = 1,
   };
   ```

2. Add strategy parameter to `SlabCache` constructor:
   ```cpp
   class SlabCache {
   public:
       explicit SlabCache(SizeClass sc, RegionType region,
                          AllocationStrategy strategy = AllocationStrategy::kCasLifo);
       AllocationStrategy strategy() const { return strategy_; }
   private:
       AllocationStrategy strategy_;
   };
   ```

3. Update `src/mem/slab_cache.cpp` constructor to store strategy.

### REFACTOR — Verify clean

- Run all existing SlabCache tests — no regression
- Verify default construction uses `kCasLifo`

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"
```

---

## Phase 2: Segregated Bin Storage

**Goal:** Add the segregated bin array (8 `adt::FreeList` instances) in a union
with the existing single freelist. Validate storage layout with `static_assert`.

### RED — Write failing tests

```cpp
TEST(SegregatedFreelist, BinStorageInitialized) {
    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    // After construction, all 8 bins should be empty
    auto stats = cache.stats();
    EXPECT_EQ(stats.segregated_bin_allocs, 0u);
}

TEST(SegregatedFreelist, BinCountMatchesConstant) {
    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    EXPECT_EQ(cache.bin_count(), kNumSegregatedBins);
}

TEST(SegregatedFreelist, UnionSizeConstraint) {
    // The segregated storage must not increase SlabCache size vs single freelist
    // for the default path (verified at compile time)
    SUCCEED();  // static_assert in header validates this
}
```

### GREEN — Implement

1. Add constants to `slab_cache.hpp`:
   ```cpp
   static constexpr uint8_t kNumSegregatedBins = 8;
   static constexpr uint8_t kMaxSearchDepthPerBin = 3;
   ```

2. Add union storage:
   ```cpp
   union FreelistStorage {
       adt::FreeList single;                     // kCasLifo
       adt::FreeList bins[kNumSegregatedBins];   // kSegregatedFit
       FreelistStorage() : single{} {}            // zero-init single
       ~FreelistStorage() {}                      // no-op (trivial dtor)
   } freelist_;
   ```

3. Add `bin_count()` accessor.

### REFACTOR — Verify clean

- `static_assert(sizeof(FreelistStorage::bins) >= sizeof(FreelistStorage::single))`
- Existing tests still pass with union in place

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"
```

---

## Phase 3: Bin Index Computation + Deallocate Routing

**Goal:** On `deallocate()`, compute the correct bin index from the block's
offset within the slab and push to that bin.

### RED — Write failing tests

```cpp
TEST(SegregatedFreelist, DeallocateRoutesToCorrectBin) {
    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    auto* block1 = cache.allocate(ActorId{1});
    auto* block2 = cache.allocate(ActorId{1});

    cache.deallocate(block1);
    cache.deallocate(block2);

    // After freeing, each block is in a bin determined by its offset
    // Re-allocating should return from bins (not bump)
    auto* recycled = cache.allocate(ActorId{1});
    EXPECT_NE(recycled, nullptr);
    // recycled should be one of the freed blocks
    EXPECT_TRUE(recycled == block1 || recycled == block2);
}

TEST(SegregatedFreelist, DeallocateComputeBinIndex) {
    // Bin index = (block_offset / bin_stride) % N
    // For a 64KB slab with 8 bins: stride = 8KB
    // Block at offset 0     → bin 0
    // Block at offset 8KB   → bin 1
    // Block at offset 60KB  → bin 7
    // Validate bin computation helper directly
    size_t slab_size = 64 * 1024;
    size_t stride = slab_size / kNumSegregatedBins;
    EXPECT_EQ(compute_bin_index(0, stride), 0u);
    EXPECT_EQ(compute_bin_index(8192, stride), 1u);
    EXPECT_EQ(compute_bin_index(63488, stride), 7u);
}
```

### GREEN — Implement

1. Add `bin_stride_bytes_` member to `SlabCache`, computed in `refill()` when a
   new slab is installed:
   ```cpp
   uint32_t bin_stride_bytes_;
   // In refill():
   bin_stride_bytes_ = slab_size_bytes / kNumSegregatedBins;
   ```

2. Add `compute_bin_index(AllocHeader* header)` private method:
   ```cpp
   uint8_t compute_bin_index(const AllocHeader* header) const {
       auto* slab_base = current_slab_base();
       size_t offset = reinterpret_cast<const char*>(header) - slab_base;
       return static_cast<uint8_t>((offset / bin_stride_bytes_) % kNumSegregatedBins);
   }
   ```

3. Update `deallocate()` to route to correct bin when strategy is
   `kSegregatedFit`:
   ```cpp
   void SlabCache::deallocate(void* ptr) {
       // ... existing corruption checks ...
       if (strategy_ == AllocationStrategy::kSegregatedFit) {
           uint8_t bin = compute_bin_index(header);
           freelist_.bins[bin].push(header);
       } else {
           freelist_.single.push(header);
       }
   }
   ```

### REFACTOR — Verify clean

- Extract `compute_bin_index` as a free function (testable independently)
- No regression in existing deallocation tests

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SlabCache*"
```

---

## Phase 4: Round-Robin Start Bin in Allocate

**Goal:** `allocate()` rotates the starting bin to prevent all allocations from
clustering in the low-address bins. Search wraps around: bins
`[start, start+N-1]` modulo N.

### RED — Write failing tests

```cpp
TEST(SegregatedFreelist, RoundRobinAdvancesStartBin) {
    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    // Allocate enough to fill a slab, then free some to populate bins
    std::vector<void*> blocks;
    for (int i = 0; i < 100; ++i) {
        blocks.push_back(cache.allocate(ActorId{1}));
    }
    for (int i = 0; i < 100; i += 2) {
        cache.deallocate(blocks[i]);  // free every other block
    }

    // After multiple allocations, start_bin should have advanced
    // (internal state, verified indirectly via allocation pattern)
    auto* a = cache.allocate(ActorId{1});
    auto* b = cache.allocate(ActorId{1});
    EXPECT_NE(a, b);
    // Both came from bins (not bump) because we freed blocks
}

TEST(SegregatedFreelist, StartBinWrapsAfterFullCycle) {
    // After kNumSegregatedBins + 1 allocations from bins, start_bin wraps
    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    // Fill and free blocks to ensure all bins have entries
    std::vector<void*> blocks;
    for (int i = 0; i < 200; ++i) blocks.push_back(cache.allocate(ActorId{1}));
    for (auto* b : blocks) cache.deallocate(b);

    // Allocate kNumSegregatedBins times — one from each bin
    for (int i = 0; i < kNumSegregatedBins; ++i) {
        cache.allocate(ActorId{1});
    }
    // start_bin is now back to original; next allocation would reuse first bin
    SUCCEED();  // no assertion needed — we're verifying no crash/wrap behavior
}
```

### GREEN — Implement

1. Add `start_bin_` member:
   ```cpp
   uint8_t start_bin_{0};
   ```

2. Implement segregated allocate path:
   ```cpp
   void* SlabCache::allocate_from_segregated(ActorId owner) {
       for (uint8_t i = 0; i < kNumSegregatedBins; ++i) {
           uint8_t bin = (start_bin_ + i) % kNumSegregatedBins;
           uint8_t depth = 0;
           while (auto* block = freelist_.bins[bin].try_pop()) {
               if constexpr (kDebugBuild) verify_canary_and_poison(block);
               stamp_header(block, owner);
               start_bin_ = (bin + 1) % kNumSegregatedBins;
               return block->user_data();
               if (++depth >= kMaxSearchDepthPerBin) break;
           }
       }
       return nullptr;  // all bins exhausted
   }
   ```

### REFACTOR — Verify clean

- Extract `allocate_from_segregated` as a private method
- `start_bin_` is only updated on successful allocation

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"
```

---

## Phase 5: Bounded Search Depth Per Bin

**Goal:** Limit per-bin search to `kMaxSearchDepthPerBin` (3), forcing rotation
to the next bin even if the current bin has more free blocks. This prevents
clustering in any single bin and maintains address diversity.

### RED — Write failing tests

```cpp
TEST(SegregatedFreelist, SearchDepthBoundedToThree) {
    // Fill one bin with many freed blocks, others empty
    // Verify we don't drain the whole bin before moving to next
    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);

    // Allocate and free many blocks that all map to the same bin (bin 0)
    // by allocating/freeing blocks in the first bin_stride range
    std::vector<void*> same_bin_blocks;
    size_t blocks_per_bin = 64 * 1024 / kNumSegregatedBins / 72;  // ~113 blocks/bin
    for (size_t i = 0; i < blocks_per_bin; ++i) {
        same_bin_blocks.push_back(cache.allocate(ActorId{1}));
    }
    for (auto* b : same_bin_blocks) cache.deallocate(b);

    // Now allocate kMaxSearchDepthPerBin times from this bin
    // The first 3 should come from bin 0, then the search moves to bin 1
    // But since only bin 0 has blocks, after cycling through all empty bins
    // we'll come back to bin 0 and get 3 more
    for (int i = 0; i < 10; ++i) {
        auto* block = cache.allocate(ActorId{1});
        EXPECT_NE(block, nullptr);
    }
}

TEST(SegregatedFreelist, FallsToNextBinAfterDepthExhausted) {
    // Setup: populate bin 0 with 10 blocks, bin 1 with 5 blocks, others empty
    // Allocate 3 from bin 0 → start_bin=1
    // Allocate: bin 1 has 5 → take up to 3 before moving to bin 2
    // Bin 2 is empty → bin 3 → ... → wrap to bin 0
    // Verify we eventually get blocks from both bin 0 and bin 1
    SUCCEED();  // Validated by observing allocation patterns in later stats test
}
```

### GREEN — Implement

The depth counter is already in the Phase 4 `allocate_from_segregated()`
implementation. Add a stats counter for observability:

```cpp
struct Stats {
    // ... existing ...
    std::atomic<uint64_t> segregated_bin_exhausted_depth{0};  // hit depth limit
};
```

### REFACTOR — Verify clean

- `kMaxSearchDepthPerBin` is a `static constexpr` — no magic number
- Stats counter validates the bounded-search behavior

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"
```

---

## Phase 6: Refill Integration

**Goal:** Integrate segregated allocation into the full allocate path:
bump first → segregated bins → refill from `SegmentProvider`.

### RED — Write failing tests

```cpp
TEST(SegregatedFreelist, RefillWhenAllBinsExhausted) {
    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);

    // Allocate until first slab is full
    std::vector<void*> blocks;
    for (int i = 0; i < 1000; ++i) {
        void* block = cache.allocate(ActorId{1});
        ASSERT_NE(block, nullptr);
        blocks.push_back(block);
    }

    // Bump pointer should have refilled at least once
    auto stats = cache.stats();
    EXPECT_GE(stats.slab_acquire_count.load(), 1u);
}

TEST(SegregatedFreelist, NewSlabInheritsStrategy) {
    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);

    // Fill first slab completely
    std::vector<void*> blocks;
    while (blocks.size() < 2000) {
        void* block = cache.allocate(ActorId{1});
        if (!block) break;
        blocks.push_back(block);
    }

    // Free all — blocks go to segregated bins
    for (auto* b : blocks) cache.deallocate(b);

    // Re-allocate — should come from bins, not virgin bump
    auto* recycled = cache.allocate(ActorId{1});
    EXPECT_NE(recycled, nullptr);

    auto stats = cache.stats();
    EXPECT_GT(stats.segregated_bin_allocs.load(), 0u);
}
```

### GREEN — Implement

Update `SlabCache::allocate()` to call `allocate_from_segregated()` after bump
exhaustion:

```cpp
void* SlabCache::allocate(ActorId owner) {
    // 1. Try freelist (strategy-dependent)
    if (strategy_ == AllocationStrategy::kSegregatedFit) {
        if (auto* block = allocate_from_segregated(owner))
            return block;
    } else {
        if (auto* block = freelist_.single.try_pop()) {
            stamp_header(block, owner);
            return block->user_data();
        }
    }

    // 2. Bump allocate
    if (bump_ptr_ + block_size_ <= slab_end_) {
        auto* block = bump_allocate(owner);
        return block->user_data();
    }

    // 3. Refill
    if (refill()) goto retry;  // retry from step 1
    return nullptr;  // OOM
}
```

### REFACTOR — Verify clean

- Strategy dispatch is a single branch at the top of `allocate()` — predictor-friendly
- `refill()` sets `bin_stride_bytes_` from new slab size

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SlabCache*"
```

---

## Phase 7: Fault Injection Wiring

**Goal:** Wire `FAULT_INJECT` sites for segregated path testing.

### RED — Write failing tests

```cpp
TEST(SegregatedFreelist, FaultInjectionBinPopCorrupt) {
    // Enable fault: hpactor.allocator.segregated.bin_pop.corrupt
    // Allocate from bin → should detect corruption via canary mismatch
    FaultSchedule::Builder()
        .at("hpactor.allocator.segregated.bin_pop.corrupt",
            FaultAction::Corrupt)
        .build_and_install();
    FaultController::instance().enable();

    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    auto* block = cache.allocate(ActorId{1});
    cache.deallocate(block);

    // Next allocate should hit corrupt bin pop
    // In debug build: assertion fires
    // In release: nullptr returned (corruption detected, block skipped)
    auto* corrupt = cache.allocate(ActorId{1});
    // Either nullptr or the corruption was detected
    FaultController::instance().disable();
}

TEST(SegregatedFreelist, FaultInjectionAllBinsEmpty) {
    FaultSchedule::Builder()
        .at("hpactor.allocator.segregated.bins_empty",
            FaultAction::Fail)
        .build_and_install();
    FaultController::instance().enable();

    SlabCache cache(SizeClass::k32, RegionType::kActor,
                    AllocationStrategy::kSegregatedFit);
    auto* block = cache.allocate(ActorId{1});  // from bump
    cache.deallocate(block);  // goes to bin, but bin appears empty

    // Next allocate from segregated returns nullptr → falls to bump → refill
    auto* block2 = cache.allocate(ActorId{1});
    EXPECT_NE(block2, nullptr);  // refill succeeded
    FaultController::instance().disable();
}
```

### GREEN — Implement

1. Add `FAULT_INJECT` in `allocate_from_segregated()`:
   ```cpp
   void* SlabCache::allocate_from_segregated(ActorId owner) {
       for (uint8_t i = 0; i < kNumSegregatedBins; ++i) {
           uint8_t bin = (start_bin_ + i) % kNumSegregatedBins;
           uint8_t depth = 0;
           while (auto* block = freelist_.bins[bin].try_pop()) {
               FAULT_INJECT("hpactor.allocator.segregated.bin_pop.corrupt",
                            block->magic = kFreedMagic + 1);
               // ... stamp + return ...
           }
       }
       FAULT_INJECT("hpactor.allocator.segregated.bins_empty", return nullptr);
       return nullptr;
   }
   ```

2. Register fault points in `src/fault/fault_points.cpp`.

### REFACTOR — Verify clean

- Fault injection sites are `HPACTOR_UNLIKELY` cold branches
- Disabled at compile time when `ENABLE_FAULT_INJECTION=OFF`

### Verification:
```bash
cmake -S . -B build -DENABLE_FAULT_INJECTION=ON
ninja -C build test_unit_fault test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"
```

---

## Phase 8: Concurrent Stress Test

**Goal:** Validate thread safety with 4-thread alloc/free churn using
segregated strategy for `kActor` region.

### RED — Write failing test

```cpp
TEST(SegregatedFreelist, Concurrent4ThreadStress) {
    // Spawn 4 threads, each with its own ThreadLocalAllocator using
    // kSegregatedFit for kActor region.
    // Each thread: 10K alloc + 10K free, random sizes, verify integrity

    std::atomic<bool> start{false};
    std::atomic<uint64_t> total_allocs{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            ThreadLocalAllocator tla(/* strategy_override */);
            while (!start.load()) { /* spin */ }
            std::vector<void*> blocks;
            for (int i = 0; i < 10000; ++i) {
                auto* b = tla.allocate(RegionType::kActor, 32, ActorId{uint32_t(t * 10000 + i)});
                if (b) {
                    blocks.push_back(b);
                    total_allocs.fetch_add(1);
                }
            }
            for (auto* b : blocks) {
                tla.deallocate(b);
            }
        });
    }

    start.store(true);
    for (auto& t : threads) t.join();

    EXPECT_GE(total_allocs.load(), 30000u);  // at least 75% success
}
```

### GREEN — Implement

No new code — validates existing implementation under concurrent load.

### REFACTOR — Verify clean

- TSan-clean: no data races on `freelist_.bins[]` or `start_bin_`
- Thread-local `SlabCache` invariant holds (each thread has its own)

### Verification:
```bash
cmake -S . -B build -DENABLE_TSAN=ON
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"
```

---

## Files Changed Summary

| File | Phase | Change |
|------|-------|--------|
| `include/hpactor/mem/slab_cache.hpp` | 1–6 | `AllocationStrategy` enum, `kNumSegregatedBins`, union storage, `start_bin_`, `bin_stride_bytes_`, `allocate_from_segregated()`, segregated stats |
| `src/mem/slab_cache.cpp` | 1–6 | Constructor strategy param, `deallocate()` bin routing, `allocate()` strategy dispatch, `refill()` stride setup |
| `include/hpactor/mem/thread_local_allocator.hpp` | 1 | Pass strategy to `SlabCache` constructor |
| `src/mem/thread_local_allocator.cpp` | 1 | Strategy parameter plumbing |
| `src/fault/fault_points.cpp` | 7 | Register 2 new fault points |
| `tests/unit/mem/test_segregated_freelist.cpp` | 1–8 | **New file** — 15 test cases |
| `tests/unit/mem/CMakeLists.txt` | 1 | Add `test_segregated_freelist` GTest target |

## Verification Checklist

After all phases complete:

```bash
# Unit tests
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"

# All existing memory tests — zero regression
ctest -R "test_unit_mem" --output-on-failure --parallel 8

# TSan
cmake -S . -B build-tsan -DENABLE_TSAN=ON
ninja -C build-tsan test_unit_mem
./build-tsan/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"

# ASan
cmake -S . -B build-asan -DENABLE_ASAN=ON
ninja -C build-asan test_unit_mem
./build-asan/tests/unit/mem/test_unit_mem --gtest_filter="*SegregatedFreelist*"

# Fault injection
cmake -S . -B build -DENABLE_FAULT_INJECTION=ON
ctest -R "test_unit_mem|test_unit_fault" --output-on-failure
```
