# MEM-005: Huge Page Support — Design Spec

**Date:** 2026-06-20
**Branch:** (future) `feature/mem-005-huge-pages`
**Issue:** #339 (Phase 2.2, P1)
**Parent Doc:** `docs/architecture/memory/memory-management-erlang-gap-analysis.md`
**Depends on:** MEM-004 (Super Carrier)

## 1. Motivation

### 1.1 Problem

HPActor's memory architecture design mentions `MAP_HUGETLB` but the current
`SegmentProvider::allocate_new_segment()` does not use it:

```cpp
void* base = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
```

At million-actor scale, each 2MB segment backed by 4KB pages requires 512
page table entries (PTEs). With 32,000 segments (64GB), that's 16 million PTEs.
Each TLB miss during an allocation or cross-thread free lookup walks the page
table, adding measurable latency jitter.

### 1.2 Erlang Reference

Erlang's super carrier uses huge pages (`+MMlp on`) so that a single 2MB TLB
entry covers an entire carrier segment. The `mseg_alloc` also supports huge
pages for individual segments.

### 1.3 Expected Impact

| Metric | Before (4KB pages) | After (2MB huge pages) | Delta |
|--------|--------------------|------------------------|-------|
| TLB entries for 64GB allocator | ~16M (4KB) | ~32K (2MB) | -99.8% |
| TLB misses / M allocations | ~500 (estimated) | ~50 | -90% |
| Page table memory | ~128MB (16M × 8B) | ~256KB (32K × 8B) | -99.8% |
| Slab carve latency (with super carrier) | ~1 µs | ~0.5 µs (fewer PTEs to fault) | -50% |

---

## 2. Design Goals

1. **Prefer huge pages for super carrier** — `MAP_HUGETLB` on Linux.
2. **Graceful fallback** — if huge pages are unavailable (pool exhausted, not
   configured), fall back to THP (`MADV_HUGEPAGE`) or 4KB pages.
3. **No compile-time dependency** — huge page support is detected at runtime.
4. **Minimal API change** — the `SuperCarrier` and `SegmentProvider` handle
   page size transparently.

---

## 3. Design

### 3.1 Huge Page Detection

At startup, `SegmemtProvider` probes huge page availability:

```cpp
struct HugePageInfo {
    bool explicit_huge_pages_available{false};   // MAP_HUGETLB works
    bool transparent_huge_pages_available{false}; // THP enabled
    size_t huge_page_size{0};                     // 2MB (default) or 1GB
    size_t huge_page_size_1gb{0};                 // 1GB if available
};

HugePageInfo probe_huge_pages() {
    HugePageInfo info;

    // Probe MAP_HUGETLB
    void* probe = mmap(nullptr, kHugePageProbeSize,
                       PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                       -1, 0);
    if (probe != MAP_FAILED) {
        info.explicit_huge_pages_available = true;
        // Determine page size:
        // - Check /proc/self/smaps for KernelPageSize
        // - Or try MAP_HUGETLB with MAP_HUGE_2MB / MAP_HUGE_1GB flags
        info.huge_page_size = detect_huge_page_size();
        munmap(probe, kHugePageProbeSize);
    }

    // Probe 1GB pages
    probe = mmap(nullptr, kHugePageProbeSize1G,
                 PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB,
                 -1, 0);
    if (probe != MAP_FAILED) {
        info.huge_page_size_1gb = 1ULL * 1024 * 1024 * 1024;
        munmap(probe, kHugePageProbeSize1G);
    }

    // Probe THP
    info.transparent_huge_pages_available = check_thp_enabled();

    return info;
}
```

### 3.2 Super Carrier with Huge Pages

The `SuperCarrier::init()` uses the detected huge page size:

```cpp
bool SuperCarrier::init(size_t size_bytes, const HugePageInfo& huge_info) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

    if (huge_info.explicit_huge_pages_available) {
        flags |= MAP_HUGETLB;
        // Optionally: MAP_HUGE_2MB or MAP_HUGE_1GB
    }

    carrier_base_ = mmap(nullptr, size_bytes, PROT_NONE, flags, -1, 0);
    if (carrier_base_ == MAP_FAILED) {
        // Fallback: try without MAP_HUGETLB
        flags &= ~MAP_HUGETLB;
        carrier_base_ = mmap(nullptr, size_bytes, PROT_NONE, flags, -1, 0);
        if (carrier_base_ == MAP_FAILED)
            return false;

        // Use THP hint instead
        madvise(carrier_base_, size_bytes, MADV_HUGEPAGE);
    }

    carrier_size_ = size_bytes;
    return true;
}
```

### 3.3 Fallback Hierarchy

```
1. MAP_HUGETLB + MAP_HUGE_1GB  → 1GB pages for the entire carrier
   ↓ (fallback)
2. MAP_HUGETLB + MAP_HUGE_2MB  → 2MB pages for the carrier
   ↓ (fallback)
3. MAP_HUGETLB (default size)  → system default huge page size
   ↓ (fallback)
4. mmap + MADV_HUGEPAGE        → 4KB pages with THP hint (kernel may merge)
   ↓ (fallback)
5. mmap (4KB pages)             → standard 4KB pages (current behavior)
```

### 3.4 Legacy SegmentProvider Path

When the super carrier is disabled or exhausted, individual `SegmentProvider`
segments also attempt huge page allocation:

```cpp
void* SegmentProvider::allocate_new_segment(size_t size_bytes) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    if (huge_info_.explicit_huge_pages_available) {
        flags |= MAP_HUGETLB;
    }

    void* base = mmap(nullptr, size_bytes, PROT_READ | PROT_WRITE,
                      flags, -1, 0);

    if (base == MAP_FAILED && (flags & MAP_HUGETLB)) {
        // Fallback without huge pages
        flags &= ~MAP_HUGETLB;
        base = mmap(nullptr, size_bytes, PROT_READ | PROT_WRITE,
                    flags, -1, 0);
    }

    if (base != MAP_FAILED && !(flags & MAP_HUGETLB)) {
        madvise(base, size_bytes, MADV_HUGEPAGE);
    }

    return base;
}
```

### 3.5 Configuration

```toml
[system.memory.huge_pages]
enabled = true              # Attempt huge pages (default: true)
prefer_1gb = false          # Try 1GB pages first (default: false)
fallback_to_thp = true      # Use THP if explicit huge pages unavailable
```

### 3.6 Observability

Extend `SegmemtProvider::Stats`:

```cpp
struct Stats {
    // ... existing ...
    uint64_t huge_page_segments{0};     // Segments backed by explicit huge pages
    uint64_t thp_segments{0};           // Segments with THP hint
    uint64_t regular_segments{0};       // Segments with 4KB pages
};
```

CLI visibility: `/memory segments` shows page size per segment.

---

## 4. Implementation Plan

### 4.1 TDDFlow Sequence

| Step | Test | Implementation |
|------|------|----------------|
| 1 | `HugePageDetection` — probe detects availability correctly | `probe_huge_pages()` |
| 2 | `SuperCarrierHugePageInit` — carrier with MAP_HUGETLB | `SuperCarrier::init()` huge page path |
| 3 | `SuperCarrierFallbackTHP` — fallback to THP when MAP_HUGETLB fails | Fallback hierarchy |
| 4 | `SegmentProviderHugePageSegment` — individual segments with huge pages | `allocate_new_segment()` huge page path |
| 5 | `HugePageStatsReporting` — stats correctly count page types | Stats counters |

### 4.2 Files Changed

| File | Change |
|------|--------|
| `include/hpactor/mem/segment_provider.hpp` | Add `HugePageInfo` struct, stats fields |
| `src/mem/segment_provider.cpp` | Implement huge page detection, `allocate_new_segment()` with huge pages |
| `src/mem/super_carrier.cpp` | Init with huge page flags, fallback hierarchy |
| `tests/unit/mem/test_huge_pages.cpp` | **New file** — 5 test cases |
| `tests/unit/mem/CMakeLists.txt` | Add new test target |

**Note:** Huge page tests requiring `MAP_HUGETLB` are gated on
`IsHugePageAvailable()` check; skipped (not failed) when huge pages are
unavailable in the test environment.

---

## 5. Testing Strategy

### 5.1 Unit Tests

| Test | What It Validates |
|------|-------------------|
| `HugePageDetection` | Probe correctly identifies available huge page support |
| `SuperCarrierHugePageInit` | Carrier backed by huge pages when available |
| `SuperCarrierFallbackTHP` | Falls back to THP when explicit huge pages unavailable |
| `SegmentProviderHugePageSegment` | Legacy segment path uses huge pages when available |
| `HugePageStatsReporting` | Stats distinguish huge page vs THP vs regular segments |

### 5.2 Platform Considerations

- **CI:** Most CI runners don't have huge pages configured. Tests are gated and
  skip gracefully.
- **Linux desktop:** `sudo sysctl vm.nr_hugepages=4096` enables testing.
- **macOS:** Huge pages unavailable on macOS (no MAP_HUGETLB). All tests gate
  on `IsHugePageAvailable()` and test the fallback path.

---

## 6. Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| Huge page pool exhaustion under concurrent allocation | Low | Graceful fallback to THP/4KB; per-segment allocation not per-page |
| Huge page fragmentation (cannot satisfy contiguous 2MB allocation) | Low | Huge pages are allocated at segment granularity (≥64KB), not per-block |
| macOS lack of MAP_HUGETLB | None | macOS uses standard 4KB pages with THP-equivalent via the VM subsystem automatically |
| CI testing without huge pages | Low | Tests gate and skip; production path tested manually or on dedicated hardware |

---

## 7. Acceptance Criteria

1. Huge page detection works on Linux (reads `/proc/sys/vm/nr_hugepages`).
2. Super carrier uses huge pages when available (verified via `/proc/self/smaps`
   `KernelPageSize: 2048 kB`).
3. Fallback to THP/4KB works silently when huge pages are exhausted.
4. All existing memory tests pass with huge pages enabled (or skipped).
5. No regression on macOS (no huge pages, but no crashes or errors).
