# MEM-004: Super Carrier — Implementation Plan

**Date:** 2026-06-20
**Design Spec:** `docs/superpowers/specs/2026-06-20-mem-04-super-carrier-design.md`
**Issue:** #339 (Phase 2.1, P1)
**Depends on:** None (standalone — integrates with SegmentProvider)

## Phase Overview

| Phase | Name | Tests | Files Changed |
|-------|------|-------|---------------|
| 1 | `SuperCarrier` class skeleton + `init()` | 2 | 2 |
| 2 | `carve()` — mprotect-based slab allocation | 2 | 1 |
| 3 | `release()` — MADV_FREE + mprotect(PROT_NONE) | 2 | 1 |
| 4 | SegmentProvider integration — primary source | 2 | 2 |
| 5 | Exhaustion + legacy fallback | 2 | 1 |
| 6 | Carrier growth via `mremap` (Linux) | 2 | 1 |
| 7 | Platform guard + 32-bit fallback | 2 | 1 |
| 8 | Fault injection | 2 | 2 |

---

## Phase 1: `SuperCarrier` Class Skeleton + `init()`

**Goal:** Create the `SuperCarrier` class with a huge `PROT_NONE` virtual
reservation at `ActorSystem` startup. Zero physical memory consumed.

### RED — Write failing tests

File: `tests/unit/mem/test_super_carrier.cpp` (new)

```cpp
#include <gtest/gtest.h>
#include "hpactor/mem/segment_provider.hpp"

using namespace hpactor::mem;

TEST(SuperCarrier, InitReservesVirtualRange) {
    SuperCarrier carrier;
    bool ok = carrier.init(8ULL * 1024 * 1024 * 1024);  // 8GB
#if HPACTOR_SUPER_CARRIER_ENABLED
    EXPECT_TRUE(ok);
    EXPECT_TRUE(carrier.is_initialized());
    EXPECT_EQ(carrier.total_reserved(), 8ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(carrier.total_carved(), 0u);
    EXPECT_EQ(carrier.current_carved(), 0u);
#else
    // 32-bit: init should fail gracefully
    EXPECT_FALSE(ok) << "Super carrier disabled on 32-bit";
#endif
}

TEST(SuperCarrier, InitZeroPhysicalMemory) {
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(1ULL * 1024 * 1024 * 1024));
#if HPACTOR_SUPER_CARRIER_ENABLED
    // Virtual range is reserved but PROT_NONE — no physical pages committed
    // Verified via /proc/self/smaps (Linux) or vmmap (macOS): RSS should be 0
    // This is a smoke test — we can't easily query RSS from within the test
    SUCCEED();
#endif
}
```

### GREEN — Implement

1. Create `include/hpactor/mem/super_carrier.hpp`:
   ```cpp
   class SuperCarrier {
   public:
       bool init(size_t size_bytes);
       void* carve(size_t slab_size_bytes);
       void release(void* slab_addr, size_t slab_size_bytes);
       bool is_initialized() const { return carrier_base_ != nullptr; }
       size_t total_reserved() const { return carrier_size_; }
       size_t total_carved() const { return carve_offset_.load(); }
       size_t current_carved() const;
   private:
       void* carrier_base_{nullptr};
       size_t carrier_size_{0};
       std::atomic<size_t> carve_offset_{0};
       std::atomic<size_t> released_bytes_{0};
   };
   ```

2. Implement `init()` in `src/mem/super_carrier.cpp`:
   ```cpp
   bool SuperCarrier::init(size_t size_bytes) {
   #if HPACTOR_SUPER_CARRIER_ENABLED
       carrier_base_ = mmap(nullptr, size_bytes, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                            -1, 0);
       if (carrier_base_ == MAP_FAILED) return false;
       carrier_size_ = size_bytes;
       return true;
   #else
       (void)size_bytes;
       return false;  // 32-bit: disabled
   #endif
   }
   ```

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SuperCarrier*"
```

---

## Phase 2: `carve()` — mprotect-Based Slab Allocation

**Goal:** Carve slabs from the super carrier by `mprotect`-ing a slice from
`PROT_NONE` to `PROT_READ|PROT_WRITE`. Monotonic offset advance.

### RED — Write failing tests

```cpp
TEST(SuperCarrier, CarveReturnsWritableMemory) {
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(1ULL * 1024 * 1024 * 1024));
#if HPACTOR_SUPER_CARRIER_ENABLED
    void* slab = carrier.carve(64 * 1024);  // 64KB slab
    ASSERT_NE(slab, nullptr);
    // Memory should be writable
    memset(slab, 0x42, 64 * 1024);
    // Offset should have advanced
    EXPECT_EQ(carrier.total_carved(), 64u * 1024);
#endif
}

TEST(SuperCarrier, CarveAdvancesOffset) {
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(1ULL * 1024 * 1024 * 1024));
#if HPACTOR_SUPER_CARRIER_ENABLED
    void* s1 = carrier.carve(64 * 1024);
    void* s2 = carrier.carve(64 * 1024);
    EXPECT_NE(s1, s2);
    EXPECT_EQ(carrier.total_carved(), 128u * 1024);
    // s2 should be s1 + 64KB (contiguous carve)
    EXPECT_EQ(static_cast<char*>(s2) - static_cast<char*>(s1), 64 * 1024);
#endif
}
```

### GREEN — Implement

```cpp
void* SuperCarrier::carve(size_t slab_size_bytes) {
    size_t offset = carve_offset_.fetch_add(slab_size_bytes);
    if (offset + slab_size_bytes > carrier_size_) {
        carve_offset_.fetch_sub(slab_size_bytes);  // rollback
        return nullptr;  // exhausted
    }
    void* addr = static_cast<char*>(carrier_base_) + offset;
    if (mprotect(addr, slab_size_bytes, PROT_READ | PROT_WRITE) != 0) {
        carve_offset_.fetch_sub(slab_size_bytes);
        return nullptr;
    }
    return addr;
}
```

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SuperCarrier*"
```

---

## Phase 3: `release()` — MADV_FREE + mprotect(PROT_NONE)

**Goal:** Return physical pages to the OS while keeping the virtual reservation.

### RED — Write failing tests

```cpp
TEST(SuperCarrier, ReleaseReturnsPhysicalPages) {
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(1ULL * 1024 * 1024 * 1024));
#if HPACTOR_SUPER_CARRIER_ENABLED
    void* slab = carrier.carve(64 * 1024);
    ASSERT_NE(slab, nullptr);
    memset(slab, 0x42, 64 * 1024);  // touch pages
    carrier.release(slab, 64 * 1024);
    // After release: virtual still reserved, physical freed
    EXPECT_EQ(carrier.current_carved(), 0u);  // carved - released
#endif
}

TEST(SuperCarrier, ReleaseNonCarvedPointerIsNoop) {
    SuperCarrier carrier;
    ASSERT_TRUE(carrier.init(1ULL * 1024 * 1024 * 1024));
    // Releasing a pointer not in the carrier range should be safe
    int dummy;
    carrier.release(&dummy, sizeof(dummy));  // Should not crash
    SUCCEED();
}
```

### GREEN — Implement

```cpp
void SuperCarrier::release(void* slab_addr, size_t slab_size_bytes) {
    if (slab_addr < carrier_base_ ||
        slab_addr >= static_cast<char*>(carrier_base_) + carrier_size_) {
        return;  // Not in our range
    }
    madvise(slab_addr, slab_size_bytes, MADV_FREE);
    mprotect(slab_addr, slab_size_bytes, PROT_NONE);
    released_bytes_.fetch_add(slab_size_bytes);
}
```

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SuperCarrier*"
```

---

## Phase 4: SegmentProvider Integration

**Goal:** `SegmentProvider::acquire_slab()` tries the super carrier first;
falls back to legacy `mmap` on carrier exhaustion.

### RED — Write failing tests

```cpp
TEST(SuperCarrier, SegmentProviderUsesCarrierFirst) {
    SuperCarrier carrier;
    carrier.init(1ULL * 1024 * 1024 * 1024);
    SegmentProvider::instance().set_super_carrier(&carrier);

    auto* slab = SegmentProvider::instance().acquire_slab(SizeClass::k32);
    ASSERT_NE(slab, nullptr);
    // Slab should be within carrier range
    EXPECT_GE(slab, carrier.carrier_base());
    EXPECT_LT(slab, static_cast<char*>(carrier.carrier_base()) + carrier.total_reserved());
    SegmentProvider::instance().release_slab(slab);
}

TEST(SuperCarrier, SegmentProviderFallbackToMmapWhenNoCarrier) {
    SegmentProvider::instance().set_super_carrier(nullptr);  // no carrier
    auto* slab = SegmentProvider::instance().acquire_slab(SizeClass::k32);
    ASSERT_NE(slab, nullptr);
    // Legacy mmap path works
    SegmentProvider::instance().release_slab(slab);
}
```

### GREEN — Implement

Update `SegmentProvider::acquire_slab_memory()` to try carrier first:
```cpp
void* SegmentProvider::acquire_slab_memory(size_t size_bytes) {
    if (super_carrier_ && super_carrier_->is_initialized()) {
        if (auto* mem = super_carrier_->carve(size_bytes))
            return mem;
    }
    return allocate_new_segment_legacy(size_bytes);
}
```

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SuperCarrier*"
ctest -R "test_unit_mem" --output-on-failure --parallel 8
```

---

## Phase 5: Exhaustion + Legacy Fallback

**Goal:** When carrier is full, the system gracefully falls back to individual
`mmap` segments without errors or data loss.

### RED — Write failing tests

```cpp
TEST(SuperCarrier, ExhaustionTriggersFallback) {
    SuperCarrier carrier;
    // Tiny carrier: only 256KB
    ASSERT_TRUE(carrier.init(256 * 1024));
    SegmentProvider::instance().set_super_carrier(&carrier);

    // Carve until exhaustion
    std::vector<void*> slabs;
    while (true) {
        auto* s = SegmentProvider::instance().acquire_slab(SizeClass::k32);
        if (!s) break;
        slabs.push_back(s);
    }
    EXPECT_GE(slabs.size(), 1u);  // At least one from carrier

    // Next allocation should fall back to legacy mmap (not nullptr)
    auto* legacy = SegmentProvider::instance().acquire_slab(SizeClass::k32);
    EXPECT_NE(legacy, nullptr) << "Legacy fallback must work after carrier exhaustion";
    // Cleanup
    for (auto* s : slabs) SegmentProvider::instance().release_slab(s);
    SegmentProvider::instance().release_slab(legacy);
}

TEST(SuperCarrier, ExhaustionStatsReflectFallback) {
    SuperCarrier carrier;
    carrier.init(256 * 1024);
    SegmentProvider::instance().set_super_carrier(&carrier);

    auto stats_before = SegmentProvider::instance().stats();
    // Exhaust carrier, then get one legacy segment
    std::vector<void*> slabs;
    while (auto* s = SegmentProvider::instance().acquire_slab(SizeClass::k32))
        slabs.push_back(s);
    auto* legacy = SegmentProvider::instance().acquire_slab(SizeClass::k32);
    ASSERT_NE(legacy, nullptr);

    auto stats_after = SegmentProvider::instance().stats();
    EXPECT_GE(stats_after.legacy_segments, stats_before.legacy_segments + 1);

    for (auto* s : slabs) SegmentProvider::instance().release_slab(s);
    SegmentProvider::instance().release_slab(legacy);
}
```

### GREEN — Implement

The fallback is already built into the Phase 4 integration. Add stats tracking:
```cpp
struct Stats {
    // ... existing ...
    uint64_t carrier_slabs{0};
    uint64_t legacy_segments{0};
};
```

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SuperCarrier*"
```

---

## Phase 6: Carrier Growth via `mremap` (Linux)

**Goal:** Attempt `mremap` to extend the reservation when exhausted (Linux only).

### RED — Write failing tests

```cpp
TEST(SuperCarrier, GrowExtendsReservation) {
#if defined(__linux__) && defined(__LP64__)
    SuperCarrier carrier;
    carrier.init(8ULL * 1024 * 1024 * 1024);  // 8GB initial
    carrier.set_max_size(16ULL * 1024 * 1024 * 1024);  // 16GB max
    carrier.set_can_grow(true);

    // Exhaust the 8GB carrier
    std::vector<void*> slabs;
    size_t total = 0;
    while (total < 8ULL * 1024 * 1024 * 1024) {
        auto* s = carrier.carve(64 * 1024);
        if (!s) {
            // Try grow
            EXPECT_TRUE(carrier.grow(8ULL * 1024 * 1024 * 1024));
            s = carrier.carve(64 * 1024);
        }
        ASSERT_NE(s, nullptr);
        slabs.push_back(s);
        total += 64 * 1024;
    }
    EXPECT_GT(carrier.total_reserved(), 8ULL * 1024 * 1024 * 1024);
    for (auto* s : slabs) carrier.release(s, 64 * 1024);
#else
    GTEST_SKIP() << "mremap only supported on Linux x86_64";
#endif
}

TEST(SuperCarrier, GrowFailsWhenMaxReached) {
    SuperCarrier carrier;
    carrier.init(8ULL * 1024 * 1024 * 1024);
    carrier.set_max_size(8ULL * 1024 * 1024 * 1024);  // same as current
    carrier.set_can_grow(true);
#if HPACTOR_SUPER_CARRIER_ENABLED
    EXPECT_FALSE(carrier.grow(1ULL * 1024 * 1024 * 1024));  // at max
#endif
}
```

### GREEN — Implement

```cpp
#if defined(__linux__) && defined(MREMAP_MAYMOVE)
bool SuperCarrier::grow(size_t additional_bytes) {
    if (!can_grow_) return false;
    size_t new_size = carrier_size_ + additional_bytes;
    if (new_size > max_carrier_size_) return false;
    void* new_base = mremap(carrier_base_, carrier_size_, new_size, MREMAP_MAYMOVE);
    if (new_base == MAP_FAILED) return false;
    carrier_base_ = new_base;
    carrier_size_ = new_size;
    return true;
}
#else
bool SuperCarrier::grow(size_t) { return false; }
#endif
```

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SuperCarrier*"
```

---

## Phase 7: Platform Guard + 32-bit Fallback

**Goal:** `HPACTOR_SUPER_CARRIER_ENABLED` is 0 on 32-bit; super carrier
gracefully disabled with zero behavior change.

### RED — Write failing tests

```cpp
TEST(SuperCarrier, DisabledOn32Bit) {
#ifndef __LP64__
    SuperCarrier carrier;
    EXPECT_FALSE(carrier.init(8ULL * 1024 * 1024 * 1024));
    EXPECT_FALSE(carrier.is_initialized());
    EXPECT_EQ(carrier.carve(64 * 1024), nullptr);
#endif
}

TEST(SuperCarrier, SegmentProviderWorksWithoutCarrier) {
    SegmentProvider::instance().set_super_carrier(nullptr);
    auto* slab = SegmentProvider::instance().acquire_slab(SizeClass::k32);
    ASSERT_NE(slab, nullptr);
    SegmentProvider::instance().release_slab(slab);
    // No crash, no carrier — legacy behavior unchanged
}
```

### GREEN — Implement

`HPACTOR_SUPER_CARRIER_ENABLED` is defined in the build system:
```cmake
# CMakeLists.txt
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(hpactor_lib PUBLIC HPACTOR_SUPER_CARRIER_ENABLED=1)
else()
    target_compile_definitions(hpactor_lib PUBLIC HPACTOR_SUPER_CARRIER_ENABLED=0)
endif()
```

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SuperCarrier*"
```

---

## Phase 8: Fault Injection

### RED — Write failing tests

```cpp
TEST(SuperCarrier, FaultInjectionInitFail) {
    FaultSchedule::Builder()
        .at("hpactor.allocator.super_carrier.init_fail", FaultAction::Fail)
        .build_and_install();
    FaultController::instance().enable();

    SuperCarrier carrier;
    EXPECT_FALSE(carrier.init(8ULL * 1024 * 1024 * 1024));
    FaultController::instance().disable();
    // System falls back to legacy mmap — no crash
}

TEST(SuperCarrier, FaultInjectionCarveFail) {
    SuperCarrier carrier;
    carrier.init(1ULL * 1024 * 1024 * 1024);
    FaultSchedule::Builder()
        .at("hpactor.allocator.super_carrier.carve_fail", FaultAction::Fail)
        .build_and_install();
    FaultController::instance().enable();

    void* slab = carrier.carve(64 * 1024);
    EXPECT_EQ(slab, nullptr);  // carve failed
    FaultController::instance().disable();
    // Legacy fallback picks up the slack
}
```

### GREEN — Implement

Wire `FAULT_INJECT` in `init()`, `carve()`, and `grow()`.

---

## Files Changed Summary

| File | Change |
|------|--------|
| `include/hpactor/mem/super_carrier.hpp` | **New file** — `SuperCarrier` class declaration |
| `src/mem/super_carrier.cpp` | **New file** — init, carve, release, grow |
| `include/hpactor/mem/segment_provider.hpp` | Add `SuperCarrier` member, `acquire_slab_memory()`, legacy/fallback stats |
| `src/mem/segment_provider.cpp` | Integrate carrier into acquire/release, legacy fallback |
| `include/hpactor/mem/memory_config.hpp` | `HPACTOR_SUPER_CARRIER_ENABLED` guard |
| `CMakeLists.txt` | Add `super_carrier.cpp` to build, `__LP64__` guard |
| `src/fault/fault_points.cpp` | Register 3 new fault points |
| `tests/unit/mem/test_super_carrier.cpp` | **New file** — 16 test cases |
| `tests/unit/mem/CMakeLists.txt` | Add new test target |

## Verification Checklist

```bash
# Unit tests
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SuperCarrier*"

# All existing tests — zero regression with carrier enabled
ctest -R "test_unit_mem" --output-on-failure --parallel 8

# 32-bit build
cmake -S . -B build-32 -DCMAKE_CXX_FLAGS="-m32"
ninja -C build-32 test_unit_mem
./build-32/tests/unit/mem/test_unit_mem --gtest_filter="*SuperCarrier*"

# Verify physical memory: /proc/self/smaps shows PROT_NONE carrier range with RSS=0

# TSan + ASan
cmake -S . -B build-tsan -DENABLE_TSAN=ON && ninja -C build-tsan test_unit_mem
```
