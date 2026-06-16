# Adaptive Time-Based Worker Backoff Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace magic-number backoff constants with an adaptive, self-calibrating mechanism that achieves near-zero idle CPU on Linux while preserving fast wakeup on work arrival.

**Architecture:** Three additions to `WorkerThread`: (1) a `BackoffCalibration` struct populated once by an OS-level probe, (2) wall-clock-based `backoff(elapsed)` replacing iteration-count `backoff()`, and (3) exponential CV timeout replacing the fixed 100ms default. Test injection allows deterministic calibration for unit tests.

**Tech Stack:** C++20, `std::chrono`, `std::call_once`, `std::atomic`, `pthread` / `sched_yield`, Google Test

**Spec:** `docs/superpowers/specs/2026-06-15-adaptive-time-based-backoff-design.md`

---

### Task 1: BackoffCalibration struct, test injection plumbing, WorkerSnapshot extension

**Files:**
- Modify: `include/hpactor/sched/worker_thread.hpp`
- Modify: `include/hpactor/sched/scheduler.hpp` (WorkerSnapshot)
- Create: `tests/unit/sched/test_worker_backoff.cpp`
- Modify: `tests/unit/sched/CMakeLists.txt`

- [ ] **Step 1: Write failing tests for calibration injection and snapshot fields**

Create `tests/unit/sched/test_worker_backoff.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/sched/worker_thread.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <gtest/gtest.h>

using namespace hpactor::sched;

// ── BackoffCalibration defaults ──────────────────────────────────────

TEST(WorkerBackoffTest, CalibrationDefaultYieldIsEffective) {
    BackoffCalibration cal;
    // Safe defaults: assume yield is NOT effective (safer for Linux).
    EXPECT_FALSE(cal.yield_is_effective);
}

TEST(WorkerBackoffTest, CalibrationDefaultMinSleepIsSane) {
    BackoffCalibration cal;
    EXPECT_GE(cal.min_effective_sleep_ns, 1'000u);   // at least 1µs
    EXPECT_LE(cal.min_effective_sleep_ns, 10'000'000u); // at most 10ms
}

TEST(WorkerBackoffTest, CalibrationDefaultPollingBudgetIsSane) {
    BackoffCalibration cal;
    EXPECT_GE(cal.polling_budget_ns, 1'000'000u);     // at least 1ms
    EXPECT_LE(cal.polling_budget_ns, 100'000'000u);   // at most 100ms
}

// ── Test calibration injection ────────────────────────────────────────

TEST(WorkerBackoffTest, TestCalibrationInjection) {
    BackoffCalibration cal;
    cal.yield_is_effective = true;
    cal.min_effective_sleep_ns = 123'456;
    cal.spin_threshold_ns = 20'000;
    cal.polling_budget_ns = 50'000'000;

    WorkerThread::set_test_calibration(&cal);

    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);
    worker.start();

    // Worker should use the injected calibration.
    const auto& used = worker.calibration();
    EXPECT_TRUE(used.yield_is_effective);
    EXPECT_EQ(used.min_effective_sleep_ns, 123'456u);
    EXPECT_EQ(used.spin_threshold_ns, 20'000u);
    EXPECT_EQ(used.polling_budget_ns, 50'000'000u);

    worker.stop();
    WorkerThread::set_test_calibration(nullptr);
}

TEST(WorkerBackoffTest, TestCalibrationClearedAfterNull) {
    // After setting nullptr, a new worker should use real calibration (probe).
    WorkerThread::set_test_calibration(nullptr);
    // We can't assert specific probe values, but we can verify the worker
    // starts and runs without test calibration.
    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);
    worker.start();
    const auto& used = worker.calibration();
    // Probe should have run and produced sane values.
    EXPECT_GE(used.min_effective_sleep_ns, 1'000u);
    EXPECT_LE(used.polling_budget_ns, 100'000'000u);
    worker.stop();
}

// ── WorkerSnapshot new fields ─────────────────────────────────────────

TEST(WorkerBackoffTest, WorkerSnapshotHasNewFields) {
    WorkerSnapshot ws;
    // Fields should be default-constructed.
    EXPECT_FALSE(ws.calibration_yield_effective);
    EXPECT_EQ(ws.calibration_min_sleep_ns, 0u);
    EXPECT_EQ(ws.consecutive_empty_wakes, 0u);
}
```

- [ ] **Step 2: Verify tests fail to compile**

```bash
ninja -C build test_unit_sched
```
Expected: COMPILE ERROR — `BackoffCalibration` not defined, `set_test_calibration` not declared, `WorkerSnapshot` missing fields.

- [ ] **Step 3: Add BackoffCalibration struct and plumbing to worker_thread.hpp**

In `include/hpactor/sched/worker_thread.hpp`, add before the `WorkerThread` class (around line 33):

```cpp
/// \brief OS scheduling characteristics measured at startup.
///
/// Populated once by a calibration probe that runs on the first worker
/// thread.  Values are immutable after construction.  Tests may inject a
/// fixed calibration via \c set_test_calibration() before \c start().
struct BackoffCalibration {
    /// \brief Whether \c sched_yield() actually yields the CPU.
    ///
    /// \c true on macOS/Mach where \c thread_switch() deschedules the
    /// caller.  \c false on Linux/CFS where yield merely rotates the
    /// run-queue and the caller is immediately rescheduled.
    bool yield_is_effective = false;

    /// \brief Minimum sleep duration the kernel can reliably honour.
    ///
    /// Derived from the knee in the actual-vs-requested sleep curve.
    /// Typically 10-50 µs on modern Linux with hrtimers; 1-4 ms on
    /// older kernels or virtualized environments.
    uint32_t min_effective_sleep_ns = 50'000;

    /// \brief How long to spin (yield) before escalating to nanosleep.
    ///
    /// 0 when \c yield_is_effective is \c false (no point spinning on
    /// Linux).  ~20 µs when yield actually deschedules the caller.
    uint32_t spin_threshold_ns = 0;

    /// \brief Total wall-clock idle time before escalating from polling
    ///        backoff to CV blocking.
    ///
    /// Default 10 ms.  Increased proportionally on systems with coarse
    /// timer granularity.
    uint32_t polling_budget_ns = 10'000'000;
};
```

Inside the `WorkerThread` class, add public test-injection API (after the diagnostic accessors block, around line 338):

```cpp
    // ── Calibration (test injection + runtime access) ───────────────

    /// \brief Inject a fixed calibration for deterministic testing.
    ///
    /// When set (non-null), the startup probe is skipped and this
    /// calibration is used instead.  Call with \c nullptr to restore
    /// auto-calibration for subsequent workers.
    ///
    /// \note Not thread-safe — call before any worker \c start().
    static void set_test_calibration(const BackoffCalibration* cal) {
        test_calibration_override_ = cal;
    }

    /// \brief Read the calibration in effect for this worker.
    ///
    /// \return Immutable reference to the calibration used by this worker.
    const BackoffCalibration& calibration() const {
        return calibration_;
    }
```

Add new diagnostic accessors:

```cpp
    /// \brief Whether the OS yield operation actually deschedules the caller.
    bool diag_yield_is_effective() const {
        return calibration_.yield_is_effective;
    }
    /// \brief Measured kernel timer granularity in nanoseconds.
    uint32_t diag_min_sleep_ns() const {
        return calibration_.min_effective_sleep_ns;
    }
    /// \brief Consecutive CV wakeups that found no work (exponential
    ///        timeout level).
    uint32_t diag_consecutive_empty_wakes() const {
        return consecutive_empty_wakes_.load(std::memory_order_relaxed);
    }
```

Add new private members:

```cpp
    /// \brief Calibration values used by this worker (copied from shared probe
    ///        or test override at startup).
    BackoffCalibration calibration_;

    /// \brief Wall-clock time when the worker first became idle.
    ///
    /// Default-constructed (epoch) means "not idle."  Set on the first idle
    /// iteration and cleared when work is found.
    std::chrono::steady_clock::time_point idle_since_{};

    /// \brief Number of consecutive CV timeout wakeups that found no work.
    ///
    /// Drives exponential growth of the safety-net CV timeout.  Reset to 0
    /// when work is found.  Atomic for snapshot reads from CLI/metrics threads.
    std::atomic<uint32_t> consecutive_empty_wakes_{0};

    /// \brief Whether the worker is currently inside the CV-blocking idle model.
    ///
    /// Set \c true when the worker actually sleeps on \c sleep_cv_, cleared
    /// when work is processed.  Used by \c diag_is_in_cv_model().
    std::atomic<bool> in_cv_model_{false};

    /// \brief Shared result of the one-time calibration probe.
    static BackoffCalibration shared_calibration_;
    /// \brief One-time flag guarding the calibration probe.
    static std::once_flag calibration_once_;
    /// \brief Test override — when non-null, skip the probe and use this.
    static const BackoffCalibration* test_calibration_override_;
```

- [ ] **Step 4: Add new fields to WorkerSnapshot in scheduler.hpp**

In `include/hpactor/sched/scheduler.hpp`, in the `WorkerSnapshot` struct (around line 70), add after `idle_model`:

```cpp
    // Adaptive backoff calibration diagnostics
    bool calibration_yield_effective{false};
    uint32_t calibration_min_sleep_ns{0};
    uint32_t consecutive_empty_wakes{0};
```

- [ ] **Step 5: Add test file to CMakeLists.txt**

In `tests/unit/sched/CMakeLists.txt`, add `test_worker_backoff.cpp` to the `add_executable` list:

```
add_executable(test_unit_sched
    test_edf_queue.cpp
    ...
    test_lost_wakeup_rate_limit.cpp
    test_worker_backoff.cpp
)
```

- [ ] **Step 6: Build and run tests to verify compilation and test failures**

```bash
ninja -C build test_unit_sched
./build/tests/unit/sched/test_unit_sched --gtest_filter="*WorkerBackoff*"
```
Expected: CalibrationDefault* tests PASS, TestCalibrationInjection PASS, TestCalibrationClearedAfterNull PASS, WorkerSnapshotHasNewFields PASS.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/sched/worker_thread.hpp include/hpactor/sched/scheduler.hpp tests/unit/sched/test_worker_backoff.cpp tests/unit/sched/CMakeLists.txt
git commit -m "feat(sched): add BackoffCalibration struct and test injection plumbing

Introduce BackoffCalibration with safe defaults (no-yield, 50us min sleep,
10ms polling budget). Add static set_test_calibration() for deterministic
test injection. Extend WorkerSnapshot with calibration and empty-wake
diag fields. Add new WorkerBackoffTest unit test file.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Calibration probe implementation

**Files:**
- Modify: `src/sched/worker_thread.cpp`
- Modify: `tests/unit/sched/test_worker_backoff.cpp`

- [ ] **Step 1: Write a failing test for the calibration probe**

Add to `tests/unit/sched/test_worker_backoff.cpp`:

```cpp
// ── Calibration probe ─────────────────────────────────────────────────

TEST(WorkerBackoffTest, CalibrationProbeYieldIsEffective) {
    WorkerThread::set_test_calibration(nullptr);
    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);
    worker.start();

    const auto& cal = worker.calibration();
    // Sanity: min_effective_sleep_ns should be in [1us, 10ms].
    EXPECT_GE(cal.min_effective_sleep_ns, 1'000u);
    EXPECT_LE(cal.min_effective_sleep_ns, 10'000'000u);

    // Sanity: polling_budget_ns should be >= 10ms and <= 100ms.
    EXPECT_GE(cal.polling_budget_ns, 10'000'000u);
    EXPECT_LE(cal.polling_budget_ns, 100'000'000u);

    // spin_threshold_ns is 0 when yield is not effective, 20'000 otherwise.
    if (!cal.yield_is_effective) {
        EXPECT_EQ(cal.spin_threshold_ns, 0u);
    } else {
        EXPECT_EQ(cal.spin_threshold_ns, 20'000u);
    }

    worker.stop();
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*CalibrationProbe*"
```
Expected: FAIL or produce default values (probe not yet implemented).

- [ ] **Step 3: Implement the calibration probe in worker_thread.cpp**

In `src/sched/worker_thread.cpp`, add static member definitions (after the file-scope constants):

```cpp
// ── Calibration probe (static state) ──────────────────────────────────
BackoffCalibration WorkerThread::shared_calibration_;
std::once_flag WorkerThread::calibration_once_;
const BackoffCalibration* WorkerThread::test_calibration_override_ = nullptr;

namespace {

BackoffCalibration run_calibration_probe() {
    BackoffCalibration cal;

    // 1. Yield effectiveness: time 1000 consecutive sched_yield() calls.
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i) {
            std::this_thread::yield();
        }
        auto t1 = std::chrono::steady_clock::now();
        auto elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count();
        // Pure spin: ~10-50 ns/iter -> ~10-50 us total for 1000 iters.
        // Real yield: ~1-10 us/iter -> ~1-10 ms total for 1000 iters.
        cal.yield_is_effective = (elapsed_ns > 500'000);
    }

    // 2. Timer granularity: sleep at increasing durations, measure actual.
    {
        constexpr uint32_t kDurationsNs[] = {
            1'000, 10'000, 50'000, 100'000, 500'000, 1'000'000};
        uint32_t best = 50'000; // fallback
        for (uint32_t dur_ns : kDurationsNs) {
            auto t0 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::nanoseconds(dur_ns));
            auto t1 = std::chrono::steady_clock::now();
            auto actual_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count();
            if (actual_ns > 0 &&
                actual_ns <= static_cast<int64_t>(dur_ns) * 4) {
                best = dur_ns;
                break;
            }
        }
        cal.min_effective_sleep_ns = best;
    }

    // 3. Derived thresholds.
    cal.spin_threshold_ns = cal.yield_is_effective ? 20'000 : 0;
    cal.polling_budget_ns =
        std::max(10'000'000u, cal.min_effective_sleep_ns * 100);

    return cal;
}

} // anonymous namespace
```

Update `WorkerThread::start()` to populate `calibration_` (insert after the `running_` and `stop_requested_` stores):

```cpp
void WorkerThread::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    running_.store(true, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);

    // Populate calibration: use test override if set, otherwise run probe once.
    if (test_calibration_override_) {
        calibration_ = *test_calibration_override_;
    } else {
        std::call_once(calibration_once_, [] {
            shared_calibration_ = run_calibration_probe();
        });
        calibration_ = shared_calibration_;
    }

    thread_ = std::thread([this] {
        // ... existing thread body ...
    });
}
```

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*WorkerBackoff*"
```
Expected: All tests pass.

- [ ] **Step 5: Run existing worker thread tests**

```bash
ninja -C build && ctest -R "WorkerThread" --output-on-failure
```
Expected: All existing WorkerThread tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/sched/worker_thread.cpp tests/unit/sched/test_worker_backoff.cpp
git commit -m "feat(sched): implement OS calibration probe

Run once on first worker thread start. Measures:
- yield_is_effective: whether sched_yield() actually deschedules
- min_effective_sleep_ns: kernel timer granularity
Derives spin_threshold_ns and polling_budget_ns from measurements.

Guarded by std::call_once. Test override via set_test_calibration()
lets tests inject deterministic values.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Time-based backoff(elapsed) replacing iteration-count backoff()

**Files:**
- Modify: `src/sched/worker_thread.cpp` — `backoff()`, `try_poll_idle()`, `reset_backoff()`, `process_work_item()`, `thread_loop()`
- Modify: `include/hpactor/sched/worker_thread.hpp` — `backoff()` signature, `reset_backoff()`
- Modify: `tests/unit/sched/test_worker_backoff.cpp` — add time-based backoff tests

- [ ] **Step 1: Write failing tests for time-based backoff behavior**

Add to `tests/unit/sched/test_worker_backoff.cpp`:

```cpp
// ── Time-based backoff ────────────────────────────────────────────────

TEST(WorkerBackoffTest, WorkerReachesCvModelWithInjectedCalibration) {
    BackoffCalibration cal;
    cal.yield_is_effective = false;
    cal.min_effective_sleep_ns = 1'000;        // 1us (fast)
    cal.spin_threshold_ns = 0;                  // no spin
    cal.polling_budget_ns = 5'000'000;          // 5ms polling budget

    WorkerThread::set_test_calibration(&cal);

    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);
    worker.start();

    // Wait for the worker to escalate to CV model.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker.diag_is_in_cv_model() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(worker.diag_is_in_cv_model());
    EXPECT_GT(worker.diag_cv_escalations(), 0u);
    EXPECT_GT(worker.diag_idle_iters(), 0u);

    worker.stop();
    WorkerThread::set_test_calibration(nullptr);
}

TEST(WorkerBackoffTest, BackoffResetsWhenWorkFound) {
    BackoffCalibration cal;
    cal.yield_is_effective = false;
    cal.min_effective_sleep_ns = 1'000;
    cal.spin_threshold_ns = 0;
    cal.polling_budget_ns = 10'000'000;  // 10ms

    WorkerThread::set_test_calibration(&cal);

    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);

    std::atomic<bool> work_processed{false};
    worker.set_work_processor([&](const WorkItem&) {
        work_processed.store(true);
    });

    worker.start();

    // Let worker go idle and enter CV.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker.diag_is_in_cv_model() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(worker.diag_is_in_cv_model());

    // Push work.
    WorkItem item;
    item.actor = ActorId{0};
    worker.push(0, item);

    // Wait for work to be processed.
    auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!work_processed.load() &&
           std::chrono::steady_clock::now() < deadline2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(work_processed.load());

    // After processing work, worker should be out of CV model.
    EXPECT_FALSE(worker.diag_is_in_cv_model());

    worker.stop();
    WorkerThread::set_test_calibration(nullptr);
}
```

- [ ] **Step 2: Run tests to confirm failure**

```bash
ninja -C build test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*WorkerReachesCvModel*:*BackoffResets*"
```
Expected: `WorkerReachesCvModelWithInjectedCalibration` FAIL — `diag_is_in_cv_model()` still checks old mechanism.

- [ ] **Step 3: Implement time-based backoff(elapsed)**

Replace the existing `backoff()` method in `src/sched/worker_thread.cpp`:

```cpp
void WorkerThread::backoff(std::chrono::nanoseconds elapsed) {
    uint64_t ns = static_cast<uint64_t>(elapsed.count());

    // Stage 0: spin (yield) only when yield is effective.
    if (ns < calibration_.spin_threshold_ns) {
        if (calibration_.yield_is_effective) {
            std::this_thread::yield();
        }
        return;
    }

    // Stage 1: proportional sleep for the first 1 ms of idle time.
    if (ns < 1'000'000) {
        uint64_t sleep_ns = ns / 4;
        if (sleep_ns < calibration_.min_effective_sleep_ns) {
            sleep_ns = calibration_.min_effective_sleep_ns;
        }
        std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
        return;
    }

    // Stage 2: capped moderate sleep (500 us).
    std::this_thread::sleep_for(std::chrono::nanoseconds(500'000));
}
```

Replace `try_poll_idle()`:

```cpp
bool WorkerThread::try_poll_idle() {
    // Standalone workers (no owner_) stay in polling indefinitely.
    if (!owner_) {
        diag_idle_iters_.fetch_add(1, std::memory_order_relaxed);
        increment_donations();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        return true;
    }

    auto now = std::chrono::steady_clock::now();

    // Record idle start on first idle iteration.
    if (idle_since_ == std::chrono::steady_clock::time_point{}) {
        idle_since_ = now;
    }

    auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - idle_since_)
            .count();

    if (static_cast<uint64_t>(elapsed_ns) < calibration_.polling_budget_ns) {
        diag_idle_iters_.fetch_add(1, std::memory_order_relaxed);
        increment_donations();
        backoff(std::chrono::nanoseconds(elapsed_ns));
        return true;
    }
    return false;
}
```

Replace `reset_backoff()`:

```cpp
void WorkerThread::reset_backoff() {
    idle_since_ = std::chrono::steady_clock::time_point{};
}
```

Update `process_work_item()`:

```cpp
void WorkerThread::process_work_item(const WorkItem& item) {
    diag_work_found_.fetch_add(1, std::memory_order_relaxed);
    // Reset idle tracking — worker is active.
    idle_since_ = std::chrono::steady_clock::time_point{};
    consecutive_empty_wakes_.store(0, std::memory_order_relaxed);
    in_cv_model_.store(false, std::memory_order_relaxed);
    if (processor_) {
        processor_(item);
    }
}
```

Update `diag_is_in_cv_model()`:

```cpp
bool WorkerThread::diag_is_in_cv_model() const {
    return in_cv_model_.load(std::memory_order_relaxed);
}
```

Set `in_cv_model_` in `enter_cv_block()` after the double-check passes:

Add this line after `diag_cv_escalations_.fetch_add(1, std::memory_order_relaxed);`:
```cpp
    in_cv_model_.store(true, std::memory_order_relaxed);
```

Update `thread_loop()` to NOT call `reset_backoff()` after CV wakeup with no work:

```cpp
void WorkerThread::thread_loop() {
    tl_current_worker_id = config_.worker_index;

    while (!stop_requested_.load(std::memory_order_acquire) &&
           running_.load(std::memory_order_acquire)) {
        if (pause_handler_) {
            pause_handler_();
        }

        // Phase 1: Try to find and process work.
        if (try_find_and_process_work()) {
            continue;
        }

        // Phase 2: Polling idle model.
        if (try_poll_idle()) {
            continue;
        }

        // Phase 3: CV blocking model.
        // enter_cv_block() returns true if work was found during the
        // pre-sleep double-check (already processed, idle state reset).
        // Returns false after CV wait completed without finding work.
        // In the false case, DON'T reset idle_since_ — let it keep
        // tracking from the original idle start so the next
        // try_poll_idle() immediately re-enters CV.
        if (enter_cv_block()) {
            continue;
        }
        // CV wait completed without finding work.
        // idle_since_ retains its pre-CV value -> immediate re-entry to CV.
    }
}
```

Update the `backoff()` declaration in the header from `void backoff();` to:

```cpp
    /// \brief Adaptive idle backoff: yield -> proportional sleep -> capped sleep.
    ///
    /// \param[in] elapsed Wall-clock time since the worker first became idle.
    void backoff(std::chrono::nanoseconds elapsed);
```

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*WorkerBackoff*"
```
Expected: All tests pass.

- [ ] **Step 5: Run existing scheduler tests**

```bash
ninja -C build && ctest -R "WorkerThread|HybridScheduler|PriorityScheduler" --output-on-failure
```
Expected: All pass.

- [ ] **Step 6: Commit**

```bash
git add src/sched/worker_thread.cpp include/hpactor/sched/worker_thread.hpp tests/unit/sched/test_worker_backoff.cpp
git commit -m "feat(sched): replace iteration-count backoff with time-based backoff(elapsed)

Replace the fixed-iteration backoff() with a wall-clock-based version
that takes elapsed idle time and selects the sleep strategy based on
OS calibration values. Update try_poll_idle() to track idle_since_
timestamp and escalate to CV after polling_budget_ns of idle time.
After a CV wakeup with no work, idle_since_ is preserved so the next
try_poll_idle() immediately re-enters CV without wasted backoff ramp.

Track in_cv_model_ flag for fast diag_is_in_cv_model() queries.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Exponential CV timeout

**Files:**
- Modify: `src/sched/worker_thread.cpp` — `enter_cv_block()` timeout computation
- Modify: `tests/unit/sched/test_worker_backoff.cpp` — add exponential timeout tests

- [ ] **Step 1: Write failing tests for exponential CV timeout**

Add to `tests/unit/sched/test_worker_backoff.cpp`:

```cpp
// ── Exponential CV timeout ─────────────────────────────────────────────

TEST(WorkerBackoffTest, ConsecutiveEmptyWakesIncrements) {
    BackoffCalibration cal;
    cal.yield_is_effective = false;
    cal.min_effective_sleep_ns = 1'000;
    cal.spin_threshold_ns = 0;
    cal.polling_budget_ns = 1'000'000;  // 1ms — reach CV very fast

    WorkerThread::set_test_calibration(&cal);

    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);
    worker.start();

    // Wait for CV escalation.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker.diag_is_in_cv_model() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(worker.diag_is_in_cv_model());
    EXPECT_GE(worker.diag_cv_escalations(), 1u);

    worker.stop();
    WorkerThread::set_test_calibration(nullptr);
}

TEST(WorkerBackoffTest, ConsecutiveEmptyWakesResetsWhenWorkFound) {
    BackoffCalibration cal;
    cal.yield_is_effective = false;
    cal.min_effective_sleep_ns = 1'000;
    cal.spin_threshold_ns = 0;
    cal.polling_budget_ns = 1'000'000;  // 1ms

    WorkerThread::set_test_calibration(&cal);

    WorkerThread::Config cfg;
    cfg.worker_index = 0;
    WorkerThread worker(cfg);

    std::atomic<bool> work_done{false};
    worker.set_work_processor([&](const WorkItem&) {
        work_done.store(true);
    });

    worker.start();

    // Let worker enter CV.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!worker.diag_is_in_cv_model() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(worker.diag_is_in_cv_model());

    // Push work.
    WorkItem item;
    item.actor = ActorId{0};
    worker.push(0, item);

    // Wait for processing.
    auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!work_done.load() &&
           std::chrono::steady_clock::now() < deadline2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(work_done.load());

    // After work is found, consecutive_empty_wakes should reset to 0.
    EXPECT_EQ(worker.diag_consecutive_empty_wakes(), 0u);

    worker.stop();
    WorkerThread::set_test_calibration(nullptr);
}
```

- [ ] **Step 2: Run tests to confirm behavior**

```bash
ninja -C build test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*ConsecutiveEmptyWakes*"
```
Expected: `ConsecutiveEmptyWakesResetsWhenWorkFound` should PASS (reset already implemented in Task 3). `ConsecutiveEmptyWakesIncrements` verifies basic CV escalation.

- [ ] **Step 3: Implement exponential CV timeout**

In `enter_cv_block()`, replace the default timeout with exponential computation (for the no-EDF-deadlines case):

```cpp
    diag_cv_escalations_.fetch_add(1, std::memory_order_relaxed);
    in_cv_model_.store(true, std::memory_order_relaxed);

    // Compute EDF-aware CV timeout.
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::milliseconds(100);
    int64_t edf_ns = owner_->edf_next_deadline();
    if (edf_ns != INT64_MAX) {
        int64_t now_ns = now.time_since_epoch().count();
        int64_t delta_ns = edf_ns - now_ns;
        if (delta_ns <= 0)
            delta_ns = 1'000'000;
        auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::nanoseconds(delta_ns));
        auto margin = std::chrono::milliseconds(1);
        timeout = (delta_ms > margin) ? (delta_ms - margin)
                                      : std::chrono::milliseconds(1);
        if (timeout > std::chrono::milliseconds(100))
            timeout = std::chrono::milliseconds(100);
    } else {
        // No EDF deadlines: exponential safety-net timeout.
        constexpr uint32_t kBaseTimeoutMs = 100;
        constexpr uint32_t kMaxTimeoutMs = 30'000;
        uint32_t c =
            consecutive_empty_wakes_.load(std::memory_order_relaxed);
        uint32_t shift = std::min(c, 9u);
        uint32_t ms = kBaseTimeoutMs << shift;
        timeout = std::chrono::milliseconds(std::min(ms, kMaxTimeoutMs));
    }
```

Increment `consecutive_empty_wakes_` on timeout wake (update the `timed_out` branch):

```cpp
    if (timed_out) {
        diag_cv_timeout_wakes_.fetch_add(1, std::memory_order_relaxed);
        consecutive_empty_wakes_.fetch_add(1, std::memory_order_relaxed);
    } else {
        diag_cv_notify_wakes_.fetch_add(1, std::memory_order_relaxed);
    }
```

- [ ] **Step 4: Build and run tests**

```bash
ninja -C build test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*WorkerBackoff*"
```
Expected: All tests pass.

- [ ] **Step 5: Run existing tests**

```bash
ninja -C build && ctest -R "WorkerThread|HybridScheduler|PriorityScheduler" --output-on-failure
```
Expected: All pass.

- [ ] **Step 6: Commit**

```bash
git add src/sched/worker_thread.cpp tests/unit/sched/test_worker_backoff.cpp
git commit -m "feat(sched): exponential CV timeout replacing fixed 100ms default

When no EDF deadlines exist, the CV timeout starts at 100ms and doubles
each consecutive empty timeout wake (100->200->400->...->30s cap). The
consecutive_empty_wakes_ counter resets to 0 when work is found. This
reduces idle CPU from ~0.6% to near-zero after ~1 minute of inactivity.

Work arrival via wake_if_blocking() still wakes the worker immediately
regardless of timeout — no latency penalty.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Cleanup — remove old constants and backoff_counter_, update scheduler snapshot

**Files:**
- Modify: `src/sched/worker_thread.cpp` — remove old constants
- Modify: `include/hpactor/sched/worker_thread.hpp` — remove `backoff_counter_`
- Modify: `src/sched/scheduler.cpp` — populate new `WorkerSnapshot` fields

- [ ] **Step 1: Remove old file-scope constants**

In `src/sched/worker_thread.cpp`, remove the `#if defined` block containing `kBackoffYieldIters`, `kBackoffSleepIters`, and `kPollThreshold`.

- [ ] **Step 2: Remove backoff_counter_ member from header**

In `include/hpactor/sched/worker_thread.hpp`, remove the `backoff_counter_` member declaration and its `diag_backoff_counter()` accessor.

- [ ] **Step 3: Update scheduler.cpp to populate new WorkerSnapshot fields**

In `src/sched/scheduler.cpp`, in `worker_snapshots()`, add after `ws.cv_timeout_wakes`:

```cpp
        // Adaptive backoff calibration diagnostics.
        ws.calibration_yield_effective =
            worker_threads_[i]->diag_yield_is_effective();
        ws.calibration_min_sleep_ns =
            worker_threads_[i]->diag_min_sleep_ns();
        ws.consecutive_empty_wakes =
            worker_threads_[i]->diag_consecutive_empty_wakes();
```

- [ ] **Step 4: Build and verify compilation**

```bash
ninja -C build
```
Expected: Clean build, no errors.

- [ ] **Step 5: Run targeted tests**

```bash
ctest -R "WorkerThread|WorkerBackoff|HybridScheduler|PriorityScheduler|SchedulerControl" --output-on-failure
```
Expected: All tests pass.

- [ ] **Step 6: Run full test suite**

```bash
ctest --output-on-failure --parallel 8
```
Expected: All tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/sched/worker_thread.cpp include/hpactor/sched/worker_thread.hpp src/sched/scheduler.cpp
git commit -m "refactor(sched): remove old backoff constants and backoff_counter_

Remove kBackoffYieldIters, kBackoffSleepIters, kPollThreshold platform-
specific constants and the backoff_counter_ atomic member. These are
fully replaced by BackoffCalibration, idle_since_ time-point tracking,
and the in_cv_model_ flag.

Populate new calibration diagnostics in WorkerSnapshot.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Post-Implementation Verification

After all tasks complete:

- [ ] **Verify idle CPU on Linux:** Run a benchmark app with light workload, observe `diag_consecutive_empty_wakes` grow and CPU drop.
- [ ] **Verify macOS still works:** Run same benchmark, confirm no regression.
- [ ] **Check CLI introspection:** `inspect workers` shows calibration and consecutive_empty_wakes fields.
