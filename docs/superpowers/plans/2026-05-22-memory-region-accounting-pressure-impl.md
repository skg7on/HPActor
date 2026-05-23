# Memory Region Accounting And Pressure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the memory-management gap where typed memory regions, per-actor accounting, allocator telemetry, and watermark pressure are described by the architecture but not enforced by the runtime allocator paths.

**Architecture:** Keep the existing two-tier allocator, but make allocation provenance explicit. Every managed allocation carries its region and fallback/provenance flag in `AllocHeader`, every slab has owner metadata in `SegmentProvider`, each `ThreadLocalAllocator` owns a region-by-size-class cache matrix, and `mem::allocate()` becomes the single admission point for region limits, memory tracking, and sampled telemetry.

**Tech Stack:** C++20, no exceptions, no RTTI, existing `hpactor_lib`, `mem::ThreadLocalAllocator`, `mem::SlabCache`, `mem::MemoryTracker`, `mem::TelemetryRingBuffer`, CMake/Ninja, GoogleTest discovered through `cmake/gtest.cmake`.

---

## Gap Analysis

The current implementation has strong allocator building blocks, but the architecture goal is not complete yet:

| Design requirement | Current implementation | Gap to close |
|---|---|---|
| Typed memory regions get separate slab sets and stats | `RegionType` exists, but `mem::allocate(RegionType, ...)` ignores the region and `ThreadLocalAllocator` is keyed only by size class | Add region-aware cache matrix and region stats |
| Per-actor shadow counters are on allocator hot paths | `MemoryTracker` exists and tests call it manually | Wire `record_alloc()` and `record_free()` into `mem::allocate()` and `mem::deallocate()` |
| Allocation telemetry is sampled by allocator paths | `TelemetryRingBuffer` exists, but allocator paths never emit events | Add a memory telemetry singleton and sampled event emission |
| Watermark-based pressure prevents OOM | No region limits or rejection path exist | Add `MemoryRegionRegistry` admission checks with configurable hard limits and pressure state |
| Cross-thread frees return blocks to the owning slab | `ThreadLocalAllocator::deallocate()` routes by the freeing thread's cache only | Track slab owner metadata and return frees to the origin `SlabCache` |
| Fallback allocations are safe across allocator contexts | `mem::allocate()` falls back to raw `malloc`, while `mem::deallocate()` blindly uses `t_tla` when present | Give fallback allocations the same `AllocHeader` and a fallback flag |

This plan intentionally focuses on the accounting and pressure foundation. Full actor hibernation, actor relocation compaction, and subsystem-specific reclaim actions remain separate work because they depend on trustworthy region accounting first.

## Current Test Architecture

This plan targets the reorganized GoogleTest layout:

- `tests/CMakeLists.txt` includes `cmake/gtest.cmake`, creates `hpactor_test_support`, and delegates to `tests/unit`, `tests/integration`, and `tests/system`.
- Unit memory tests live in `tests/unit/mem/` and are compiled into one executable: `test_unit_mem`.
- `tests/unit/mem/CMakeLists.txt` lists each memory test source in the `add_executable(test_unit_mem ...)` source list, links `hpactor pthread hpactor_test_support GTest::gtest_main`, and calls `gtest_discover_tests(test_unit_mem)`.
- Actor integration tests live in `tests/integration/actor/` and are compiled into `test_integration_actor`, also using `GTest::gtest_main`.
- New tests should use `#include <gtest/gtest.h>`, `TEST`/`TEST_F`, and `EXPECT_*`/`ASSERT_*`; do not add new standalone `int main()` tests or new per-file executable targets.
- Implementation should begin from a branch that already contains this test reorganization. If this plan branch is older than that reorg, merge or rebase the test reorg before starting Task 1 so the paths and targets below exist.

## File Structure

**Create:**
- `src/mem/memory_region.cpp` - region registry, admission checks, snapshots, and test reset helpers
- `include/hpactor/mem/memory_telemetry.hpp` - memory telemetry singleton around `TelemetryRingBuffer`
- `src/mem/memory_telemetry.cpp` - telemetry singleton storage and sampled event emission
- `tests/unit/mem/test_memory_region_accounting.cpp` - allocation provenance, region accounting, tracking, fallback safety, and pressure admission tests

**Modify:**
- `include/hpactor/mem/alloc_header.hpp` - region and provenance flag helpers
- `include/hpactor/mem/segment_provider.hpp` - slab owner metadata and slab lookup API
- `src/mem/segment_provider.cpp` - register and look up slab owners
- `include/hpactor/mem/slab_cache.hpp` - store `RegionType`, register slab ownership
- `src/mem/slab_cache.cpp` - stamp region metadata and preserve origin-cache frees
- `include/hpactor/mem/thread_local_allocator.hpp` - region-aware cache matrix and stats accessor
- `src/mem/thread_local_allocator.cpp` - allocate/deallocate by region and origin slab
- `include/hpactor/mem/memory_region.hpp` - region limits, pressure state, snapshots, registry API
- `include/hpactor/mem/memory_config.hpp` - route allocation through region admission, tracker, fallback header, telemetry
- `src/mem/memory_config.cpp` - fallback helpers and telemetry/tracker integration storage
- `include/hpactor/mem/std_allocator.hpp` - call `mem::allocate(region, ...)` instead of bypassing region accounting
- `include/hpactor/core/actor_system.hpp` - route programmatic actor spawn through `mem::allocate_shared(..., kActor, ...)`
- `src/CMakeLists.txt` - add new memory source files
- `tests/unit/mem/CMakeLists.txt` - add `test_memory_region_accounting.cpp` to `test_unit_mem`

## Task 1: Add Failing Coverage For The Missing Runtime Contract

**Files:**
- Create: `tests/unit/mem/test_memory_region_accounting.cpp`
- Modify: `tests/unit/mem/CMakeLists.txt`

- [ ] **Step 1: Add the failing test file**

```cpp
// tests/unit/mem/test_memory_region_accounting.cpp
#include <gtest/gtest.h>
#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/memory_tracker.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>

#include <cstring>
#include <thread>

namespace mem = hpactor::mem;

static hpactor::ActorId test_actor(uint64_t offset) {
    return hpactor::ActorId{900000 + offset};
}

static mem::ActorMemoryStats actor_stats(hpactor::ActorId id) {
    mem::ActorMemoryStats out{};
    mem::MemoryTracker::instance().snapshot(id, out);
    return out;
}

class MemoryRegionAccountingTest : public ::testing::Test {
  protected:
    void TearDown() override {
        mem::set_thread_allocator(nullptr);
        mem::MemoryRegionRegistry::instance().configure_region(
            mem::RegionType::kNetwork, mem::RegionLimit{});
    }
};

TEST_F(MemoryRegionAccountingTest, RegionMetadataAndAccounting) {
    mem::ThreadLocalAllocator tla;
    mem::set_thread_allocator(&tla);

    auto& regions = mem::MemoryRegionRegistry::instance();
    const auto before_region = regions.snapshot(mem::RegionType::kMessage);
    const auto before_actor = actor_stats(test_actor(1));

    void* ptr = mem::allocate(mem::RegionType::kMessage, 100, test_actor(1));
    ASSERT_NE(ptr, nullptr);

    auto* header = mem::AllocHeader::from_user_data(ptr);
    EXPECT_EQ(header->region(), mem::RegionType::kMessage);
    EXPECT_FALSE(header->is_fallback());
    EXPECT_EQ(header->user_size(), 128u);

    const auto during_region = regions.snapshot(mem::RegionType::kMessage);
    const auto during_actor = actor_stats(test_actor(1));
    EXPECT_EQ(during_region.active_bytes, before_region.active_bytes + 128);
    EXPECT_EQ(during_region.alloc_count, before_region.alloc_count + 1);
    EXPECT_EQ(during_actor.current_bytes, before_actor.current_bytes + 128);
    EXPECT_EQ(during_actor.alloc_count, before_actor.alloc_count + 1);

    mem::deallocate(ptr);

    const auto after_region = regions.snapshot(mem::RegionType::kMessage);
    const auto after_actor = actor_stats(test_actor(1));
    EXPECT_EQ(after_region.active_bytes, before_region.active_bytes);
    EXPECT_EQ(after_region.free_count, before_region.free_count + 1);
    EXPECT_EQ(after_actor.current_bytes, before_actor.current_bytes);
    EXPECT_EQ(after_actor.free_count, before_actor.free_count + 1);
}

TEST_F(MemoryRegionAccountingTest, CrossThreadFreeReturnsToOriginCache) {
    mem::ThreadLocalAllocator producer;
    mem::ThreadLocalAllocator consumer;

    mem::set_thread_allocator(&producer);
    const auto before_free =
        producer.stats(mem::RegionType::kMessage, mem::SizeClass::k32B)
            .free_count.load();

    void* ptr = mem::allocate(mem::RegionType::kMessage, 32, test_actor(2));
    ASSERT_NE(ptr, nullptr);

    mem::set_thread_allocator(&consumer);
    mem::deallocate(ptr);

    const auto after_free =
        producer.stats(mem::RegionType::kMessage, mem::SizeClass::k32B)
            .free_count.load();
    EXPECT_EQ(after_free, before_free + 1);
}

TEST_F(MemoryRegionAccountingTest, FallbackAllocationIsSelfDescribing) {
    mem::set_thread_allocator(nullptr);

    void* ptr = mem::allocate(mem::RegionType::kNetwork, 48, test_actor(3));
    ASSERT_NE(ptr, nullptr);

    auto* header = mem::AllocHeader::from_user_data(ptr);
    EXPECT_EQ(header->region(), mem::RegionType::kNetwork);
    EXPECT_TRUE(header->is_fallback());
    EXPECT_EQ(header->user_size(), 64u);

    mem::ThreadLocalAllocator freeing_thread;
    mem::set_thread_allocator(&freeing_thread);
    mem::deallocate(ptr);
}

TEST_F(MemoryRegionAccountingTest, RegionHardLimitRejectsBeforeSlabGrowth) {
    mem::ThreadLocalAllocator tla;
    mem::set_thread_allocator(&tla);

    auto& regions = mem::MemoryRegionRegistry::instance();
    const auto before = regions.snapshot(mem::RegionType::kNetwork);

    mem::RegionLimit limit{};
    limit.hard_limit_bytes = before.active_bytes + 64;
    limit.high_watermark_ratio = 0.50f;
    regions.configure_region(mem::RegionType::kNetwork, limit);

    void* first = mem::allocate(mem::RegionType::kNetwork, 64, test_actor(4));
    ASSERT_NE(first, nullptr);

    void* rejected = mem::allocate(mem::RegionType::kNetwork, 64, test_actor(4));
    EXPECT_EQ(rejected, nullptr);

    const auto after_reject = regions.snapshot(mem::RegionType::kNetwork);
    EXPECT_EQ(after_reject.rejected_alloc_count,
              before.rejected_alloc_count + 1);
    EXPECT_EQ(after_reject.active_bytes, before.active_bytes + 64);

    mem::deallocate(first);
    regions.configure_region(mem::RegionType::kNetwork, mem::RegionLimit{});
}
```

- [ ] **Step 2: Register the new test target**

Append `test_memory_region_accounting.cpp` to the source list in
`tests/unit/mem/CMakeLists.txt`:

```cmake
add_executable(test_unit_mem
    test_size_class.cpp
    test_alloc_header.cpp
    test_freelist.cpp
    test_segment_provider.cpp
    test_slab_cache.cpp
    test_thread_local_allocator.cpp
    test_memory_stress.cpp
    test_memory_tracker.cpp
    test_telemetry_ring_buffer.cpp
    test_memory_poisoning.cpp
    test_hibernation.cpp
    test_guard_page.cpp
    test_compaction.cpp
    test_allocator_benchmark.cpp
    test_std_allocator.cpp
    test_memory_region_accounting.cpp
)
```

- [ ] **Step 3: Verify the test fails for the current gap**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build test_unit_mem
```

Expected: build failure because `MemoryRegionRegistry`, `RegionLimit`, `AllocHeader::region()`, `AllocHeader::is_fallback()`, and `ThreadLocalAllocator::stats(RegionType, SizeClass)` do not exist yet.

## Task 2: Add Region And Provenance Metadata To AllocHeader

**Files:**
- Modify: `include/hpactor/mem/alloc_header.hpp`
- Modify: `tests/unit/mem/test_alloc_header.cpp`

- [ ] **Step 1: Add region flag helpers without changing header size**

Add `#include <hpactor/mem/memory_region.hpp>` and these helpers inside `namespace hpactor::mem` before `AllocHeader`:

```cpp
inline constexpr uint16_t kAllocRegionMask = 0x0007;
inline constexpr uint16_t kAllocFallbackFlag = 0x0008;

inline constexpr uint16_t flags_for_region(RegionType region) noexcept {
    return static_cast<uint16_t>(region) & kAllocRegionMask;
}
```

Update `AllocHeader::stamp`:

```cpp
static AllocHeader* stamp(void* block, SizeClass sc, ActorId owner,
                          RegionType region = RegionType::kInternal,
                          bool fallback = false) noexcept {
    auto* h = static_cast<AllocHeader*>(block);
    h->owner_id = owner.value();
    h->incarnation = 0;
    h->magic = kAllocMagic;
    h->size_class = static_cast<uint8_t>(sc);
    h->generation = 0;
    h->flags = flags_for_region(region);
    if (fallback) {
        h->flags |= kAllocFallbackFlag;
    }
    h->_padding = 0;
    h->timestamp = 0;
    return h;
}
```

Add these methods to `AllocHeader`:

```cpp
RegionType region() const noexcept {
    return static_cast<RegionType>(flags & kAllocRegionMask);
}

void set_region(RegionType region) noexcept {
    flags = static_cast<uint16_t>((flags & ~kAllocRegionMask)
                                  | flags_for_region(region));
}

bool is_fallback() const noexcept {
    return (flags & kAllocFallbackFlag) != 0;
}

void mark_fallback() noexcept {
    flags |= kAllocFallbackFlag;
}
```

- [ ] **Step 2: Extend the existing alloc header test**

Add assertions after the existing stamp checks:

```cpp
EXPECT_EQ(hdr->region(), RegionType::kInternal);
EXPECT_FALSE(hdr->is_fallback());

AllocHeader* msg_hdr =
    AllocHeader::stamp(buffer, SizeClass::k128B, hpactor::ActorId{42},
                       RegionType::kMessage, true);
EXPECT_EQ(msg_hdr->region(), RegionType::kMessage);
EXPECT_TRUE(msg_hdr->is_fallback());
static_assert(sizeof(AllocHeader) == 32, "AllocHeader must stay 32 bytes");
```

- [ ] **Step 3: Verify header tests pass**

Run:

```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem \
  --gtest_filter=AllocHeaderTest.*
```

Expected: all `AllocHeaderTest` cases pass.

## Task 3: Track Slab Ownership For Cross-Thread Frees

**Files:**
- Modify: `include/hpactor/mem/segment_provider.hpp`
- Modify: `src/mem/segment_provider.cpp`
- Modify: `include/hpactor/mem/slab_cache.hpp`
- Modify: `src/mem/slab_cache.cpp`

- [ ] **Step 1: Add slab owner metadata API**

In `segment_provider.hpp`, include `memory_region.hpp` and add:

```cpp
struct SlabInfo {
    void* segment_base{nullptr};
    void* slab_base{nullptr};
    size_t segment_size{0};
    size_t slab_size{0};
    void* owner_cache{nullptr};
    RegionType region{RegionType::kInternal};
    SizeClass size_class{SizeClass::k32B};
    bool found{false};
};

void register_slab_owner(void* slab, size_t slab_size, void* owner_cache,
                         RegionType region, SizeClass sc);

SlabInfo lookup_slab(void* ptr) const;
```

Change the private map value from a raw segment index to a record:

```cpp
struct SlabRecord {
    uint32_t segment_index{0};
    size_t slab_size{0};
    void* owner_cache{nullptr};
    RegionType region{RegionType::kInternal};
    SizeClass size_class{SizeClass::k32B};
};

std::unordered_map<void*, SlabRecord> slab_records_;
```

- [ ] **Step 2: Update segment acquisition and release**

In `segment_provider.cpp`, replace `addr_to_segment_` uses with `slab_records_`. `acquire_slab()` and `carve_from_segment()` should insert a `SlabRecord` with `owner_cache == nullptr`; `SlabCache::refill()` fills it immediately by calling `register_slab_owner()`.

Implement `lookup_slab()` as an exact lookup first, then an interior-pointer scan:

```cpp
SegmentProvider::SlabInfo SegmentProvider::lookup_slab(void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& [slab_base, record] : slab_records_) {
        auto* begin = static_cast<std::byte*>(slab_base);
        auto* end = begin + record.slab_size;
        if (ptr >= begin && ptr < end && record.segment_index < segments_.size()) {
            return SlabInfo{segments_[record.segment_index].base,
                            slab_base,
                            segments_[record.segment_index].size,
                            record.slab_size,
                            record.owner_cache,
                            record.region,
                            record.size_class,
                            true};
        }
    }
    return SlabInfo{};
}
```

- [ ] **Step 3: Give SlabCache a region and register itself**

In `slab_cache.hpp`, update the constructor and add accessors:

```cpp
explicit SlabCache(SizeClass sc, RegionType region = RegionType::kInternal)
    : size_class_(sc), region_(region) {}

RegionType region() const noexcept { return region_; }

RegionType region_;
```

In `SlabCache::refill()`:

```cpp
SegmentProvider::instance().register_slab_owner(
    current_slab_, slab_size_, this, region_, size_class_);
```

In `SlabCache::allocate()` stamp the region:

```cpp
auto* hdr = AllocHeader::stamp(raw, size_class_, owner, region_);
```

- [ ] **Step 4: Verify segment and slab tests**

Run:

```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem \
  --gtest_filter='SegmentProviderTest.*:SlabCacheTest.*'
```

Expected: both tests pass.

## Task 4: Make ThreadLocalAllocator Region-Aware

**Files:**
- Modify: `include/hpactor/mem/thread_local_allocator.hpp`
- Modify: `src/mem/thread_local_allocator.cpp`
- Modify: `tests/unit/mem/test_thread_local_allocator.cpp`

- [ ] **Step 1: Replace one-dimensional caches with region-by-size-class caches**

Use this private member:

```cpp
using CacheRow = std::array<SlabCache*, kNumSizeClasses>;
std::array<CacheRow, kNumRegionTypes> caches_{};
```

Add region-aware public methods while preserving existing call sites:

```cpp
void* allocate(RegionType region, SizeClass sc, ActorId owner) noexcept;
void* allocate(RegionType region, size_t user_bytes, ActorId owner) noexcept;

void* allocate(SizeClass sc, ActorId owner) noexcept {
    return allocate(RegionType::kInternal, sc, owner);
}

void* allocate_bytes(size_t user_bytes, ActorId owner) noexcept {
    return allocate(RegionType::kInternal, user_bytes, owner);
}

const SlabCache::Stats& stats(RegionType region, SizeClass sc) const noexcept;
```

- [ ] **Step 2: Initialize one SlabCache per region and size class**

In the constructor:

```cpp
for (uint8_t r = 0; r < kNumRegionTypes; ++r) {
    for (uint8_t s = 0; s < kNumSizeClasses; ++s) {
        caches_[r][s] = new SlabCache(static_cast<SizeClass>(s),
                                      static_cast<RegionType>(r));
    }
}
```

In the destructor, delete the same matrix.

- [ ] **Step 3: Route deallocation to the origin slab cache**

Implement `ThreadLocalAllocator::deallocate()` using `SegmentProvider::lookup_slab()`:

```cpp
void ThreadLocalAllocator::deallocate(void* user_ptr) noexcept {
    auto* hdr = AllocHeader::from_user_data(user_ptr);
    auto slab = SegmentProvider::instance().lookup_slab(hdr);
    if (slab.found && slab.owner_cache) {
        static_cast<SlabCache*>(slab.owner_cache)->deallocate(user_ptr);
        return;
    }

    const auto region = hdr->region();
    const auto sc = static_cast<SizeClass>(hdr->size_class);
    caches_[static_cast<uint8_t>(region)][static_cast<uint8_t>(sc)]
        ->deallocate(user_ptr);
}
```

- [ ] **Step 4: Extend thread-local allocator tests**

Add a region-specific allocation case:

```cpp
void* msg = tla.allocate(RegionType::kMessage, SizeClass::k64B,
                         hpactor::ActorId{55});
auto* hdr = AllocHeader::from_user_data(msg);
EXPECT_EQ(hdr->region(), RegionType::kMessage);
tla.deallocate(msg);
```

- [ ] **Step 5: Verify allocator tests**

Run:

```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem \
  --gtest_filter='ThreadLocalAllocatorTest.*:MemoryRegionAccountingTest.*'
```

Expected: `ThreadLocalAllocatorTest.*` passes. `MemoryRegionAccountingTest.*` still fails because region registry and fallback accounting are not wired yet.

## Task 5: Implement MemoryRegionRegistry Admission And Snapshots

**Files:**
- Modify: `include/hpactor/mem/memory_region.hpp`
- Create: `src/mem/memory_region.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add pressure and limit types**

Add `#include <array>` to `memory_region.hpp`, then add these types:

```cpp
enum class MemoryPressureState : uint8_t {
    kNormal = 0,
    kHigh = 1,
    kHardLimit = 2,
};

struct RegionLimit {
    uint64_t hard_limit_bytes{0}; // 0 means unlimited
    float high_watermark_ratio{1.0f};
};

struct RegionSnapshot {
    uint64_t total_allocated{0};
    uint64_t total_freed{0};
    uint64_t active_bytes{0};
    uint64_t high_water_mark{0};
    uint64_t alloc_count{0};
    uint64_t free_count{0};
    uint64_t corruption_events{0};
    uint64_t rejected_alloc_count{0};
    RegionLimit limit{};
    MemoryPressureState pressure{MemoryPressureState::kNormal};
};
```

Add `rejected_alloc_count` to `RegionStats`.

- [ ] **Step 2: Add the registry API**

```cpp
class MemoryRegionRegistry {
  public:
    static MemoryRegionRegistry& instance();

    bool try_reserve(RegionType region, size_t charged_bytes) noexcept;
    void commit_alloc(RegionType region, size_t charged_bytes) noexcept;
    void cancel_reservation(RegionType region, size_t charged_bytes) noexcept;
    void record_free(RegionType region, size_t charged_bytes) noexcept;
    void record_corruption(RegionType region) noexcept;

    void configure_region(RegionType region, RegionLimit limit) noexcept;
    RegionLimit limit(RegionType region) const noexcept;
    RegionSnapshot snapshot(RegionType region) const noexcept;

  private:
    MemoryRegionRegistry();
    static uint8_t index(RegionType region) noexcept;
    MemoryPressureState pressure_for(uint8_t idx, uint64_t active) const noexcept;

    std::array<RegionStats, kNumRegionTypes> stats_{};
    std::array<RegionLimit, kNumRegionTypes> limits_{};
};
```

- [ ] **Step 3: Implement atomic reservation with hard-limit rejection**

In `memory_region.cpp`:

```cpp
bool MemoryRegionRegistry::try_reserve(RegionType region,
                                       size_t charged_bytes) noexcept {
    const uint8_t idx = index(region);
    auto& stats = stats_[idx];
    const auto limit_cfg = limits_[idx];

    uint64_t current = stats.active_bytes.load(std::memory_order_relaxed);
    for (;;) {
        const uint64_t projected = current + charged_bytes;
        if (limit_cfg.hard_limit_bytes != 0 &&
            projected > limit_cfg.hard_limit_bytes) {
            stats.rejected_alloc_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (stats.active_bytes.compare_exchange_weak(
                current, projected, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            uint64_t peak = stats.high_water_mark.load(std::memory_order_relaxed);
            while (projected > peak &&
                   !stats.high_water_mark.compare_exchange_weak(
                       peak, projected, std::memory_order_relaxed,
                       std::memory_order_relaxed)) {}
            return true;
        }
    }
}
```

`commit_alloc()` increments `total_allocated` and `alloc_count`. `cancel_reservation()` subtracts from `active_bytes`. `record_free()` subtracts from `active_bytes`, increments `total_freed`, and increments `free_count`.

- [ ] **Step 4: Add the source file to hpactor_lib**

In `src/CMakeLists.txt`, add:

```cmake
    mem/memory_region.cpp
```

Place it next to `mem/memory_config.cpp`.

- [ ] **Step 5: Verify registry compiles**

Run:

```bash
ninja -C build hpactor_lib
```

Expected: `hpactor_lib` builds.

## Task 6: Wire Global Allocation To Region Admission, Tracker, And Safe Fallback

**Files:**
- Modify: `include/hpactor/mem/memory_config.hpp`
- Modify: `src/mem/memory_config.cpp`

- [ ] **Step 1: Move non-trivial allocation helpers out of inline-only code**

Keep declarations in `memory_config.hpp`:

```cpp
void* allocate(RegionType region, size_t user_bytes, ActorId owner) noexcept;
void* allocate_class(SizeClass sc, ActorId owner) noexcept;
void deallocate(void* user_ptr) noexcept;
```

Add `#include <hpactor/mem/memory_telemetry.hpp>`,
`#include <hpactor/mem/memory_tracker.hpp>`,
`#include <hpactor/mem/segment_provider.hpp>`, and
`#include <hpactor/mem/slab_cache.hpp>` in `memory_config.cpp`.

Add internal helpers in `memory_config.cpp`:

```cpp
namespace {

void* fallback_allocate(mem::RegionType region, mem::SizeClass sc,
                        hpactor::ActorId owner) noexcept {
    const size_t bytes = mem::block_size(sc);
    void* raw = std::malloc(bytes); // NOLINT
    if (!raw) {
        return nullptr;
    }
    auto* header = mem::AllocHeader::stamp(raw, sc, owner, region, true);
    mem::CanaryFooter::stamp(header, bytes);
    return header->user_data();
}

void fallback_deallocate(mem::AllocHeader* header) noexcept {
    std::free(header); // NOLINT
}

} // namespace
```

- [ ] **Step 2: Implement `mem::allocate()` as the only admission point**

```cpp
void* allocate(RegionType region, size_t user_bytes, ActorId owner) noexcept {
    const SizeClass sc = class_for_size(user_bytes);
    const size_t charged_bytes = size_for_class(sc);
    auto& regions = MemoryRegionRegistry::instance();

    if (!regions.try_reserve(region, charged_bytes)) {
        return nullptr;
    }

    void* ptr = nullptr;
    if (t_tla) {
        ptr = t_tla->allocate(region, sc, owner);
    } else {
        ptr = fallback_allocate(region, sc, owner);
    }

    if (!ptr) {
        regions.cancel_reservation(region, charged_bytes);
        return nullptr;
    }

    regions.commit_alloc(region, charged_bytes);
    if constexpr (kMemoryTrackingEnabled) {
        MemoryTracker::instance().record_alloc(owner, charged_bytes);
    }
    MemoryTelemetry::instance().record_alloc(owner, region, sc, charged_bytes);
    return ptr;
}
```

`allocate_class(sc, owner)` should call `allocate(RegionType::kInternal, size_for_class(sc), owner)`.

- [ ] **Step 3: Implement `mem::deallocate()` before the header is marked freed**

```cpp
void deallocate(void* user_ptr) noexcept {
    if (!user_ptr) {
        return;
    }

    auto* header = AllocHeader::from_user_data(user_ptr);
    const ActorId owner{header->owner_id};
    const RegionType region = header->region();
    const auto sc = static_cast<SizeClass>(header->size_class);
    const size_t charged_bytes = size_for_class(sc);
    const bool fallback = header->is_fallback();

    if constexpr (kMemoryTrackingEnabled) {
        MemoryTracker::instance().record_free(owner, charged_bytes);
    }
    MemoryRegionRegistry::instance().record_free(region, charged_bytes);
    MemoryTelemetry::instance().record_free(owner, region, sc, charged_bytes);

    if (fallback) {
        header->magic = kFreedMagic;
        fallback_deallocate(header);
        return;
    }

    if (t_tla) {
        t_tla->deallocate(user_ptr);
        return;
    }

    auto slab = SegmentProvider::instance().lookup_slab(header);
    if (slab.found && slab.owner_cache) {
        static_cast<SlabCache*>(slab.owner_cache)->deallocate(user_ptr);
    }
}
```

- [ ] **Step 4: Verify the gap test progresses**

Run:

```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem \
  --gtest_filter=MemoryRegionAccountingTest.*
```

Expected: the new test passes or fails only on missing telemetry APIs from Task 7.

## Task 7: Add Allocator Telemetry Sampling

**Files:**
- Create: `include/hpactor/mem/memory_telemetry.hpp`
- Create: `src/mem/memory_telemetry.cpp`
- Modify: `include/hpactor/mem/telemetry_ring_buffer.hpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/mem/test_telemetry_ring_buffer.cpp`

- [ ] **Step 1: Add event type constants**

In `telemetry_ring_buffer.hpp`:

```cpp
enum class AllocationEventType : uint8_t {
    kAlloc = 0,
    kFree = 1,
    kCorruption = 2,
    kHibernateIn = 3,
    kHibernateOut = 4,
    kRejected = 5,
};
```

- [ ] **Step 2: Add `MemoryTelemetry`**

```cpp
// include/hpactor/mem/memory_telemetry.hpp
#pragma once

#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/telemetry_ring_buffer.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <cstddef>

namespace hpactor::mem {

class MemoryTelemetry {
  public:
    static constexpr uint32_t kDefaultSampleRate = 128;

    static MemoryTelemetry& instance();

    void set_sample_rate(uint32_t sample_rate) noexcept;
    uint32_t sample_rate() const noexcept;

    void record_alloc(ActorId actor, RegionType region, SizeClass sc,
                      size_t charged_bytes) noexcept;
    void record_free(ActorId actor, RegionType region, SizeClass sc,
                     size_t charged_bytes) noexcept;

    template <typename F>
    void drain(F&& callback) {
        ring_.drain(std::forward<F>(callback));
    }

  private:
    void record(ActorId actor, RegionType region, SizeClass sc,
                size_t charged_bytes, AllocationEventType type) noexcept;

    TelemetryRingBuffer<> ring_{};
    std::atomic<uint32_t> sample_rate_{kDefaultSampleRate};
    std::atomic<uint64_t> sequence_{0};
};

} // namespace hpactor::mem
```

- [ ] **Step 3: Implement sampling**

In `memory_telemetry.cpp`, `sample_rate == 0` means no events, `sample_rate == 1` means every event, and `N` means one event per `N` allocator events. Use `std::chrono::steady_clock` nanoseconds for `timestamp`.

- [ ] **Step 4: Add telemetry allocation test**

Extend `tests/unit/mem/test_telemetry_ring_buffer.cpp` with a GoogleTest case:

```cpp
TEST(MemoryTelemetryTest, EmitsSampledAllocatorEvents) {
    auto& telemetry = MemoryTelemetry::instance();
    telemetry.set_sample_rate(1);

    ThreadLocalAllocator tla;
    set_thread_allocator(&tla);

    void* ptr = allocate(RegionType::kMessage, 32, ActorId{901000});
    ASSERT_NE(ptr, nullptr);
    deallocate(ptr);

    int alloc_events = 0;
    int free_events = 0;
    telemetry.drain([&](const AllocationEvent& event) {
        if (event.actor_id == 901000 &&
            event.event_type == static_cast<uint8_t>(AllocationEventType::kAlloc)) {
            ++alloc_events;
        }
        if (event.actor_id == 901000 &&
            event.event_type == static_cast<uint8_t>(AllocationEventType::kFree)) {
            ++free_events;
        }
    });
    EXPECT_EQ(alloc_events, 1);
    EXPECT_EQ(free_events, 1);

    telemetry.set_sample_rate(MemoryTelemetry::kDefaultSampleRate);
    set_thread_allocator(nullptr);
}
```

- [ ] **Step 5: Add source file and verify**

In `src/CMakeLists.txt`, add:

```cmake
    mem/memory_telemetry.cpp
```

Run:

```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem \
  --gtest_filter='TelemetryRingBufferTest.*:MemoryTelemetryTest.*:MemoryRegionAccountingTest.*'
```

Expected: both tests pass.

## Task 8: Route Standard Allocator And Actor Spawn Through Region-Aware Allocation

**Files:**
- Modify: `include/hpactor/mem/std_allocator.hpp`
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `tests/unit/mem/test_std_allocator.cpp`
- Modify: `tests/integration/actor/test_actor_system.cpp`

- [ ] **Step 1: Update `MemStdAllocator` to preserve `RegionType`**

Replace direct `t_tla->allocate_bytes()` calls with:

```cpp
ptr = mem::allocate(region_, bytes, owner);
```

Replace direct `t_tla->deallocate()` and raw small-allocation frees with:

```cpp
mem::deallocate(ptr);
```

Oversized allocations still use raw `malloc/free` in this task. The fat-block mmap and guard-page path from the architecture stays outside this plan.

- [ ] **Step 2: Route programmatic actor spawn through kActor allocation**

In `ActorSystem::spawn()`:

```cpp
auto actor = mem::allocate_shared<T>(id, mem::RegionType::kActor,
                                     nullptr, *this,
                                     std::forward<Args>(args)...);
```

Keep the existing `std::shared_ptr<AbstractActor>` storage unchanged. This routes both the actor object and `allocate_shared` control block through the `kActor` region for normal programmatic spawns.

- [ ] **Step 3: Add std allocator region accounting assertion**

Add a `MemStdAllocatorTest` fixture case to `tests/unit/mem/test_std_allocator.cpp`:

```cpp
TEST_F(MemStdAllocatorTest, RegionAccountingTracksContainerStorage) {
    auto before = MemoryRegionRegistry::instance().snapshot(RegionType::kActor);

    {
        std::vector<int, MemStdAllocator<int>> values(
            MemStdAllocator<int>(ActorId{902000}, RegionType::kActor));
        values.push_back(1);
        values.push_back(2);
    }

    auto after = MemoryRegionRegistry::instance().snapshot(RegionType::kActor);
    EXPECT_GE(after.alloc_count, before.alloc_count + 1);
    EXPECT_GE(after.free_count, before.free_count + 1);
}
```

- [ ] **Step 4: Add actor spawn accounting assertion**

In `tests/integration/actor/test_actor_system.cpp`, add these includes:

```cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/mem/memory_region.hpp>

#include <chrono>
```

Add this GoogleTest case:

```cpp
TEST(ActorSystemTest, ActorSpawnUsesActorRegionAccounting) {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;

    hpactor::ActorSystem system(cfg);

    auto before = hpactor::mem::MemoryRegionRegistry::instance().snapshot(
        hpactor::mem::RegionType::kActor);

    auto actor = system.spawn<hpactor::EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(actor));

    auto after = hpactor::mem::MemoryRegionRegistry::instance().snapshot(
        hpactor::mem::RegionType::kActor);
    EXPECT_GE(after.alloc_count, before.alloc_count + 1);

    hpactor::ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);
    system.shutdown(opts);
}
```

- [ ] **Step 5: Verify**

Run:

```bash
ninja -C build test_unit_mem test_integration_actor
./build/tests/unit/mem/test_unit_mem \
  --gtest_filter=MemStdAllocatorTest.RegionAccountingTracksContainerStorage
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter=ActorSystemTest.ActorSpawnUsesActorRegionAccounting
```

Expected: both tests pass.

## Task 9: Full Verification

**Files:**
- No source changes

- [ ] **Step 1: Run focused memory and actor tests**

Run:

```bash
ctest -R "MemoryRegionAccountingTest|AllocHeaderTest|SegmentProviderTest|SlabCacheTest|ThreadLocalAllocatorTest|MemoryTrackerTest|TelemetryRingBufferTest|MemoryTelemetryTest|MemStdAllocatorTest|ActorSystemTest" --output-on-failure
```

Expected: all listed tests pass.

- [ ] **Step 2: Run all tests**

Run:

```bash
ctest --output-on-failure --parallel 8
```

Expected: all tests pass.

- [ ] **Step 3: Check formatting and patch hygiene**

Run:

```bash
git diff --check
```

Expected: no whitespace errors.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mem/alloc_header.hpp \
        include/hpactor/mem/segment_provider.hpp \
        src/mem/segment_provider.cpp \
        include/hpactor/mem/slab_cache.hpp \
        src/mem/slab_cache.cpp \
        include/hpactor/mem/thread_local_allocator.hpp \
        src/mem/thread_local_allocator.cpp \
        include/hpactor/mem/memory_region.hpp \
        src/mem/memory_region.cpp \
        include/hpactor/mem/memory_config.hpp \
        src/mem/memory_config.cpp \
        include/hpactor/mem/telemetry_ring_buffer.hpp \
        include/hpactor/mem/memory_telemetry.hpp \
        src/mem/memory_telemetry.cpp \
        include/hpactor/mem/std_allocator.hpp \
        include/hpactor/core/actor_system.hpp \
        src/CMakeLists.txt \
        tests/unit/mem/CMakeLists.txt \
        tests/unit/mem/test_memory_region_accounting.cpp \
        tests/unit/mem/test_alloc_header.cpp \
        tests/unit/mem/test_thread_local_allocator.cpp \
        tests/unit/mem/test_telemetry_ring_buffer.cpp \
        tests/unit/mem/test_std_allocator.cpp \
        tests/integration/actor/test_actor_system.cpp
git commit -m "feat(mem): enforce typed region accounting and pressure admission"
```

## Acceptance Evidence

The implementation is complete when:

- `mem::allocate(RegionType::kMessage, ...)` stamps `AllocHeader::region() == kMessage`.
- `kActor`, `kMessage`, `kNetwork`, and other regions have separate `SlabCache` instances per size class.
- `MemoryRegionRegistry::snapshot(region)` reports active bytes, high-water bytes, allocation count, free count, and rejected allocation count.
- `MemoryTracker::snapshot(actor)` changes automatically after allocator calls; tests no longer need to call `record_alloc()` manually to prove runtime accounting.
- A slab allocation freed by a different thread returns to the origin `SlabCache`.
- A fallback allocation made with no `ThreadLocalAllocator` can be safely freed on a thread that has a `ThreadLocalAllocator`.
- Region hard limits reject before acquiring another slab.
- `MemoryTelemetry` emits sampled allocation and free events.
- Focused tests and the full test suite pass.

## Explicit Non-Scope

These are still valid architecture goals, but they should be implemented after this plan:

- Fat-block direct `mmap` path with guard pages for allocations larger than 4KB.
- Actor hibernation protocol: idle timeout, explicit `context()->hibernate()`, memory-pressure actor selection, and reactivation.
- Relocating compaction that updates actor registry pointers.
- TOML/runtime config for region hard limits and high-water ratios.
- Subsystem-specific reclaim callbacks such as rejecting spawn, closing idle network connections, or hibernating least-recently-active actors.
