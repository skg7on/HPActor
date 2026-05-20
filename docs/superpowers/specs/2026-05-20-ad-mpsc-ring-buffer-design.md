# Shared MPSC Ring Buffer ADT — Design Spec

**Issue:** [#120 [REF-002]](https://github.com/skg7on/HPActor/issues/120)
**Date:** 2026-05-20
**Status:** Draft
**Subsystem:** ADT
**Priority:** P0 (Refactor lane)

## 1. Summary

Extract the duplicated MPSC ring buffer implementation from four subsystem
locations into a shared `hpactor::adt` ADT. Provide a compile-time-capacity
variant (`MpscRingBuffer<T, Capacity>`) and a runtime-capacity variant
(`DynamicMpscRingBuffer<T>`). Fix a concurrency race — where a consumer can
observe an advanced write cursor before the producer has written the payload —
once in the shared ADT instead of copying it to every subsystem.

## 2. Motivation

The same lock-free MPSC ring buffer pattern is copy-pasted across four
subsystems:

| File | Namespace | Capacity | Template? |
|------|-----------|----------|-----------|
| `metrics/metrics_ring_buffer.hpp` | `hpactor::metrics` | compile-time `Capacity` | `template<T, size_t>` |
| `log/log_ring_buffer.hpp` | `hpactor::log` | runtime `capacity` arg | hardwired to `LogEvent` |
| `mem/telemetry_ring_buffer.hpp` | `hpactor::mem` | compile-time `Capacity` | alias of `metrics::MpscRingBuffer` |
| `tracing/trace_manager.hpp` | `hpactor::tracing` | compile-time default | uses `metrics::MpscRingBuffer<SpanRecord>` |

The implementations differ in details (capacity checks, overflow handling) but
share the same concurrency protocol — and the same race condition. A single
correct ADT eliminates duplication, fixes the race once, and simplifies future
ring-buffer uses.

This is Section 6 of the broader shared-ADT extraction (see
`2026-05-18-shared-adt-extraction-design.md`). It is a standalone spec because
the ring buffer has a concurrency contract, a correctness fix, and subsystem
tests that warrant independent design review.

## 3. Design

### 3.1 Concurrency Race — Root Cause and Fix

**Current protocol (buggy):**

```
Producer:                             Consumer:
1. CAS write_cursor_ w→w+1 (acquire)   1. w = write_cursor_.load(acquire)
2. buffer_[w & mask_] = value          2. atomic_thread_fence(acquire)
3. atomic_thread_fence(release)        3. read buffer_[r & mask_]
```

**Root cause:** Step 1 (CAS) stores w+1 to `write_cursor_`. The CAS has acquire
semantics on success, not release. The consumer loads `write_cursor_` with
acquire (step C1) and can see w+1 *before* the producer writes the payload at
step 2. The release fence at step 3 has no effect because there is no subsequent
atomic store for it to order — `write_cursor_` was already updated. The acquire
fence at step C2 pairs with nothing.

**Fix: per-slot publish sequence.** Each ring buffer slot gets a sequence number
`seq_[i]`, initialized to `i`. The producer claims a write position, writes the
payload, then publishes the slot by storing a sentinel value into `seq_[i]`
with release semantics. The consumer reads `seq_[i]` with acquire semantics; it
advances only when the slot is published. The consumer marks the slot free for
reuse by storing `i + Capacity` with release semantics.

```
Producer:                                 Consumer (single-threaded):
1. w = write_cursor_.fetch_add(1, relaxed)  1. r = read_cursor_.load(relaxed)
2. buffer_[w & mask_] = value               2. if seq_[r & mask_].load(acquire) != r+1: break
3. seq_[w & mask_].store(w + 1, release)    3. callback(buffer_[r & mask_])
                                            4. seq_[r & mask_].store(r + Capacity, release)
                                            5. r++
                                            6. read_cursor_.store(r, release)
```

**Correctness argument:** The producer's `fetch_add(1, relaxed)` is atomic but
has no ordering constraints — it only reserves a unique position. The payload
write at step 2 is sequenced-before the release store at step 3. The consumer's
acquire load at step C2 reads the release store from step 3. This creates a
*synchronizes-with* edge: payload write →happens-before→ consumer read. Without
the release store on `seq_[w & mask_]`, there is no happens-before relationship
and the consumer may observe stale payload data.

**Overflow detection (compile-time capacity):** The slot at position w is
available for writing when `seq_[w & mask_].load(acquire) == w`. If this
condition fails, a concurrent consumer has not yet marked the slot free — the
buffer is full.

### 3.2 Shared ADT API

#### `adt::MpscRingBuffer<T, Capacity>` — Compile-Time Capacity

```cpp
namespace hpactor::adt {

template <typename T, size_t Capacity = 65536>
class MpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    static constexpr size_t kDefaultCapacity = Capacity;

    MpscRingBuffer();

    // Producer: write value, then publish slot with release semantics.
    // Returns false if buffer is full (no slot available).
    bool try_push(const T& value) noexcept;

    // Consumer (single-threaded): drain all published slots since last drain.
    // Draining frees slots for producer reuse.
    template <typename Fn>
    size_t drain(Fn&& callback);

    // Overflow counter — incremented on each rejected try_push.
    uint64_t events_lost() const noexcept;

    // Current number of un-drained elements. O(1), lock-free.
    size_t size() const noexcept;

    bool empty() const noexcept;
};

} // namespace hpactor::adt
```

**Concurrency contract:**
- `try_push` is safe to call from any number of threads concurrently.
- `drain`, `size`, `empty` must be called from the consumer thread only.
- `events_lost` is safe from any thread.
- The buffer stores value-typed `T` (copy-constructible, copy-assignable).

#### `adt::DynamicMpscRingBuffer<T>` — Runtime Capacity

```cpp
namespace hpactor::adt {

template <typename T>
class DynamicMpscRingBuffer {
public:
    // capacity must be a power of two.
    explicit DynamicMpscRingBuffer(size_t capacity);

    // Same API surface as MpscRingBuffer<T, Capacity> above.
    bool try_push(const T& value) noexcept;
    template <typename Fn> size_t drain(Fn&& callback);
    uint64_t events_lost() const noexcept;
    size_t size() const noexcept;
    bool empty() const noexcept;
};

} // namespace hpactor::adt
```

**Concurrency contract:** Same as compile-time variant.

**Capacity validation:** The constructor validates capacity > 0 and
power-of-two at runtime via an `assert` (debug); in release builds, an invalid
capacity triggers an abort via `std::fprintf` + `std::abort()` (matching the
existing `LogRingBuffer` behavior for graceful failure reporting).

### 3.3 Subsystem Migration Path

Each subsystem keeps its public type but delegates to the shared ADT:

#### `metrics::MpscRingBuffer<T, Capacity>`

```cpp
// metrics/metrics_ring_buffer.hpp
#include <hpactor/adt/mpsc_ring_buffer.hpp>

namespace hpactor::metrics {
    template <typename T, size_t Capacity = 65536>
    using MpscRingBuffer = adt::MpscRingBuffer<T, Capacity>;
}
```

Source-compatible alias. All callers in `actor/`, `mailbox/`, `sched/`,
`supervision/`, `core/`, `tracing/`, `metrics/`, `config/`, `tests/` continue to
use `metrics::MpscRingBuffer<MetricEvent>` and
`metrics::MpscRingBuffer<MetricEvent>*` unchanged.

#### `mem::TelemetryRingBuffer<Capacity>`

```cpp
// mem/telemetry_ring_buffer.hpp
#include <hpactor/adt/mpsc_ring_buffer.hpp>

namespace hpactor::mem {
    template <size_t Capacity = 65536>
    using TelemetryRingBuffer = adt::MpscRingBuffer<AllocationEvent, Capacity>;
}
```

Source-compatible alias.

#### `tracing::TraceManager`

Already uses `metrics::MpscRingBuffer<SpanRecord>` — no change needed. The alias
in `metrics/` transparently redirects to `adt::MpscRingBuffer`.

#### `log::LogRingBuffer`

```cpp
// log/log_ring_buffer.hpp
#include <hpactor/adt/mpsc_ring_buffer.hpp>

namespace hpactor::log {

class LogRingBuffer {
public:
    explicit LogRingBuffer(size_t capacity)
        : buffer_(capacity) {}

    bool try_push(const LogEvent& value) noexcept {
        return buffer_.try_push(value);
    }

    template <typename Fn>
    size_t drain(Fn&& callback) {
        return buffer_.drain(std::forward<Fn>(callback));
    }

    uint64_t events_lost() const noexcept { return buffer_.events_lost(); }
    size_t size() const noexcept { return buffer_.size(); }
    bool empty() const noexcept { return buffer_.empty(); }

private:
    adt::DynamicMpscRingBuffer<LogEvent> buffer_;
};

} // namespace hpactor::log
```

Thin wrapper that preserves the `LogRingBuffer` type identity for existing
callers (`LogManager`, `LogDrain`, `Logger`, `test_log_ring_buffer`) while
delegating to `adt::DynamicMpscRingBuffer<LogEvent>`.

### 3.4 File Layout

```
include/hpactor/adt/mpsc_ring_buffer.hpp     — NEW: shared ADT (both variants)
include/hpactor/metrics/metrics_ring_buffer.hpp  — EDIT: alias → adt::MpscRingBuffer
include/hpactor/mem/telemetry_ring_buffer.hpp    — EDIT: alias → adt::MpscRingBuffer
include/hpactor/log/log_ring_buffer.hpp          — EDIT: wrapper delegates to adt::DynamicMpscRingBuffer

tests/adt/test_adt_mpsc_ring_buffer.cpp      — NEW: focused ADT tests
tests/log/test_log_ring_buffer.cpp            — EDIT: verify compatibility
tests/mem/test_telemetry_ring_buffer.cpp      — EDIT: verify compatibility
```

The remaining 30+ files that `#include <hpactor/metrics/metrics_ring_buffer.hpp>`
or `#include <hpactor/log/log_ring_buffer.hpp>` require no changes.

## 4. Dependencies

- **Existing:** `adt/stream_buffer.hpp` already establishes the `hpactor::adt`
  namespace and directory. No new build system changes needed.
- **None** — the ADT has no dependencies beyond `<atomic>`, `<cstddef>`,
  `<cstdint>`, `<cassert>`, `<cstdlib>`, `<vector>`, `<memory>`.
- **No protobuf, no RTTI, no exceptions** — consistent with project constraints.

## 5. Non-Goals

- Lock-free SPSC or MPMC buffer variants. The shared ADT addresses the four
  existing MPSC use cases only.
- Zero-copy or movable-only `T` support. The ring buffer stores by copy (as the
  existing implementations do). Movable-only types are deferred.
- Resize or runtime capacity change after construction.
- Ring buffer iteration or random access — only `try_push` / `drain` are
  provided.
- Blocking push with wait/notify. Overflow is reported via return value and
  `events_lost()` counter.

## 6. API Surface Summary

| API | Location | Purpose |
|-----|----------|---------|
| `adt::MpscRingBuffer<T, Capacity>` | NEW `adt/mpsc_ring_buffer.hpp` | Compile-time capacity MPSC ring buffer |
| `adt::DynamicMpscRingBuffer<T>` | NEW `adt/mpsc_ring_buffer.hpp` | Runtime capacity MPSC ring buffer |
| `metrics::MpscRingBuffer<T, C>` | EDIT `metrics/metrics_ring_buffer.hpp` | Alias → `adt::MpscRingBuffer` |
| `mem::TelemetryRingBuffer<C>` | EDIT `mem/telemetry_ring_buffer.hpp` | Alias → `adt::MpscRingBuffer` |
| `log::LogRingBuffer` | EDIT `log/log_ring_buffer.hpp` | Wrapper → `adt::DynamicMpscRingBuffer<LogEvent>` |

No new public API beyond the ADT types themselves. All subsystem types preserve
their existing call signatures.

## 7. Acceptance Criteria

- [ ] `adt::MpscRingBuffer<T, Capacity>` with per-slot publish-sequence protocol;
  compile-time capacity ≥ 2 and power-of-two enforced by `static_assert`.
- [ ] `adt::DynamicMpscRingBuffer<T>` with per-slot publish-sequence protocol;
  runtime capacity validated on construction.
- [ ] Consumer cannot observe an advanced cursor before the producer has written
  the payload (race fixed once in the ADT).
- [ ] `metrics::MpscRingBuffer<T, Capacity>` is a transparent alias — all
  existing callers compile and pass tests unchanged.
- [ ] `mem::TelemetryRingBuffer<Capacity>` is a transparent alias — all
  existing callers compile and pass tests unchanged.
- [ ] `log::LogRingBuffer` delegates to `adt::DynamicMpscRingBuffer<LogEvent>`
  — all existing callers compile and pass tests unchanged.
- [ ] `tracing::TraceManager` works unchanged via the `metrics::` alias.
- [ ] No new includes or edits required in the 30+ files that consume
  `metrics::MpscRingBuffer` or `log::LogRingBuffer`.
- [ ] `tests/adt/test_adt_mpsc_ring_buffer.cpp` covers:
  - Push/drain with ordered verification for both variants.
  - Overflow detection and `events_lost()` for both variants.
  - Runtime capacity validation (`DynamicMpscRingBuffer`).
  - Concurrent producers (≥3 threads) with an active consumer drain loop.
  - Drain-after-overflow preserves previously-inserted elements.
- [ ] Existing subsystem tests (`test_log_ring_buffer`, `test_telemetry_ring_buffer`)
  pass without modification (or trivial include-only changes).
- [ ] Full test suite (140 tests) passes. Sanitizer clean (TSAN, ASAN).

## 8. Test Plan

| Test Suite | Coverage |
|-----------|----------|
| `test_adt_mpsc_ring_buffer` (NEW) | Both variants: basic push/drain, overflow, concurrent producers + consumer, drain-after-overflow, runtime capacity validation, per-slot publish verification under contention |
| `test_log_ring_buffer` (existing) | Recompile only — prove `LogRingBuffer` wrapper preserves existing behavior |
| `test_telemetry_ring_buffer` (existing) | Recompile only — prove `TelemetryRingBuffer` alias preserves existing behavior |
| Full suite (`ctest`) | No regressions — all 140 existing tests pass |

### Focused ADT Test Scenarios

1. **Push/Drain ordering:** Push N values (N < Capacity), drain, verify order
   and count for both compile-time and dynamic variants.
2. **Overflow + events_lost:** Fill to capacity, push one more, verify
   `try_push` returns false and `events_lost()` == 1.
3. **Capacity validation:** Dynamic variant — `capacity=0` and
   non-power-of-two capacities abort; valid capacity succeeds.
4. **Concurrent producers + consumer:** N producers push M events each, consumer
   drains in a loop. Verify total drained == N*M, no events lost, no data
   corruption.
5. **Drain after overflow:** Fill, overflow, drain — verify the buffered
   elements are intact and the overflowed element is counted but not present.
6. **Publish protocol under contention:** Multiple producers pushing to
   adjacent slots, consumer draining — verify no consumer read of a
   not-yet-published slot (TSAN would catch this).
