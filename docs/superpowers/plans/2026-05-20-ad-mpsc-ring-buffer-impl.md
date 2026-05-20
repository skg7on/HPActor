# Shared MPSC Ring Buffer ADT — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract the duplicated MPSC ring buffer into `hpactor::adt` with a race-condition fix (per-slot publish/sequence protocol), then migrate all four subsystem users to the shared ADT via source-compatible aliases or wrappers.

**Architecture:** New header-only ADT in `include/hpactor/adt/mpsc_ring_buffer.hpp` containing both `MpscRingBuffer<T, Capacity>` (compile-time capacity, `std::vector` storage) and `DynamicMpscRingBuffer<T>` (runtime capacity, `std::unique_ptr<T[]>` storage). Both use a per-slot sequence-number protocol that closes the existing consumer-reads-uninitialized-slot race. Subsystem types become `using` aliases or thin wrappers — no call-site changes in the 30+ consumer files.

**Tech Stack:** C++20, `<atomic>`, `<vector>`, `<memory>`, no exceptions, no RTTI.

---

### Task 1: Create the ADT test file (failing)

**Files:**
- Create: `tests/adt/test_adt_mpsc_ring_buffer.cpp`
- Modify: `tests/CMakeLists.txt` — register new test

- [ ] **Step 1: Create tests/adt directory and test file**

```bash
mkdir -p tests/adt
```

Create `tests/adt/test_adt_mpsc_ring_buffer.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/adt/mpsc_ring_buffer.hpp>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace hpactor;

// Trivial payload for buffer tests
struct TestPayload {
    int value = 0;
    char pad[28] = {}; // make it 32 bytes, realistic size
};

int main() {
    // --- Test 1: compile-time capacity, basic push/drain ---
    {
        adt::MpscRingBuffer<TestPayload, 16> rb;
        assert(rb.empty());
        assert(rb.size() == 0);

        TestPayload p{42};
        assert(rb.try_push(p));
        assert(!rb.empty());
        assert(rb.size() == 1);

        int count = 0;
        rb.drain([&](const TestPayload& e) {
            assert(e.value == 42);
            count++;
        });
        assert(count == 1);
        assert(rb.empty());
    }

    // --- Test 2: compile-time, drain ordering ---
    {
        adt::MpscRingBuffer<TestPayload, 16> rb;
        for (int i = 0; i < 10; ++i) {
            TestPayload p{i};
            assert(rb.try_push(p));
        }
        assert(rb.size() == 10);

        int expected = 0;
        rb.drain([&](const TestPayload& e) {
            assert(e.value == expected);
            expected++;
        });
        assert(expected == 10);
        assert(rb.empty());
    }

    // --- Test 3: compile-time, overflow + events_lost ---
    {
        adt::MpscRingBuffer<TestPayload, 4> rb;
        TestPayload p{1};
        for (int i = 0; i < 4; ++i) {
            assert(rb.try_push(p));
        }
        assert(rb.size() == 4);
        assert(!rb.try_push(p)); // full — should fail
        assert(rb.events_lost() == 1);
    }

    // --- Test 4: compile-time, drain-after-overflow preserves elements ---
    {
        adt::MpscRingBuffer<TestPayload, 8> rb;
        TestPayload p{7};
        for (int i = 0; i < 8; ++i) {
            assert(rb.try_push(p));
        }
        assert(!rb.try_push(p)); // overflow
        assert(rb.events_lost() == 1);

        int drained = 0;
        rb.drain([&](const TestPayload&) { drained++; });
        assert(drained == 8);
        assert(rb.empty());
    }

    // --- Test 5: compile-time, concurrent producers + consumer ---
    {
        adt::MpscRingBuffer<TestPayload, 1024> rb;
        constexpr int kEventsPerThread = 500;
        constexpr int kThreads = 4;
        constexpr int kTotal = kEventsPerThread * kThreads;

        std::atomic<int> total_drained{0};
        std::atomic<bool> done{false};

        std::thread consumer([&]() {
            while (!done.load(std::memory_order_acquire) || !rb.empty()) {
                rb.drain([&](const TestPayload&) {
                    total_drained.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });

        std::vector<std::thread> producers;
        for (int t = 0; t < kThreads; ++t) {
            producers.emplace_back([&rb, t]() {
                for (int i = 0; i < kEventsPerThread; ++i) {
                    TestPayload p{t * 1000 + i};
                    while (!rb.try_push(p)) {
                        // spin until consumer drains
                    }
                }
            });
        }

        for (auto& th : producers) th.join();
        done.store(true, std::memory_order_release);
        consumer.join();

        assert(total_drained.load() == kTotal);
        assert(rb.empty());
    }

    // --- Test 6: dynamic capacity, basic push/drain ---
    {
        adt::DynamicMpscRingBuffer<TestPayload> rb(16);
        assert(rb.empty());

        TestPayload p{99};
        assert(rb.try_push(p));
        assert(rb.size() == 1);

        int count = 0;
        rb.drain([&](const TestPayload& e) {
            assert(e.value == 99);
            count++;
        });
        assert(count == 1);
        assert(rb.empty());
    }

    // --- Test 7: dynamic, overflow ---
    {
        adt::DynamicMpscRingBuffer<TestPayload> rb(4);
        TestPayload p{};
        for (int i = 0; i < 4; ++i) {
            assert(rb.try_push(p));
        }
        assert(!rb.try_push(p));
        assert(rb.events_lost() == 1);
    }

    // --- Test 8: dynamic, concurrent producers ---
    {
        adt::DynamicMpscRingBuffer<TestPayload> rb(1024);
        constexpr int kEventsPerThread = 500;
        constexpr int kThreads = 4;
        constexpr int kTotal = kEventsPerThread * kThreads;

        std::atomic<int> total_drained{0};
        std::atomic<bool> done{false};

        std::thread consumer([&]() {
            while (!done.load(std::memory_order_acquire) || !rb.empty()) {
                rb.drain([&](const TestPayload&) {
                    total_drained.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });

        std::vector<std::thread> producers;
        for (int t = 0; t < kThreads; ++t) {
            producers.emplace_back([&rb, t]() {
                for (int i = 0; i < kEventsPerThread; ++i) {
                    TestPayload p{t * 1000 + i};
                    while (!rb.try_push(p)) {
                        // spin until consumer drains
                    }
                }
            });
        }

        for (auto& th : producers) th.join();
        done.store(true, std::memory_order_release);
        consumer.join();

        assert(total_drained.load() == kTotal);
    }

    // --- Test 9: dynamic, capacity validation ---
    {
        // power of two: OK
        {
            adt::DynamicMpscRingBuffer<TestPayload> rb(64);
            assert(rb.empty());
        }

        // zero: should abort
        {
            bool zero_aborted = false;
            // We can't safely test abort() in-process, so skip.
            // The constructor asserts/aborts on invalid capacity.
            (void)zero_aborted;
        }
    }

    // --- Test 10: per-slot publish protocol under stress ---
    // Multiple producers pushing to adjacent slots while consumer drains.
    // TSAN will catch any consumer read of an unpublished slot.
    {
        adt::MpscRingBuffer<TestPayload, 256> rb;
        std::atomic<uint64_t> total_pushed{0};
        std::atomic<uint64_t> total_consumed{0};
        std::atomic<bool> stop{false};

        std::thread consumer([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                rb.drain([&](const TestPayload&) {
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                });
            }
            // final drain
            rb.drain([&](const TestPayload&) {
                total_consumed.fetch_add(1, std::memory_order_relaxed);
            });
        });

        constexpr int kProducers = 8;
        std::vector<std::thread> producers;
        for (int t = 0; t < kProducers; ++t) {
            producers.emplace_back([&rb, &total_pushed]() {
                for (int i = 0; i < 5000; ++i) {
                    TestPayload p{i};
                    if (rb.try_push(p)) {
                        total_pushed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (auto& th : producers) th.join();
        stop.store(true, std::memory_order_release);
        consumer.join();

        assert(total_consumed.load() == total_pushed.load());
        assert(rb.empty());
    }

    std::cout << "test_adt_mpsc_ring_buffer: PASS\n";
    return 0;
}
```

- [ ] **Step 2: Register the test in tests/CMakeLists.txt**

Before the line `# =============================================================================
# Core tests`, add a new ADT section:

The edit point is right after `add_compile_options(-UNDEBUG)` at line 9, before the `# ===` separator at line 11. Insert:

```cmake
# =============================================================================
# ADT tests - abstract data types
# =============================================================================
add_executable(test_adt_mpsc_ring_buffer adt/test_adt_mpsc_ring_buffer.cpp)
target_link_libraries(test_adt_mpsc_ring_buffer hpactor pthread)
add_test(NAME test_adt_mpsc_ring_buffer COMMAND test_adt_mpsc_ring_buffer)

```

- [ ] **Step 3: Verify the test fails to compile (header missing)**

```bash
cmake -S . -B build -GNinja && ninja -C build test_adt_mpsc_ring_buffer 2>&1 | head -20
```

Expected: compilation error — `fatal error: 'hpactor/adt/mpsc_ring_buffer.hpp' file not found`

- [ ] **Step 4: Commit**

```bash
git add tests/adt/test_adt_mpsc_ring_buffer.cpp tests/CMakeLists.txt
git commit -m "test(adt): add failing MPSC ring buffer ADT tests"
```

---

### Task 2: Implement the shared ADT header

**Files:**
- Create: `include/hpactor/adt/mpsc_ring_buffer.hpp`

- [ ] **Step 1: Create the ADT header**

Create `include/hpactor/adt/mpsc_ring_buffer.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace hpactor::adt {

// =============================================================================
// MpscRingBuffer<T, Capacity> — MPSC ring buffer with compile-time capacity.
//
// Multi-producer, single-consumer. Capacity must be a power of two.
//
// Concurrency protocol (per-slot publish/sequence):
//   Each slot i has a sequence number seq_[i], initialized to i.
//   Producer: fetch_add write_cursor_ → write payload → seq_[slot].store(w+1, release)
//   Consumer: wait for seq_[slot].load(acquire) == r+1 → read → seq_[slot].store(r+Cap, release)
//
// This closes the race where a consumer sees an advanced write cursor before
// the producer has written the payload.
// =============================================================================
template <typename T, size_t Capacity = 65536>
class MpscRingBuffer {
    static_assert(Capacity >= 2, "Capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    static constexpr size_t kDefaultCapacity = Capacity;

    MpscRingBuffer() : buffer_(Capacity), seq_(Capacity) {
        // Initialize sequence numbers: slot i expects write number i.
        for (size_t i = 0; i < Capacity; ++i) {
            seq_[i].store(i, std::memory_order_relaxed);
        }
    }

    // Try to push a value. Returns false if the buffer is full.
    // Safe to call from any number of threads concurrently.
    bool try_push(const T& value) noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t slot = w & mask_;
            // Slot is writable when its sequence number equals w.
            // The acquire load synchronizes with the consumer's release store,
            // ensuring we see the slot as free only after the consumer has
            // finished reading the previous occupant.
            if (seq_[slot].load(std::memory_order_acquire) != w) {
                events_lost_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (write_cursor_.compare_exchange_weak(w, w + 1,
                    std::memory_order_relaxed)) {
                break;
            }
        }
        // w is now exclusively claimed by this producer.
        buffer_[w & mask_] = value;
        // Publish: the release store ensures the buffer_ write is visible
        // before the consumer observes the published sequence number.
        seq_[w & mask_].store(w + 1, std::memory_order_release);
        return true;
    }

    // Drain all published elements. Must be called from a single consumer thread.
    template <typename Fn>
    size_t drain(Fn&& callback) {
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        size_t count = 0;
        for (;;) {
            uint64_t slot = r & mask_;
            // Slot is ready when its sequence number equals r + 1.
            // The acquire load synchronizes with the producer's release store,
            // ensuring the payload write is visible.
            if (seq_[slot].load(std::memory_order_acquire) != (r + 1)) {
                break;
            }
            callback(buffer_[slot]);
            // Mark slot free for reuse. The release store ensures buffer_ read
            // completes before a producer observes the freed slot.
            seq_[slot].store(r + Capacity, std::memory_order_release);
            ++r;
            ++count;
        }
        read_cursor_.store(r, std::memory_order_release);
        return count;
    }

    uint64_t events_lost() const noexcept {
        return events_lost_.load(std::memory_order_relaxed);
    }

    size_t size() const noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        uint64_t r = read_cursor_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    bool empty() const noexcept {
        return size() == 0;
    }

private:
    static constexpr size_t mask_ = Capacity - 1;

    alignas(64) std::atomic<uint64_t> write_cursor_{0};
    alignas(64) std::atomic<uint64_t> read_cursor_{0};
    alignas(64) std::atomic<uint64_t> events_lost_{0};
    std::vector<T> buffer_;
    std::vector<std::atomic<uint64_t>> seq_;
};

// =============================================================================
// DynamicMpscRingBuffer<T> — MPSC ring buffer with runtime capacity.
//
// Same concurrency contract as MpscRingBuffer but capacity is specified at
// construction time. Uses std::unique_ptr<T[]> for storage since the capacity
// is not a compile-time constant.
// =============================================================================
template <typename T>
class DynamicMpscRingBuffer {
public:
    // capacity must be > 0 and a power of two.
    explicit DynamicMpscRingBuffer(size_t capacity)
        : capacity_(capacity)
        , mask_(capacity - 1)
        , buffer_(std::make_unique<T[]>(capacity))
        , seq_(std::make_unique<std::atomic<uint64_t>[]>(capacity))
    {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            std::fprintf(stderr,
                         "DynamicMpscRingBuffer: capacity must be a power of two, got %zu\n",
                         capacity);
            std::abort();
        }
        for (size_t i = 0; i < capacity; ++i) {
            seq_[i].store(i, std::memory_order_relaxed);
        }
    }

    bool try_push(const T& value) noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t slot = w & mask_;
            if (seq_[slot].load(std::memory_order_acquire) != w) {
                events_lost_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (write_cursor_.compare_exchange_weak(w, w + 1,
                    std::memory_order_relaxed)) {
                break;
            }
        }
        buffer_[w & mask_] = value;
        seq_[w & mask_].store(w + 1, std::memory_order_release);
        return true;
    }

    template <typename Fn>
    size_t drain(Fn&& callback) {
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        size_t count = 0;
        for (;;) {
            uint64_t slot = r & mask_;
            if (seq_[slot].load(std::memory_order_acquire) != (r + 1)) {
                break;
            }
            callback(buffer_[slot]);
            seq_[slot].store(r + capacity_, std::memory_order_release);
            ++r;
            ++count;
        }
        read_cursor_.store(r, std::memory_order_release);
        return count;
    }

    uint64_t events_lost() const noexcept {
        return events_lost_.load(std::memory_order_relaxed);
    }

    size_t size() const noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        uint64_t r = read_cursor_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    bool empty() const noexcept {
        return size() == 0;
    }

private:
    size_t capacity_;
    size_t mask_;
    std::unique_ptr<T[]> buffer_;
    std::unique_ptr<std::atomic<uint64_t>[]> seq_;

    alignas(64) std::atomic<uint64_t> write_cursor_{0};
    alignas(64) std::atomic<uint64_t> read_cursor_{0};
    alignas(64) std::atomic<uint64_t> events_lost_{0};
};

} // namespace hpactor::adt
```

- [ ] **Step 2: Build and run the new ADT test**

```bash
cmake -S . -B build -GNinja && ninja -C build test_adt_mpsc_ring_buffer
./build/tests/test_adt_mpsc_ring_buffer
```

Expected: `test_adt_mpsc_ring_buffer: PASS`

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/adt/mpsc_ring_buffer.hpp
git commit -m "feat(adt): add MPSC ring buffer with per-slot publish/sequence protocol"
```

---

### Task 3: Migrate metrics::MpscRingBuffer to a using-alias

**Files:**
- Modify: `include/hpactor/metrics/metrics_ring_buffer.hpp`

- [ ] **Step 1: Replace the implementation with an alias**

Replace the entire content of `include/hpactor/metrics/metrics_ring_buffer.hpp` with:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/adt/mpsc_ring_buffer.hpp>

namespace hpactor::metrics {

// Alias for backward compatibility — delegates to the shared ADT.
template <typename T, size_t Capacity = 65536>
using MpscRingBuffer = adt::MpscRingBuffer<T, Capacity>;

} // namespace hpactor::metrics
```

The old file was ~90 lines; the new file is ~15 lines.

- [ ] **Step 2: Build and run all metrics-related tests**

```bash
cmake -S . -B build -GNinja && ninja -C build && ctest --output-on-failure -R "test_metrics"
```

Expected: all metrics tests pass (test_metrics_registry, test_metrics_integration).

- [ ] **Step 3: Build and run all tracing tests**

```bash
ninja -C build && ctest --output-on-failure -R "test_trace"
```

Expected: all 12 tracing tests pass. `TraceManager` uses `metrics::MpscRingBuffer<SpanRecord>` which now resolves to the ADT.

- [ ] **Step 4: Build and run all scheduler tests**

```bash
ninja -C build && ctest --output-on-failure -R "test_sched"
```

Expected: all scheduler tests pass. `HybridScheduler` and `EventBasedActor` use `metrics::MpscRingBuffer<MetricEvent>*`.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/metrics/metrics_ring_buffer.hpp
git commit -m "refactor(metrics): replace MpscRingBuffer with alias to adt::MpscRingBuffer"
```

---

### Task 4: Migrate mem::TelemetryRingBuffer to the ADT

**Files:**
- Modify: `include/hpactor/mem/telemetry_ring_buffer.hpp`

- [ ] **Step 1: Update the include and alias**

Replace the content of `include/hpactor/mem/telemetry_ring_buffer.hpp`:

Old:
```cpp
#include <hpactor/metrics/metrics_ring_buffer.hpp>
// ...
using TelemetryRingBuffer = metrics::MpscRingBuffer<AllocationEvent, Capacity>;
```

New:
```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/adt/mpsc_ring_buffer.hpp>
#include <cstdint>

namespace hpactor::mem {

// Allocation event for telemetry. Compact (32 bytes) for ring buffer density.
struct AllocationEvent {
    uint64_t timestamp;   // rdtsc or monotonic ns
    uint32_t actor_id;    // owning actor
    uint16_t block_size;  // user bytes requested
    uint8_t  size_class;  // SizeClass index
    uint8_t  region_type; // RegionType
    uint8_t  event_type;  // 0=alloc, 1=free, 2=corruption, 3=hibernate_in, 4=hibernate_out
    uint8_t  _pad[7];     // align to 32B
};

// Alias for backward compatibility — delegates to the shared ADT.
template <size_t Capacity = 65536>
using TelemetryRingBuffer = adt::MpscRingBuffer<AllocationEvent, Capacity>;

} // namespace hpactor::mem
```

The key change: `#include <hpactor/metrics/metrics_ring_buffer.hpp>` becomes `#include <hpactor/adt/mpsc_ring_buffer.hpp>`, and `metrics::MpscRingBuffer` becomes `adt::MpscRingBuffer` in the alias.

- [ ] **Step 2: Build and run the telemetry ring buffer test**

```bash
cmake -S . -B build -GNinja && ninja -C build test_telemetry_ring_buffer && ./build/tests/test_telemetry_ring_buffer
```

Expected: `test_telemetry_ring_buffer: PASS`

- [ ] **Step 3: Run all memory tests**

```bash
ninja -C build && ctest --output-on-failure -R "test_mem"
```

Expected: all memory tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mem/telemetry_ring_buffer.hpp
git commit -m "refactor(mem): point TelemetryRingBuffer alias to adt::MpscRingBuffer"
```

---

### Task 5: Migrate log::LogRingBuffer to wrap DynamicMpscRingBuffer

**Files:**
- Modify: `include/hpactor/log/log_ring_buffer.hpp`

- [ ] **Step 1: Replace the implementation with a wrapper**

Replace the entire content of `include/hpactor/log/log_ring_buffer.hpp` with:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/adt/mpsc_ring_buffer.hpp>
#include <hpactor/log/log_event.hpp>

namespace hpactor::log {

// Thin wrapper around adt::DynamicMpscRingBuffer<LogEvent>.
// Preserves the LogRingBuffer type identity for existing callers
// (LogManager, LogDrain, Logger, test_log_ring_buffer).
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

    uint64_t events_lost() const noexcept {
        return buffer_.events_lost();
    }

    size_t size() const noexcept {
        return buffer_.size();
    }

    bool empty() const noexcept {
        return buffer_.empty();
    }

private:
    adt::DynamicMpscRingBuffer<LogEvent> buffer_;
};

} // namespace hpactor::log
```

The old file was ~103 lines with raw atomics and custom buffer management. The new file is ~40 lines — wrapper with zero overhead (inlined delegation).

- [ ] **Step 2: Build and run the log ring buffer test**

```bash
cmake -S . -B build -GNinja && ninja -C build test_log_ring_buffer && ./build/tests/test_log_ring_buffer
```

Expected: test passes without modification (the `LogRingBuffer` API is unchanged — `try_push`, `drain`, `events_lost`, `size`, `empty` all work identically).

- [ ] **Step 3: Run all log tests**

```bash
ninja -C build && ctest --output-on-failure -R "test_log"
```

Expected: all 7 log tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/log/log_ring_buffer.hpp
git commit -m "refactor(log): replace LogRingBuffer impl with wrapper around adt::DynamicMpscRingBuffer"
```

---

### Task 6: Full test suite verification

**Files:** None (verification only)

- [ ] **Step 1: Full clean build**

```bash
rm -rf build && cmake -S . -B build -GNinja && ninja -C build
```

Expected: clean build, zero warnings, no errors.

- [ ] **Step 2: Run all 141 tests**

```bash
ctest --output-on-failure --parallel 8
```

Expected: all 141 tests pass (140 existing + 1 new `test_adt_mpsc_ring_buffer`).

- [ ] **Step 3: Verify no regressions with TSAN**

```bash
rm -rf build && cmake -S . -B build -GNinja -DENABLE_TSAN=ON && ninja -C build && ctest --output-on-failure --parallel 4
```

Expected: all tests pass cleanly under TSAN. TSAN would catch any remaining data race if the publish/sequence protocol were incorrect.

- [ ] **Step 4: Verify consumer file count is unchanged**

```bash
grep -r "metrics_ring_buffer.hpp" include/hpactor/ --include="*.hpp" | grep -v "metrics/metrics_ring_buffer.hpp" | wc -l
```

Expected: ~15 consumer files (scheduler, mailbox, actor_system, event_based_actor, supervision, trace_manager, metrics_actor, etc.).
Confirm no consumer files were modified — they all include the existing `metrics_ring_buffer.hpp` header.

```bash
grep -r "log_ring_buffer.hpp" include/hpactor/ --include="*.hpp" | grep -v "log/log_ring_buffer.hpp" | wc -l
```

Expected: ~5 consumer files (logger, log_drain, log_manager). Confirm none were modified.

- [ ] **Step 5: Commit (if any stray changes)**

```bash
git status
```

If clean — done. If any CMake file has the new test registration from Task 1, it should already be committed.
