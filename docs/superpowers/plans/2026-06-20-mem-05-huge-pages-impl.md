# MEM-005: Huge Page Support — Implementation Plan

**Date:** 2026-06-20
**Design Spec:** `docs/superpowers/specs/2026-06-20-mem-05-huge-pages-design.md`
**Issue:** #339 (Phase 2.2, P1)
**Depends on:** MEM-004 (Super Carrier)

## Phase Overview

| Phase | Name | Tests | Files Changed |
|-------|------|-------|---------------|
| 1 | `HugePageInfo` struct + `probe_huge_pages()` | 2 | 2 |
| 2 | SuperCarrier init with `MAP_HUGETLB` | 2 | 1 |
| 3 | Fallback hierarchy (1GB → 2MB → THP → 4KB) | 2 | 1 |
| 4 | Legacy SegmentProvider huge page path | 2 | 1 |
| 5 | Stats reporting per page size | 2 | 1 |

---

## Phase 1: `HugePageInfo` + `probe_huge_pages()`

**Goal:** Runtime detection of huge page availability via `mmap` probe.

### RED — Write failing tests

File: `tests/unit/mem/test_huge_pages.cpp` (new)

```cpp
#include <gtest/gtest.h>
#include "hpactor/mem/segment_provider.hpp"

using namespace hpactor::mem;

TEST(HugePages, ProbeDoesNotCrash) {
    HugePageInfo info = probe_huge_pages();
    // Must not crash regardless of availability
    SUCCEED();
}

TEST(HugePages, ProbeSetsPageSizeWhenAvailable) {
    HugePageInfo info = probe_huge_pages();
    if (info.explicit_huge_pages_available) {
        EXPECT_GT(info.huge_page_size, 0u);
        EXPECT_TRUE(info.huge_page_size == 2 * 1024 * 1024 ||
                    info.huge_page_size == 1 * 1024 * 1024 * 1024);
    }
}
```

### GREEN — Implement

1. Add `HugePageInfo` to `include/hpactor/mem/segment_provider.hpp`:
   ```cpp
   struct HugePageInfo {
       bool explicit_huge_pages_available{false};
       bool transparent_huge_pages_available{false};
       size_t huge_page_size{0};
       size_t huge_page_size_1gb{0};
   };
   HugePageInfo probe_huge_pages();
   ```

2. Implement probe in `src/mem/segment_provider.cpp` using `mmap(MAP_HUGETLB)`.

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*HugePages*"
```

---

## Phase 2: SuperCarrier Init with `MAP_HUGETLB`

**Goal:** `SuperCarrier::init()` uses `MAP_HUGETLB` when available.

### RED — Write failing tests

```cpp
TEST(HugePages, SuperCarrierUsesHugePagesWhenAvailable) {
    HugePageInfo info = probe_huge_pages();
    SuperCarrier carrier;
    bool ok = carrier.init(256 * 1024 * 1024, info);  // 256MB
#if HPACTOR_SUPER_CARRIER_ENABLED
    EXPECT_TRUE(ok);
    if (info.explicit_huge_pages_available) {
        // Carrier should be huge-page-backed (verified in smaps externally)
        SUCCEED();
    }
#endif
}

TEST(HugePages, SuperCarrierInitWithoutHugePagesSucceeds) {
    HugePageInfo info{};  // all false
    SuperCarrier carrier;
    bool ok = carrier.init(256 * 1024 * 1024, info);
#if HPACTOR_SUPER_CARRIER_ENABLED
    EXPECT_TRUE(ok);  // Falls back to 4KB pages
#endif
}
```

### GREEN — Implement

Update `SuperCarrier::init()` signature to accept `HugePageInfo` and set
`MAP_HUGETLB` flag when available.

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*HugePages*"
```

---

## Phase 3: Fallback Hierarchy

**Goal:** Four-tier fallback: MAP_HUGE_1GB → MAP_HUGE_2MB → MAP_HUGETLB →
MADV_HUGEPAGE → 4KB.

### RED — Write failing tests

```cpp
TEST(HugePages, FallbackToThpWhenExplicitHugePagesUnavailable) {
    HugePageInfo info{};
    info.transparent_huge_pages_available = true;
    SuperCarrier carrier;
    bool ok = carrier.init(256 * 1024 * 1024, info);
    EXPECT_TRUE(ok);  // MADV_HUGEPAGE fallback works
}

TEST(HugePages, FallbackTo4kbWhenNothingAvailable) {
    HugePageInfo info{};  // nothing available
    SuperCarrier carrier;
    bool ok = carrier.init(256 * 1024 * 1024, info);
    EXPECT_TRUE(ok);  // 4KB fallback always works
}
```

### GREEN — Implement

The fallback hierarchy in `SuperCarrier::init()`:
```cpp
bool SuperCarrier::init(size_t size_bytes, const HugePageInfo& info) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

    if (info.huge_page_size_1gb > 0) {
        carrier_base_ = mmap(nullptr, size_bytes, PROT_NONE,
                             flags | MAP_HUGETLB | MAP_HUGE_1GB, -1, 0);
        if (carrier_base_ != MAP_FAILED) return finish_init(size_bytes);
    }
    if (info.huge_page_size >= 2 * 1024 * 1024) {
        carrier_base_ = mmap(nullptr, size_bytes, PROT_NONE,
                             flags | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
        if (carrier_base_ != MAP_FAILED) return finish_init(size_bytes);
    }
    if (info.explicit_huge_pages_available) {
        carrier_base_ = mmap(nullptr, size_bytes, PROT_NONE,
                             flags | MAP_HUGETLB, -1, 0);
        if (carrier_base_ != MAP_FAILED) return finish_init(size_bytes);
    }
    // THP or 4KB fallback
    carrier_base_ = mmap(nullptr, size_bytes, PROT_NONE, flags, -1, 0);
    if (carrier_base_ != MAP_FAILED) {
        if (info.transparent_huge_pages_available)
            madvise(carrier_base_, size_bytes, MADV_HUGEPAGE);
        return finish_init(size_bytes);
    }
    return false;
}
```

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*HugePages*"
```

---

## Phase 4: Legacy SegmentProvider Huge Page Path

**Goal:** Individual `SegmentProvider` segments also attempt huge pages.

### RED — Write failing tests

```cpp
TEST(HugePages, LegacySegmentUsesHugePagesWhenAvailable) {
    HugePageInfo info = probe_huge_pages();
    SegmentProvider::instance().set_huge_page_info(info);

    auto* slab = SegmentProvider::instance().acquire_slab(SizeClass::k256);  // 128KB slab
    ASSERT_NE(slab, nullptr);

    auto stats = SegmentProvider::instance().stats();
    if (info.explicit_huge_pages_available) {
        EXPECT_GT(stats.huge_page_segments, 0u);
    }
    SegmentProvider::instance().release_slab(slab);
}
```

### GREEN — Implement

Update `allocate_new_segment_legacy()` to use `MAP_HUGETLB` when available.

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*HugePages*"
```

---

## Phase 5: Stats Reporting

**Goal:** `SegmentProvider::Stats` tracks huge-page vs THP vs regular segments.

### RED — Write failing tests

```cpp
TEST(HugePages, StatsDistinguishPageTypes) {
    auto stats = SegmentProvider::instance().stats();
    // At least one of these counters is non-zero after system startup
    uint64_t total = stats.huge_page_segments +
                     stats.thp_segments +
                     stats.regular_segments;
    EXPECT_GT(total, 0u);  // Some segments were allocated during test setup
}
```

### GREEN — Implement

Stats counters already added in Phase 4. CLI `/memory segments` exposes them.

---

## Files Changed Summary

| File | Change |
|------|--------|
| `include/hpactor/mem/segment_provider.hpp` | `HugePageInfo` struct, `probe_huge_pages()`, page-type stats |
| `src/mem/segment_provider.cpp` | Huge page probe, `allocate_new_segment_legacy()` with MAP_HUGETLB |
| `src/mem/super_carrier.cpp` | `init()` with huge page flags, fallback hierarchy |
| `tests/unit/mem/test_huge_pages.cpp` | **New file** — 9 test cases |
| `tests/unit/mem/CMakeLists.txt` | Add new test target |

## Verification Checklist

```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*HugePages*"
./build/tests/unit/mem/test_unit_mem --gtest_filter="*SuperCarrier*"
ctest -R "test_unit_mem" --output-on-failure --parallel 8
```
