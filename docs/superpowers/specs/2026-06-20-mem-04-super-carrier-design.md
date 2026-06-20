# MEM-004: Super Carrier — Contiguous Virtual Memory Reservation — Design Spec

**Date:** 2026-06-20
**Branch:** (future) `feature/mem-004-super-carrier`
**Issue:** #339 (Phase 2.1, P1)
**Parent Doc:** `docs/architecture/memory/memory-management-erlang-gap-analysis.md`

## 1. Motivation

### 1.1 Problem

HPActor's `SegmentProvider` calls `mmap` for each 2MB segment individually.
At million-actor scale, this creates:

- **Thousands of discontinuous mappings** — each consuming a TLB entry. At 2MB
  per segment and 64GB of allocator memory, that's 32,768 segments. Each
  requires a TLB entry for the segment's page table structure. Even with 2MB
  huge pages, the TLB pressure from metadata access (segment header, slab
  records, freelist heads) is significant.

- **Runtime `mmap`/`munmap` syscalls** — each slab exhaustion triggers a
  kernel round-trip. Under heavy churn, hundreds of `mmap`/`munmap` calls per
  hour add latency jitter (kernel lock contention, page table population).

- **No virtual address stability** — segments are scattered across the virtual
  address space. Future pointer compression or tagged pointer optimizations
  are infeasible without a known address range.

### 1.2 Erlang Reference

Erlang's **Super Carrier** (`+MMscs` flag) reserves a single huge contiguous
virtual address range at VM startup (e.g., 64GB). All carriers are carved from
this range:

```
Startup:  mmap(PROT_NONE, 64GB, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)
Carve:    mprotect(slab_start, slab_size, PROT_READ | PROT_WRITE)
Release:  madvise(MADV_FREE) + mprotect(PROT_NONE)
```

The reservation costs zero physical memory (MAP_NORESERVE + PROT_NONE). Only
carved slabs consume physical pages. Released slabs return physical pages but
keep the virtual reservation — no TLB invalidation for the carved range.

### 1.3 Expected Impact

| Metric | Before (per-segment mmap) | After (super carrier) | Delta |
|--------|--------------------------|----------------------|-------|
| TLB entries for allocator | ~1000s (discontinuous) | ~10s (contiguous range) | -90%+ |
| `mmap`/`munmap` syscalls/hour | 100s–1000s | ~1–2 (carrier grow only) | -99%+ |
| Slab acquire latency (refill) | ~10 µs (mmap) | ~1 µs (mprotect) | -90% |
| Virtual address range | Scattered | Predictable [base, base+size) | Qualitative |

---

## 2. Design Goals

1. **Single contiguous virtual reservation** — reserved at `ActorSystem` startup.
2. **Zero physical memory at startup** — PROT_NONE + MAP_NORESERVE.
3. **On-demand page commitment** — carve slabs via `mprotect`.
4. **Physical page return on release** — `madvise(MADV_FREE)` + `mprotect(PROT_NONE)`.
5. **64-bit only** — guarded by `#ifdef __LP64__`; 32-bit falls back to current
   per-segment mmap behavior.
6. **Configurable reservation size** — default 8GB, configurable up to 64GB.

---

## 3. Design

### 3.1 Super Carrier Lifecycle

```
ActorSystem startup:
  ┌─────────────────────────────────────────────────────────┐
  │ 1. Read config: carrier.size = 8GB (default)            │
  │ 2. mmap(nullptr, size, PROT_NONE,                       │
  │          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,    │
  │          -1, 0)                                         │
  │ 3. Store carrier_base_, carrier_size_, carve_offset_(0) │
  │ 4. Register carrier with SegmentProvider                 │
  └─────────────────────────────────────────────────────────┘

Slab acquire (SegmentProvider::acquire_slab):
  ┌─────────────────────────────────────────────────────────┐
  │ 1. Check: carve_offset_ + slab_size <= carrier_size_    │
  │ 2. slab_addr = carrier_base_ + carve_offset_            │
  │ 3. mprotect(slab_addr, slab_size, PROT_READ|PROT_WRITE) │
  │ 4. carve_offset_ += slab_size                           │
  │ 5. If carve would exceed carrier:                        │
  │    a. Try to grow carrier (mremap) — configurable        │
  │    b. Or fall back to individual mmap (legacy path)      │
  └─────────────────────────────────────────────────────────┘

Slab release (SegmentProvider::release_slab):
  ┌─────────────────────────────────────────────────────────┐
  │ 1. madvise(slab_addr, slab_size, MADV_FREE)             │
  │ 2. mprotect(slab_addr, slab_size, PROT_NONE)            │
  │ 3. Slab virtual address remains reserved                │
  │ 4. Slab cannot be reused (sequential carve only)        │
  │ 5. If all slabs at end are freed: retract carve_offset_ │
  └─────────────────────────────────────────────────────────┘
```

### 3.2 SegmentProvider Integration

The `SegmentProvider` gains a `SuperCarrier` member that becomes the primary
allocation source:

```cpp
class SuperCarrier {
public:
    // Initialize the super carrier reservation
    bool init(size_t size_bytes);

    // Carve a slab from the carrier
    // Returns nullptr if carrier is exhausted
    void* carve(size_t slab_size_bytes);

    // Release a slab's physical pages (keeps virtual)
    void release(void* slab_addr, size_t slab_size_bytes);

    // Stats
    size_t total_reserved() const;
    size_t total_carved() const;
    size_t total_released() const;
    size_t current_carved() const;  // carved - released

    bool is_initialized() const { return carrier_base_ != nullptr; }

private:
    void* carrier_base_{nullptr};
    size_t carrier_size_{0};
    std::atomic<size_t> carve_offset_{0};     // monotonic, atomic for read
    std::atomic<size_t> released_bytes_{0};   // total released back
    size_t max_carrier_size_{0};              // configurable cap
    bool can_grow_{true};                     // allow mremap growth
};

class SegmentProvider {
    // ... existing ...

    // NEW: super carrier as primary source
    SuperCarrier super_carrier_;

    // Legacy fallback for when carrier is exhausted or on 32-bit
    void* allocate_new_segment_legacy(size_t size_bytes);

    // Unified acquire
    void* acquire_slab_memory(size_t size_bytes) {
        if (super_carrier_.is_initialized()) {
            if (auto* mem = super_carrier_.carve(size_bytes))
                return mem;
            // Carrier exhausted — try legacy
        }
        return allocate_new_segment_legacy(size_bytes);
    }
};
```

### 3.3 Carrier Growth

When the carrier is exhausted, two options:

1. **Grow via `mremap`** (Linux) — extend the contiguous reservation. Only
   possible if the virtual address space immediately after the carrier is
   available. Probabilistic; not guaranteed.

2. **Fall back to individual `mmap`** — allocate additional segments outside
   the carrier. These segments still benefit from the TLB reduction of the
   existing carrier but are individually mapped.

**Policy:** Attempt growth once (up to `max_carrier_size_`). If growth fails
or is unavailable (macOS has no `mremap`), fall back to individual `mmap`.

### 3.4 Platform Support

| Platform | Super Carrier | Fallback |
|----------|--------------|----------|
| Linux x86_64 | Full support (mmap + mprotect + madvise + mremap) | Individual mmap |
| Linux aarch64 | Full support | Individual mmap |
| macOS arm64/x86_64 | Partial (no mremap; use mmap + mprotect + MADV_FREE) | Individual mmap |
| 32-bit any | **Disabled** — virtual address space too constrained | Individual mmap (current behavior) |

### 3.5 Configuration

```toml
[system.memory.carrier]
enabled = true              # Enable super carrier (default: true on __LP64__)
size_gb = 8                 # Initial reservation size (default: 8GB)
max_size_gb = 64            # Maximum growth size (default: 64GB)
can_grow = true             # Allow mremap growth (default: true)
```

**Compile-time guard:**

```cpp
#ifdef __LP64__
#define HPACTOR_SUPER_CARRIER_ENABLED 1
#else
#define HPACTOR_SUPER_CARRIER_ENABLED 0
#endif
```

### 3.6 Fault Injection

| Site | Domain | Action | Purpose |
|------|--------|--------|---------|
| `hpactor.allocator.super_carrier.init_fail` | Allocator | Fail | Simulate mmap failure at startup |
| `hpactor.allocator.super_carrier.carve_fail` | Allocator | Fail | Simulate mprotect failure during carve |
| `hpactor.allocator.super_carrier.grow_fail` | Allocator | Fail | Simulate mremap failure |

---

## 4. Implementation Plan

### 4.1 TDDFlow Sequence

| Step | Test | Implementation |
|------|------|----------------|
| 1 | `SuperCarrierInitSuccess` — init reserves virtual range, zero physical | `SuperCarrier::init()` |
| 2 | `SuperCarrierCarve` — carve returns mprotected memory, offset advances | `SuperCarrier::carve()` |
| 3 | `SuperCarrierRelease` — release returns physical pages, virtual remains | `SuperCarrier::release()` |
| 4 | `SuperCarrierExhaust` — carve returns nullptr when carrier full, fallback used | Exhaustion + legacy fallback |
| 5 | `SuperCarrierGrow` — mremap extends carrier (Linux only) | `SuperCarrier::grow()` |
| 6 | `SuperCarrierDisabled32Bit` — carrier disabled on 32-bit, legacy path used | Platform guard |
| 7 | `SuperCarrierFaultInjection` — fault sites trigger correctly | FAULT_INJECT wiring |

### 4.2 Files Changed

| File | Change |
|------|--------|
| `include/hpactor/mem/segment_provider.hpp` | Add `SuperCarrier` class declaration |
| `src/mem/segment_provider.cpp` | Implement `SuperCarrier`, integrate with `acquire_slab()` |
| `src/mem/super_carrier.cpp` | **New file** — `SuperCarrier` implementation |
| `include/hpactor/mem/memory_config.hpp` | Add carrier configuration constants |
| `src/config/parsers/memory_carrier_config_parser.cpp` | **New file** — TOML parser for `[system.memory.carrier]` |
| `tests/unit/mem/test_super_carrier.cpp` | **New file** — 7 test cases |
| `tests/unit/mem/CMakeLists.txt` | Add new test target |

---

## 5. Testing Strategy

### 5.1 Unit Tests

| Test | What It Validates |
|------|-------------------|
| `SuperCarrierInitSuccess` | Virtual reservation succeeds, `/proc/self/maps` shows PROT_NONE range |
| `SuperCarrierCarve` | Carved memory is readable/writable, carve_offset advances |
| `SuperCarrierRelease` | Released pages not accessible, stats reflect release |
| `SuperCarrierExhaust` | Carve returns nullptr at boundary, legacy fallback is invoked |
| `SuperCarrierGrow` | mremap succeeds (Linux), subsequent carves work in grown region |
| `SuperCarrierDisabled32Bit` | HPACTOR_SUPER_CARRIER_ENABLED=0 on 32-bit, legacy path used |
| `SuperCarrierFaultInjection` | Fault injection sites trigger correctly |

### 5.2 Platform Considerations

- Linux: full test coverage (mmap + mprotect + madvise + mremap).
- macOS: skip growth tests (no mremap).
- All platforms: `HPACTOR_SUPER_CARRIER_ENABLED=0` compile path tested.

---

## 6. Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| 8GB reservation fails on systems with `vm.max_map_count` limits | Low | Configurable size; default 8GB is well under typical limits (65K maps × 4KB = 256MB actually used, not 8GB virtual) |
| `mprotect` latency on carve | Low | Amortized — once per slab (thousands of allocations); measured at <1µs for 64KB–512KB |
| Carrier fragmentation (released holes cannot be reused) | Medium | Sequential carve only; released holes are permanent virtual waste. Mitigation: use large enough initial carrier (8GB) that exhaustion before process death is unlikely. Future: add a free-list of released virtual ranges if needed. |
| macOS `MADV_FREE` semantics differ from Linux (lazy reclaim) | Low | Acceptable — physical pages reclaimed under pressure eventually. Production monitoring via `MemoryPressureState`. |
| 32-bit systems cannot use super carrier | None | 32-bit is explicitly unsupported for super carrier — falls back to current behavior. |

---

## 7. Acceptance Criteria

1. Super carrier initializes on Linux/macOS x86_64 and aarch64.
2. Slab carve from super carrier is < 1µs (vs ~10µs for mmap).
3. Physical memory is returned on slab release (verified via `MADV_FREE`
   counters or `/proc/self/smaps`).
4. Fallback to individual `mmap` works when carrier is exhausted.
5. 32-bit builds compile and use legacy path without errors.
6. All existing memory tests pass with super carrier enabled.
7. TSan-clean and ASan-clean.
