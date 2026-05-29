# Scheduler Decoupling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decouple HPActor's M:N worker placement scheduler from behavior and coroutine actor execution while preserving the public `IScheduler` / `HybridScheduler` API.

**Architecture:** Keep `HybridScheduler` as the public facade, then move readiness admission, worker placement, and actor execution into focused internal components. Initial tasks add failing regression tests, then introduce an admitted-work path so behavior requeue and coroutine yield no longer bypass or fight placement policy. Later tasks extract `ActorReadyGate`, `WorkPlacementScheduler`, `ActorExecutionEngine`, `BehaviorActorRunner`, and `CoroutineActorRunner`.

**Tech Stack:** C++20, CMake/Ninja, GoogleTest, HPActor scheduler/mailbox/actor runtime, no exceptions, no RTTI.

---

## Source Design Inputs

- Spec: `docs/superpowers/specs/2026-05-29-scheduler-decoupling-design.md`
- Main implementation under refactor: `src/sched/scheduler.cpp`
- Public scheduler interface: `include/hpactor/sched/scheduler.hpp`
- Coroutine runtime headers: `include/hpactor/sched/coroutine_task.hpp`, `include/hpactor/sched/coroutine_awaiters.hpp`, `include/hpactor/sched/yield_awaiter.hpp`
- Actor runtime: `include/hpactor/actor/event_based_actor.hpp`, `src/actor/event_based_actor.cpp`
- Test helpers: `tests/support/scheduler_test_driver.hpp`

## File Structure

Create:

- `include/hpactor/sched/actor_ready_gate.hpp` - readiness admission state machine for event-based actors.
- `src/sched/actor_ready_gate.cpp` - ActorSystem lookup and `ActorState` CAS implementation.
- `include/hpactor/sched/work_placement_scheduler.hpp` - worker queue, EDF, pinning, A2WS, and dedicated-pool placement interface.
- `src/sched/work_placement_scheduler.cpp` - placement implementation extracted from `HybridScheduler`.
- `include/hpactor/sched/actor_execution_engine.hpp` - actor activation result types, behavior runner, coroutine runner declarations.
- `src/sched/actor_execution_engine.cpp` - behavior/coroutine actor activation logic extracted from `HybridScheduler::execute_actor()`.
- `include/hpactor/sched/scheduler_interfaces.hpp` - narrow timer, readiness, and yield interfaces for awaiters.
- `tests/unit/sched/test_actor_ready_gate.cpp` - readiness admission tests.
- `tests/unit/sched/test_work_placement_scheduler.cpp` - placement-only tests.

Modify:

- `src/sched/scheduler.cpp` - convert `HybridScheduler` into a facade delegating to the new components.
- `include/hpactor/sched/scheduler.hpp` - add component members and remove placement/execution details from the facade over time.
- `src/CMakeLists.txt` - compile new scheduler implementation files.
- `tests/unit/sched/CMakeLists.txt` - add new unit test sources.
- `tests/unit/sched/test_scheduler_control.cpp` - add regression tests before refactor.
- `tests/integration/sched/test_coroutine_scheduling.cpp` - add coroutine wakeup/resume regression test.
- `include/hpactor/sched/coroutine_awaiters.hpp` - narrow timer dependency.
- `include/hpactor/sched/yield_awaiter.hpp` - narrow yield dependency and preserve readiness-based scheduling.

## Targeted Verification Commands

Use targeted checks first. Do not rebuild the full project unless a CMake source-list change requires regenerating build files or a later broad failure requires it.

If `build/` does not exist:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

For scheduler unit work:

```bash
cmake --build build --target test_unit_sched
./build/tests/unit/sched/test_unit_sched --gtest_filter="SchedulerControlTest.*:ActorReadyGateTest.*:WorkPlacementSchedulerTest.*"
```

For coroutine integration work:

```bash
cmake --build build --target test_integration_sched
./build/tests/integration/sched/test_integration_sched --gtest_filter="CoroutineSchedulingTest.*"
```

For final targeted verification:

```bash
cmake --build build --target test_unit_sched test_integration_sched
./build/tests/unit/sched/test_unit_sched
./build/tests/integration/sched/test_integration_sched
git diff --check
```

---

### Task 1: Add Scheduler Regression Tests

**Files:**
- Modify: `tests/unit/sched/test_scheduler_control.cpp`

- [ ] **Step 1: Add regression tests for pinned requeue and yield**

Append these tests to `tests/unit/sched/test_scheduler_control.cpp`:

```cpp
TEST_F(SchedulerControlTest, PinnedActorRequeueStaysOnPinnedWorker) {
    cfg.scheduler_threads = 2;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* sched = system.scheduler();
    auto* ca = static_cast<CountingActor*>(actor.get().get());

    ASSERT_NE(sched, nullptr);
    sched->pin_actor_to_worker(actor.id(), 1);

    for (int i = 0; i < 3; ++i) {
        system.deliver_local(actor.id(),
                             TypedMessage(TypeTag::User, StreamBuffer{1}));
    }

    EXPECT_EQ(ca->received(), 0);
    EXPECT_TRUE(sched->run_actor(actor.id()));
    EXPECT_EQ(ca->received(), 1);
    EXPECT_TRUE(sched->run_actor(actor.id()));
    EXPECT_EQ(ca->received(), 2);
    EXPECT_TRUE(sched->run_actor(actor.id()));
    EXPECT_EQ(ca->received(), 3);
    EXPECT_FALSE(sched->run_actor(actor.id()));
}

TEST_F(SchedulerControlTest, YieldFromRunningActorRequeuesAdmittedWork) {
    cfg.scheduler_threads = 1;
    ActorSystem system(cfg);

    auto actor = system.spawn<CountingActor>();
    auto* sched = system.scheduler();
    auto* ca = static_cast<CountingActor*>(actor.get().get());
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());

    ASSERT_NE(sched, nullptr);

    // Drain the spawn-time readiness item, which has no mailbox work.
    static_cast<void>(sched->run_one_ready());
    EXPECT_TRUE(eba->actor_state().is_idle());

    eba->actor_state().set(ActorState::kRunning);
    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    EXPECT_EQ(ca->received(), 0);

    sched->yield(actor.id(), 0);

    EXPECT_TRUE(eba->actor_state().is_ready());
    EXPECT_TRUE(sched->run_one_ready());
    EXPECT_EQ(ca->received(), 1);
}
```

- [ ] **Step 2: Run the new tests and observe the current failure**

Run:

```bash
cmake --build build --target test_unit_sched
./build/tests/unit/sched/test_unit_sched --gtest_filter="SchedulerControlTest.PinnedActorRequeueStaysOnPinnedWorker:SchedulerControlTest.YieldFromRunningActorRequeuesAdmittedWork"
```

Expected before implementation:

```text
FAILED  SchedulerControlTest.PinnedActorRequeueStaysOnPinnedWorker
FAILED  SchedulerControlTest.YieldFromRunningActorRequeuesAdmittedWork
```

- [ ] **Step 3: Commit the failing regression tests**

```bash
git add tests/unit/sched/test_scheduler_control.cpp
git commit -m "test: capture scheduler requeue and yield regressions"
```

---

### Task 2: Add an Internal Admitted-Work Path in `HybridScheduler`

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `src/sched/scheduler.cpp`

- [ ] **Step 1: Add private helpers to `HybridScheduler`**

In `include/hpactor/sched/scheduler.hpp`, add these private methods near the existing private worker helpers:

```cpp
    bool try_admit_ready(ActorId actor) noexcept;
    bool try_mark_yield_ready(ActorId actor) noexcept;
    void enqueue_admitted(const WorkItem& item, uint8_t priority);
```

- [ ] **Step 2: Implement readiness admission helper**

In `src/sched/scheduler.cpp`, add this helper before `HybridScheduler::notify_ready()`:

```cpp
bool HybridScheduler::try_admit_ready(ActorId actor) noexcept {
    auto actor_ptr = system_.get_actor(actor);
    if (!actor_ptr || !actor_ptr->is_event_based_actor()) {
        return actor_ptr != nullptr;
    }

    auto* eb = static_cast<EventBasedActor*>(actor_ptr.get());
    auto& state = eb->actor_state();

    for (;;) {
        uint32_t current = state.get();
        if (current == ActorState::kReady ||
            current == ActorState::kRunning ||
            current == ActorState::kTerminated) {
            return false;
        }

        if (current == ActorState::kIdle ||
            current == ActorState::kIOWaiting) {
            uint32_t expected = current;
            if (state.cas(expected, ActorState::kReady)) {
                return true;
            }
            continue;
        }

        return false;
    }
}
```

- [ ] **Step 3: Implement yield readiness helper**

In `src/sched/scheduler.cpp`, add this helper after `try_admit_ready()`:

```cpp
bool HybridScheduler::try_mark_yield_ready(ActorId actor) noexcept {
    auto actor_ptr = system_.get_actor(actor);
    if (!actor_ptr || !actor_ptr->is_event_based_actor()) {
        return false;
    }

    auto* eb = static_cast<EventBasedActor*>(actor_ptr.get());
    auto& state = eb->actor_state();
    uint32_t expected = ActorState::kRunning;
    return state.cas(expected, ActorState::kReady);
}
```

- [ ] **Step 4: Implement placement-only admitted enqueue**

In `src/sched/scheduler.cpp`, add this helper after `try_mark_yield_ready()`:

```cpp
void HybridScheduler::enqueue_admitted(const WorkItem& item, uint8_t priority) {
    if (num_workers_ == 0) {
        return;
    }

    uint32_t victim = 0;
    bool is_pinned = false;
    {
        std::lock_guard<std::mutex> lock(pinned_mutex_);
        auto it = pinned_actors_.find(item.actor);
        if (it != pinned_actors_.end()) {
            victim = it->second % num_workers_;
            is_pinned = true;
        }
    }

    if (!is_pinned) {
        static std::atomic<uint32_t> rr_counter{0};
        victim = rr_counter.fetch_add(1, std::memory_order_relaxed) %
                 num_workers_;
    }

    if (is_pinned && workers_paused_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(pinned_mutex_);
        pinned_ready_[victim].push_back(item);
        return;
    }

    if (item.deadline_ns == INT64_MAX) {
        workers_[victim].queues[priority].push_bottom(item);
    } else {
        workers_[victim].edf_queue.push(item.deadline_ns, item);
    }
}
```

- [ ] **Step 5: Replace public readiness routing with the helpers**

In `HybridScheduler::notify_ready()`, keep the existing dedicated-thread and dedicated-pool checks, then replace the actor state gate and worker selection block with:

```cpp
    if (!try_admit_ready(actor)) {
        return;
    }

    enqueue_admitted(item, priority);
```

The method should still return early for stopped schedulers, dedicated-thread actors, and dedicated-pool actors exactly as it does before this task.

- [ ] **Step 6: Make public yield use admitted placement**

Replace `HybridScheduler::yield()` with:

```cpp
void HybridScheduler::yield(ActorId actor, uint8_t priority) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    if (!try_mark_yield_ready(actor)) {
        return;
    }
    enqueue_admitted(WorkItem{actor, INT64_MAX, 0}, priority);
}
```

- [ ] **Step 7: Replace behavior requeue direct worker pushes**

In the behavior path inside `HybridScheduler::execute_actor()`, replace both
direct worker-queue push blocks with:

```cpp
        enqueue_admitted(item, 0);
```

Keep the existing state transitions around those calls.

- [ ] **Step 8: Run targeted regression tests**

Run:

```bash
cmake --build build --target test_unit_sched
./build/tests/unit/sched/test_unit_sched --gtest_filter="SchedulerControlTest.PinnedActorRequeueStaysOnPinnedWorker:SchedulerControlTest.YieldFromRunningActorRequeuesAdmittedWork"
```

Expected after this task:

```text
[  PASSED  ] 2 tests.
```

- [ ] **Step 9: Commit the admitted-work path**

```bash
git add include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp
git commit -m "refactor(sched): route admitted scheduler work through one path"
```

---

### Task 3: Extract `ActorReadyGate`

**Files:**
- Create: `include/hpactor/sched/actor_ready_gate.hpp`
- Create: `src/sched/actor_ready_gate.cpp`
- Create: `tests/unit/sched/test_actor_ready_gate.cpp`
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `src/sched/scheduler.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/sched/CMakeLists.txt`

- [ ] **Step 1: Create `actor_ready_gate.hpp`**

Create `include/hpactor/sched/actor_ready_gate.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#pragma once

#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>

namespace hpactor {
class ActorSystem;
class EventBasedActor;
} // namespace hpactor

namespace hpactor::sched {

enum class ReadyAdmissionCode : uint8_t {
    Accepted,
    MissingActor,
    AlreadyReady,
    AlreadyRunning,
    Terminated,
    NotAdmissible,
};

struct ReadyAdmission {
    ReadyAdmissionCode code{ReadyAdmissionCode::NotAdmissible};

    bool accepted() const noexcept {
        return code == ReadyAdmissionCode::Accepted;
    }
};

class ActorReadyGate {
  public:
    explicit ActorReadyGate(ActorSystem& system) noexcept;

    ReadyAdmission try_mark_ready(ActorId actor) noexcept;
    ReadyAdmission mark_ready_already_admitted(EventBasedActor& actor) noexcept;

  private:
    ActorSystem& system_;
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Create `actor_ready_gate.cpp`**

Create `src/sched/actor_ready_gate.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#include <hpactor/sched/actor_ready_gate.hpp>

#include <hpactor/actor/actor_state.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>

namespace hpactor::sched {

ActorReadyGate::ActorReadyGate(ActorSystem& system) noexcept
    : system_(system) {}

ReadyAdmission ActorReadyGate::try_mark_ready(ActorId actor) noexcept {
    auto actor_ptr = system_.get_actor(actor);
    if (!actor_ptr) {
        return {ReadyAdmissionCode::MissingActor};
    }
    if (!actor_ptr->is_event_based_actor()) {
        return {ReadyAdmissionCode::Accepted};
    }

    auto* eb = static_cast<EventBasedActor*>(actor_ptr.get());
    auto& state = eb->actor_state();

    for (;;) {
        uint32_t current = state.get();
        switch (current) {
            case ActorState::kIdle:
            case ActorState::kIOWaiting: {
                uint32_t expected = current;
                if (state.cas(expected, ActorState::kReady)) {
                    return {ReadyAdmissionCode::Accepted};
                }
                continue;
            }
            case ActorState::kReady:
                return {ReadyAdmissionCode::AlreadyReady};
            case ActorState::kRunning:
                return {ReadyAdmissionCode::AlreadyRunning};
            case ActorState::kTerminated:
                return {ReadyAdmissionCode::Terminated};
            default:
                return {ReadyAdmissionCode::NotAdmissible};
        }
    }
}

ReadyAdmission
ActorReadyGate::mark_ready_already_admitted(EventBasedActor& actor) noexcept {
    auto& state = actor.actor_state();
    for (;;) {
        uint32_t current = state.get();
        switch (current) {
            case ActorState::kRunning:
            case ActorState::kIdle:
            case ActorState::kIOWaiting: {
                uint32_t expected = current;
                if (state.cas(expected, ActorState::kReady)) {
                    return {ReadyAdmissionCode::Accepted};
                }
                continue;
            }
            case ActorState::kReady:
                return {ReadyAdmissionCode::AlreadyReady};
            case ActorState::kTerminated:
                return {ReadyAdmissionCode::Terminated};
            default:
                return {ReadyAdmissionCode::NotAdmissible};
        }
    }
}

} // namespace hpactor::sched
```

- [ ] **Step 3: Add ready gate tests**

Create `tests/unit/sched/test_actor_ready_gate.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#include <gtest/gtest.h>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/sched/actor_ready_gate.hpp>

using namespace hpactor;

namespace {

class ReadyGateActor : public EventBasedActor {
  public:
    ReadyGateActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}
};

ActorSystem make_system() {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    return ActorSystem(cfg);
}

} // namespace

TEST(ActorReadyGateTest, IdleActorIsAdmittedAsReady) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kIdle);

    sched::ActorReadyGate gate(system);
    auto result = gate.try_mark_ready(actor.id());

    EXPECT_TRUE(result.accepted());
    EXPECT_TRUE(eba->actor_state().is_ready());
}

TEST(ActorReadyGateTest, IOWaitingActorIsAdmittedAsReady) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kIOWaiting);

    sched::ActorReadyGate gate(system);
    auto result = gate.try_mark_ready(actor.id());

    EXPECT_TRUE(result.accepted());
    EXPECT_TRUE(eba->actor_state().is_ready());
}

TEST(ActorReadyGateTest, DuplicateReadyActorIsRejected) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kReady);

    sched::ActorReadyGate gate(system);
    auto result = gate.try_mark_ready(actor.id());

    EXPECT_EQ(result.code, sched::ReadyAdmissionCode::AlreadyReady);
    EXPECT_TRUE(eba->actor_state().is_ready());
}

TEST(ActorReadyGateTest, RunningActorIsRejectedForPublicReadiness) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kRunning);

    sched::ActorReadyGate gate(system);
    auto result = gate.try_mark_ready(actor.id());

    EXPECT_EQ(result.code, sched::ReadyAdmissionCode::AlreadyRunning);
    EXPECT_TRUE(eba->actor_state().is_running());
}

TEST(ActorReadyGateTest, RunningActorCanBeMarkedReadyWhenAlreadyAdmitted) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kRunning);

    sched::ActorReadyGate gate(system);
    auto result = gate.mark_ready_already_admitted(*eba);

    EXPECT_TRUE(result.accepted());
    EXPECT_TRUE(eba->actor_state().is_ready());
}

TEST(ActorReadyGateTest, TerminatedActorIsRejected) {
    auto system = make_system();
    auto actor = system.spawn<ReadyGateActor>();
    auto* eba = static_cast<EventBasedActor*>(actor.get().get());
    eba->actor_state().set(ActorState::kTerminated);

    sched::ActorReadyGate gate(system);
    auto result = gate.try_mark_ready(actor.id());

    EXPECT_EQ(result.code, sched::ReadyAdmissionCode::Terminated);
    EXPECT_TRUE(eba->actor_state().is_terminated());
}
```

- [ ] **Step 4: Add files to CMake**

In `src/CMakeLists.txt`, add `sched/actor_ready_gate.cpp` immediately before `sched/scheduler.cpp`:

```cmake
    sched/actor_ready_gate.cpp
    sched/scheduler.cpp
```

In `tests/unit/sched/CMakeLists.txt`, add `test_actor_ready_gate.cpp` before `test_scheduler_control.cpp`:

```cmake
    test_actor_ready_gate.cpp
    test_scheduler_control.cpp
```

- [ ] **Step 5: Replace local readiness helper with `ActorReadyGate`**

In `include/hpactor/sched/scheduler.hpp`, include the new header:

```cpp
#include <hpactor/sched/actor_ready_gate.hpp>
```

Add this member after `ActorSystem& system_;`:

```cpp
    ActorReadyGate ready_gate_;
```

In the `HybridScheduler` constructor initializer list in `src/sched/scheduler.cpp`, add `ready_gate_(system)` after `system_(system)`:

```cpp
    : system_(system), ready_gate_(system), num_workers_(num_workers),
```

Replace the body of `HybridScheduler::try_admit_ready()`:

```cpp
bool HybridScheduler::try_admit_ready(ActorId actor) noexcept {
    return ready_gate_.try_mark_ready(actor).accepted();
}
```

Replace the body of `HybridScheduler::try_mark_yield_ready()`:

```cpp
bool HybridScheduler::try_mark_yield_ready(ActorId actor) noexcept {
    auto actor_ptr = system_.get_actor(actor);
    if (!actor_ptr || !actor_ptr->is_event_based_actor()) {
        return false;
    }
    auto* eb = static_cast<EventBasedActor*>(actor_ptr.get());
    return ready_gate_.mark_ready_already_admitted(*eb).accepted();
}
```

- [ ] **Step 6: Run targeted tests**

Run:

```bash
cmake --build build --target test_unit_sched
./build/tests/unit/sched/test_unit_sched --gtest_filter="ActorReadyGateTest.*:SchedulerControlTest.PinnedActorRequeueStaysOnPinnedWorker:SchedulerControlTest.YieldFromRunningActorRequeuesAdmittedWork"
```

Expected:

```text
[  PASSED  ] 8 tests.
```

- [ ] **Step 7: Commit `ActorReadyGate`**

```bash
git add include/hpactor/sched/actor_ready_gate.hpp src/sched/actor_ready_gate.cpp tests/unit/sched/test_actor_ready_gate.cpp include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp src/CMakeLists.txt tests/unit/sched/CMakeLists.txt
git commit -m "refactor(sched): extract actor readiness gate"
```

---

### Task 4: Extract `WorkPlacementScheduler`

**Files:**
- Create: `include/hpactor/sched/work_placement_scheduler.hpp`
- Create: `src/sched/work_placement_scheduler.cpp`
- Create: `tests/unit/sched/test_work_placement_scheduler.cpp`
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `src/sched/scheduler.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/sched/CMakeLists.txt`

- [ ] **Step 1: Create placement header**

Create `include/hpactor/sched/work_placement_scheduler.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#pragma once

#include <hpactor/log/logger.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/a2ws.hpp>
#include <hpactor/sched/edf_queue.hpp>
#include <hpactor/sched/work_queue.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hpactor::sched {

class DedicatedThreadPool;

enum class PlacementResult : uint8_t {
    EnqueuedShared,
    EnqueuedDedicatedPool,
    SuppressedDedicatedThread,
    NoWorkers,
};

using DedicatedDispatch = std::function<void(const WorkItem&)>;

class WorkPlacementScheduler {
  public:
    WorkPlacementScheduler(uint32_t num_workers, uint32_t num_priorities);
    ~WorkPlacementScheduler();

    WorkPlacementScheduler(const WorkPlacementScheduler&) = delete;
    WorkPlacementScheduler& operator=(const WorkPlacementScheduler&) = delete;

    void set_metrics_ring_buffer(
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics) noexcept;
    void set_logger(log::Logger* logger) noexcept;

    PlacementResult enqueue_admitted(const WorkItem& item, uint8_t priority,
                                     bool workers_paused,
                                     const DedicatedDispatch& dedicated_dispatch);

    bool pop_local(uint32_t worker_id, WorkItem& out);
    bool pop_edf(uint32_t worker_id, WorkItem& out);
    bool try_steal(uint32_t thief_worker_id, WorkItem& out);
    bool pop_any_for_test(WorkItem& out);
    bool pop_one_on_worker_for_test(uint32_t worker_id, WorkItem& out);
    bool take_pinned_for_test(ActorId actor, WorkItem& out, uint32_t& worker_id);

    void pin_actor_to_worker(ActorId actor, uint32_t worker_id);
    void unpin_actor(ActorId actor);
    void flush_pinned_to_shared();

    void register_dedicated_thread(ActorId actor, int cpu_affinity);
    void register_dedicated_pool(ActorId actor, uint32_t pool_size);
    void unregister_dedicated(ActorId actor);

    uint32_t worker_count() const noexcept {
        return num_workers_;
    }

    A2WS& a2ws() noexcept {
        return a2ws_;
    }

  private:
    struct alignas(64) WorkerState {
        std::unique_ptr<ChaselevDeque<WorkItem>[]> queues;
        uint32_t index{0};
        EDFQueue edf_queue;
    };

    struct DedicatedStorage;

    uint32_t choose_worker(ActorId actor, bool& is_pinned);
    void enqueue_shared(const WorkItem& item, uint8_t priority,
                        uint32_t worker_id);
    void emit_steal_metric(const WorkItem& item, uint32_t from_worker);

    uint32_t num_workers_;
    uint32_t num_priorities_;
    std::vector<WorkerState> workers_;
    A2WS a2ws_;
    std::unique_ptr<DedicatedStorage> dedicated_;

    mutable std::mutex pinned_mutex_;
    std::unordered_map<ActorId, uint32_t> pinned_actors_;
    std::vector<std::deque<WorkItem>> pinned_ready_;

    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_{nullptr};
    log::Logger* logger_{nullptr};
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Create placement implementation by moving existing logic**

Create `src/sched/work_placement_scheduler.cpp` with this structure:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#include <hpactor/sched/work_placement_scheduler.hpp>

#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/sched/dedicated_thread_pool.hpp>

#include <climits>

namespace hpactor::sched {

extern thread_local uint32_t tl_current_worker_id;

struct WorkPlacementScheduler::DedicatedStorage {
    std::unordered_set<ActorId> dedicated_thread_actors_;
    std::unordered_map<ActorId, int> dedicated_thread_affinity_;
    std::mutex dedicated_mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<DedicatedThreadPool>>
        dedicated_pools_;
    std::unordered_map<ActorId, uint32_t> actor_pool_map_;
};

WorkPlacementScheduler::WorkPlacementScheduler(uint32_t num_workers,
                                               uint32_t num_priorities)
    : num_workers_(num_workers), num_priorities_(num_priorities),
      workers_(num_workers), a2ws_(num_workers),
      dedicated_(std::make_unique<DedicatedStorage>()),
      pinned_ready_(num_workers) {
    for (uint32_t i = 0; i < num_workers_; ++i) {
        workers_[i].queues =
            std::make_unique<ChaselevDeque<WorkItem>[]>(num_priorities_);
        workers_[i].index = i;
    }
}

WorkPlacementScheduler::~WorkPlacementScheduler() = default;

void WorkPlacementScheduler::set_metrics_ring_buffer(
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics) noexcept {
    metrics_ring_buffer_ = metrics;
}

void WorkPlacementScheduler::set_logger(log::Logger* logger) noexcept {
    logger_ = logger;
}

uint32_t WorkPlacementScheduler::choose_worker(ActorId actor, bool& is_pinned) {
    is_pinned = false;
    uint32_t victim = 0;
    {
        std::lock_guard<std::mutex> lock(pinned_mutex_);
        auto it = pinned_actors_.find(actor);
        if (it != pinned_actors_.end()) {
            victim = it->second % num_workers_;
            is_pinned = true;
            return victim;
        }
    }

    static std::atomic<uint32_t> rr_counter{0};
    return rr_counter.fetch_add(1, std::memory_order_relaxed) % num_workers_;
}

void WorkPlacementScheduler::enqueue_shared(const WorkItem& item,
                                            uint8_t priority,
                                            uint32_t worker_id) {
    if (item.deadline_ns == INT64_MAX) {
        workers_[worker_id].queues[priority].push_bottom(item);
    } else {
        workers_[worker_id].edf_queue.push(item.deadline_ns, item);
    }
}

PlacementResult WorkPlacementScheduler::enqueue_admitted(
    const WorkItem& item, uint8_t priority, bool workers_paused,
    const DedicatedDispatch& dedicated_dispatch) {
    {
        std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
        if (dedicated_->dedicated_thread_actors_.find(item.actor) !=
            dedicated_->dedicated_thread_actors_.end()) {
            return PlacementResult::SuppressedDedicatedThread;
        }

        auto actor_pool = dedicated_->actor_pool_map_.find(item.actor);
        if (actor_pool != dedicated_->actor_pool_map_.end()) {
            auto pool = dedicated_->dedicated_pools_.find(actor_pool->second);
            if (pool != dedicated_->dedicated_pools_.end()) {
                auto* dedicated_pool = pool->second.get();
                dedicated_pool->enqueue(item.actor,
                                        [dedicated_dispatch, item]() {
                                            dedicated_dispatch(item);
                                        });
                return PlacementResult::EnqueuedDedicatedPool;
            }
        }
    }

    if (num_workers_ == 0) {
        return PlacementResult::NoWorkers;
    }

    bool is_pinned = false;
    uint32_t worker_id = choose_worker(item.actor, is_pinned);
    if (is_pinned && workers_paused) {
        std::lock_guard<std::mutex> lock(pinned_mutex_);
        pinned_ready_[worker_id].push_back(item);
        return PlacementResult::EnqueuedShared;
    }

    enqueue_shared(item, priority, worker_id);
    return PlacementResult::EnqueuedShared;
}

bool WorkPlacementScheduler::pop_edf(uint32_t worker_id, WorkItem& out) {
    auto& worker = workers_[worker_id];
    if (worker.edf_queue.empty()) {
        return false;
    }
    int64_t deadline = 0;
    if (!worker.edf_queue.peek(deadline)) {
        return false;
    }
    return worker.edf_queue.pop(out);
}

bool WorkPlacementScheduler::pop_local(uint32_t worker_id, WorkItem& out) {
    if (pop_edf(worker_id, out)) {
        return true;
    }
    auto& worker = workers_[worker_id];
    for (uint32_t p = 0; p < num_priorities_; ++p) {
        if (worker.queues[p].pop_bottom(out)) {
            return true;
        }
    }
    return false;
}

void WorkPlacementScheduler::emit_steal_metric(const WorkItem& item,
                                               uint32_t from_worker) {
    if (metrics_ring_buffer_) {
        metrics::MetricEvent evt{};
        evt.actor_id = item.actor;
        evt.event_type = metrics::MetricEventType::kSchedulerSteal;
        evt.value_hi = from_worker;
        metrics_ring_buffer_->try_push(evt);
    }
}

bool WorkPlacementScheduler::try_steal(uint32_t thief_worker_id,
                                       WorkItem& out) {
    for (uint32_t attempt = 0; attempt < num_workers_; ++attempt) {
        uint32_t victim_idx = a2ws_.get_victim(attempt % num_workers_);
        auto& victim = workers_[victim_idx];

        if (victim.edf_queue.pop(out)) {
            a2ws_.record_steal(attempt % num_workers_, victim_idx);
            emit_steal_metric(out, victim_idx);
            HPACTOR_LOG_DEBUG(
                log::LogCategory::kScheduler, out.actor,
                static_cast<uint32_t>(log::LogEventId::kSchedulerSteal),
                "work stolen",
                log::field("from_worker", static_cast<uint64_t>(victim_idx)),
                log::field("to_worker", static_cast<uint64_t>(thief_worker_id)));
            return true;
        }

        for (uint32_t p = 0; p < num_priorities_; ++p) {
            if (victim.queues[p].steal_top(out)) {
                a2ws_.record_steal(attempt % num_workers_, victim_idx);
                emit_steal_metric(out, victim_idx);
                HPACTOR_LOG_DEBUG(
                    log::LogCategory::kScheduler, out.actor,
                    static_cast<uint32_t>(log::LogEventId::kSchedulerSteal),
                    "work stolen",
                    log::field("from_worker", static_cast<uint64_t>(victim_idx)),
                    log::field("to_worker",
                               static_cast<uint64_t>(thief_worker_id)));
                return true;
            }
        }

        a2ws_.record_attempt(attempt % num_workers_, victim_idx, false);
    }
    return false;
}

bool WorkPlacementScheduler::pop_any_for_test(WorkItem& out) {
    for (uint32_t w = 0; w < num_workers_; ++w) {
        if (pop_edf(w, out)) {
            return true;
        }
        for (uint32_t p = 0; p < num_priorities_; ++p) {
            if (workers_[w].queues[p].steal_top(out)) {
                return true;
            }
        }
    }
    return false;
}

bool WorkPlacementScheduler::pop_one_on_worker_for_test(uint32_t worker_id,
                                                        WorkItem& out) {
    if (worker_id >= num_workers_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(pinned_mutex_);
        if (!pinned_ready_[worker_id].empty()) {
            out = pinned_ready_[worker_id].front();
            pinned_ready_[worker_id].pop_front();
            return true;
        }
    }
    if (pop_edf(worker_id, out)) {
        return true;
    }
    for (uint32_t p = 0; p < num_priorities_; ++p) {
        if (workers_[worker_id].queues[p].steal_top(out)) {
            return true;
        }
    }
    return false;
}

bool WorkPlacementScheduler::take_pinned_for_test(ActorId actor, WorkItem& out,
                                                  uint32_t& worker_id) {
    std::lock_guard<std::mutex> lock(pinned_mutex_);
    auto it = pinned_actors_.find(actor);
    if (it == pinned_actors_.end() || num_workers_ == 0) {
        return false;
    }
    worker_id = it->second % num_workers_;
    auto& queue = pinned_ready_[worker_id];
    for (auto iter = queue.begin(); iter != queue.end(); ++iter) {
        if (iter->actor == actor) {
            out = *iter;
            queue.erase(iter);
            return true;
        }
    }
    return false;
}

void WorkPlacementScheduler::pin_actor_to_worker(ActorId actor,
                                                 uint32_t worker_id) {
    std::lock_guard<std::mutex> lock(pinned_mutex_);
    pinned_actors_[actor] = worker_id;
}

void WorkPlacementScheduler::unpin_actor(ActorId actor) {
    std::lock_guard<std::mutex> lock(pinned_mutex_);
    pinned_actors_.erase(actor);
}

void WorkPlacementScheduler::flush_pinned_to_shared() {
    std::lock_guard<std::mutex> lock(pinned_mutex_);
    for (uint32_t w = 0; w < num_workers_; ++w) {
        for (auto& item : pinned_ready_[w]) {
            workers_[w].queues[0].push_bottom(item);
        }
        pinned_ready_[w].clear();
    }
}

void WorkPlacementScheduler::register_dedicated_thread(ActorId actor,
                                                       int cpu_affinity) {
    std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
    dedicated_->dedicated_thread_actors_.insert(actor);
    if (cpu_affinity >= 0) {
        dedicated_->dedicated_thread_affinity_[actor] = cpu_affinity;
    }
}

void WorkPlacementScheduler::register_dedicated_pool(ActorId actor,
                                                     uint32_t pool_size) {
    std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
    auto& pool = dedicated_->dedicated_pools_[pool_size];
    if (!pool) {
        pool = std::make_unique<DedicatedThreadPool>(pool_size);
        pool->start();
    }
    dedicated_->actor_pool_map_[actor] = pool_size;
}

void WorkPlacementScheduler::unregister_dedicated(ActorId actor) {
    std::lock_guard<std::mutex> lock(dedicated_->dedicated_mutex_);
    dedicated_->dedicated_thread_actors_.erase(actor);
    dedicated_->dedicated_thread_affinity_.erase(actor);
    dedicated_->actor_pool_map_.erase(actor);
}

} // namespace hpactor::sched
```

- [ ] **Step 3: Add placement-only tests**

Create `tests/unit/sched/test_work_placement_scheduler.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#include <gtest/gtest.h>

#include <hpactor/sched/work_placement_scheduler.hpp>

using namespace hpactor;

TEST(WorkPlacementSchedulerTest, EnqueueAdmittedCanBePoppedFromWorker) {
    sched::WorkPlacementScheduler placement(1, 4);
    sched::WorkItem item{ActorId{10}, INT64_MAX, 0};

    auto result = placement.enqueue_admitted(
        item, 0, false, [](const sched::WorkItem&) {});

    EXPECT_EQ(result, sched::PlacementResult::EnqueuedShared);

    sched::WorkItem out;
    EXPECT_TRUE(placement.pop_local(0, out));
    EXPECT_EQ(out.actor, ActorId{10});
}

TEST(WorkPlacementSchedulerTest, DeadlineWorkUsesEdfBeforePriorityQueue) {
    sched::WorkPlacementScheduler placement(1, 4);
    sched::WorkItem priority{ActorId{20}, INT64_MAX, 0};
    sched::WorkItem deadline{ActorId{21}, 42, 1};

    placement.enqueue_admitted(priority, 0, false,
                               [](const sched::WorkItem&) {});
    placement.enqueue_admitted(deadline, 0, false,
                               [](const sched::WorkItem&) {});

    sched::WorkItem out;
    EXPECT_TRUE(placement.pop_local(0, out));
    EXPECT_EQ(out.actor, ActorId{21});
}

TEST(WorkPlacementSchedulerTest, PinnedPausedWorkGoesToPinnedQueue) {
    sched::WorkPlacementScheduler placement(2, 4);
    placement.pin_actor_to_worker(ActorId{30}, 1);

    sched::WorkItem item{ActorId{30}, INT64_MAX, 0};
    auto result = placement.enqueue_admitted(
        item, 0, true, [](const sched::WorkItem&) {});

    EXPECT_EQ(result, sched::PlacementResult::EnqueuedShared);

    sched::WorkItem out;
    uint32_t worker_id = 0;
    EXPECT_TRUE(placement.take_pinned_for_test(ActorId{30}, out, worker_id));
    EXPECT_EQ(worker_id, 1u);
    EXPECT_EQ(out.actor, ActorId{30});
}

TEST(WorkPlacementSchedulerTest, DedicatedThreadSuppressesSharedPlacement) {
    sched::WorkPlacementScheduler placement(1, 4);
    placement.register_dedicated_thread(ActorId{40}, -1);

    bool dedicated_called = false;
    auto result = placement.enqueue_admitted(
        sched::WorkItem{ActorId{40}, INT64_MAX, 0}, 0, false,
        [&dedicated_called](const sched::WorkItem&) {
            dedicated_called = true;
        });

    EXPECT_EQ(result, sched::PlacementResult::SuppressedDedicatedThread);
    EXPECT_FALSE(dedicated_called);

    sched::WorkItem out;
    EXPECT_FALSE(placement.pop_local(0, out));
}
```

- [ ] **Step 4: Add placement files to CMake**

In `src/CMakeLists.txt`, add `sched/work_placement_scheduler.cpp` after `sched/actor_ready_gate.cpp`:

```cmake
    sched/actor_ready_gate.cpp
    sched/work_placement_scheduler.cpp
    sched/scheduler.cpp
```

In `tests/unit/sched/CMakeLists.txt`, add `test_work_placement_scheduler.cpp` after `test_actor_ready_gate.cpp`:

```cmake
    test_actor_ready_gate.cpp
    test_work_placement_scheduler.cpp
    test_scheduler_control.cpp
```

- [ ] **Step 5: Replace `HybridScheduler` placement members**

In `include/hpactor/sched/scheduler.hpp`, include the placement header:

```cpp
#include <hpactor/sched/work_placement_scheduler.hpp>
```

Add this member after `ActorReadyGate ready_gate_;`:

```cpp
    WorkPlacementScheduler placement_;
```

Remove these members after all scheduler.cpp references are replaced:

```cpp
    std::vector<WorkerState> workers_;
    A2WS a2ws_;
    struct DedicatedStorage;
    std::unique_ptr<DedicatedStorage> dedicated_;
    mutable std::mutex pinned_mutex_;
    std::unordered_map<ActorId, uint32_t> pinned_actors_;
    std::vector<std::deque<WorkItem>> pinned_ready_;
```

Keep `WorkerState` removed from `HybridScheduler`; it now lives inside `WorkPlacementScheduler`.

- [ ] **Step 6: Initialize placement in the scheduler constructor**

In `src/sched/scheduler.cpp`, update the constructor initializer list:

```cpp
    : system_(system), ready_gate_(system), placement_(num_workers, num_priorities),
      num_workers_(num_workers), num_priorities_(num_priorities),
      workers_paused_(start_paused) {
```

Delete the loop that initializes `workers_[i].queues`; placement now does it.

- [ ] **Step 7: Delegate placement methods from `HybridScheduler`**

Replace `HybridScheduler::enqueue_admitted()` with:

```cpp
void HybridScheduler::enqueue_admitted(const WorkItem& item, uint8_t priority) {
    placement_.enqueue_admitted(
        item, priority, workers_paused_.load(std::memory_order_acquire),
        [this](const WorkItem& dedicated_item) {
            mark_dispatch_begin();
            execute_actor(dedicated_item);
            mark_dispatch_end();
        });
}
```

Replace `pop_local()`, `pop_edf()`, `try_steal()`, and `pop_any_ready()` bodies with calls to `placement_`:

```cpp
bool HybridScheduler::pop_local(WorkItem& out, uint32_t worker_id) {
    return placement_.pop_local(worker_id, out);
}

bool HybridScheduler::pop_edf(WorkItem& out, uint32_t worker_id) {
    return placement_.pop_edf(worker_id, out);
}

bool HybridScheduler::try_steal(WorkItem& out) {
    return placement_.try_steal(tl_current_worker_id, out);
}

bool HybridScheduler::pop_any_ready(WorkItem& out) {
    return placement_.pop_any_for_test(out);
}
```

In `resume_workers()`, replace the pinned flush block with:

```cpp
    placement_.flush_pinned_to_shared();
```

In `pin_actor_to_worker()`, `unpin_actor()`, `register_dedicated_thread()`, `register_dedicated_pool()`, and `unregister_dedicated()`, delegate to `placement_`.

In `run_actor()`, use `placement_.take_pinned_for_test()` and then execute the returned item with the returned worker id.

In `run_one_on_worker()`, use `placement_.pop_one_on_worker_for_test(worker_id, item)`.

- [ ] **Step 8: Remove duplicate EDF check in worker loop**

In `HybridScheduler::worker_loop()`, delete the second `pop_edf(item, worker_id)` block because `pop_local()` already checks EDF first. The loop order becomes:

```cpp
        if (pop_local(item, worker_id)) {
            mark_dispatch_begin();
            execute_actor(item);
            mark_dispatch_end();
            continue;
        }

        if (try_steal(item)) {
            mark_dispatch_begin();
            execute_actor(item);
            mark_dispatch_end();
            continue;
        }
```

- [ ] **Step 9: Run placement and scheduler tests**

Run:

```bash
cmake --build build --target test_unit_sched
./build/tests/unit/sched/test_unit_sched --gtest_filter="WorkPlacementSchedulerTest.*:SchedulerControlTest.*:ActorReadyGateTest.*"
```

Expected:

```text
[  PASSED  ] all selected tests.
```

- [ ] **Step 10: Commit placement extraction**

```bash
git add include/hpactor/sched/work_placement_scheduler.hpp src/sched/work_placement_scheduler.cpp tests/unit/sched/test_work_placement_scheduler.cpp include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp src/CMakeLists.txt tests/unit/sched/CMakeLists.txt
git commit -m "refactor(sched): extract worker placement scheduler"
```

---

### Task 5: Extract Behavior Actor Execution

**Files:**
- Create: `include/hpactor/sched/actor_execution_engine.hpp`
- Create: `src/sched/actor_execution_engine.cpp`
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `src/sched/scheduler.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Create execution engine header**

Create `include/hpactor/sched/actor_execution_engine.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#pragma once

#include <hpactor/log/logger.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/actor_ready_gate.hpp>
#include <hpactor/sched/work_queue.hpp>

#include <cstdint>

namespace hpactor {
class ActorSystem;
class EventBasedActor;
} // namespace hpactor

namespace hpactor::sched {

enum class ActorRunDisposition : uint8_t {
    Skipped,
    SuspendedOrIdle,
    RequeueReady,
    Terminated,
};

struct ActorRunResult {
    ActorRunDisposition disposition{ActorRunDisposition::Skipped};
    uint8_t priority{0};
    int64_t deadline_ns{INT64_MAX};
};

struct ActorExecutionContext {
    uint32_t worker_id{UINT32_MAX};
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics{nullptr};
    log::Logger* logger{nullptr};
};

class BehaviorActorRunner {
  public:
    BehaviorActorRunner(ActorSystem& system, ActorReadyGate& ready_gate) noexcept;

    ActorRunResult run(EventBasedActor& actor, const WorkItem& item,
                       const ActorExecutionContext& context) noexcept;

  private:
    ActorSystem& system_;
    ActorReadyGate& ready_gate_;
};

class ActorExecutionEngine {
  public:
    ActorExecutionEngine(ActorSystem& system, ActorReadyGate& ready_gate) noexcept;

    ActorRunResult run_behavior(EventBasedActor& actor, const WorkItem& item,
                                const ActorExecutionContext& context) noexcept;

  private:
    BehaviorActorRunner behavior_runner_;
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Create behavior execution implementation**

Create `src/sched/actor_execution_engine.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#include <hpactor/sched/actor_execution_engine.hpp>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/types/failure_reason.hpp>

#include <chrono>

namespace hpactor::sched {

namespace {

uint64_t steady_now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void emit_expired_metric(const ActorExecutionContext& context, ActorId actor,
                         uint64_t now_ns) noexcept {
    if (!context.metrics) {
        return;
    }
    metrics::MetricEvent evt{};
    evt.timestamp_ns = now_ns;
    evt.actor_id = actor;
    evt.event_type = metrics::MetricEventType::kDeliveryExpired;
    evt.code = static_cast<uint8_t>(FailureReason::Expired);
    evt.value_hi = 1;
    context.metrics->try_push(evt);
}

} // namespace

BehaviorActorRunner::BehaviorActorRunner(ActorSystem& system,
                                         ActorReadyGate& ready_gate) noexcept
    : system_(system), ready_gate_(ready_gate) {}

ActorRunResult BehaviorActorRunner::run(
    EventBasedActor& actor, const WorkItem& item,
    const ActorExecutionContext& context) noexcept {
    auto& actor_state = actor.actor_state();

    uint32_t expected = ActorState::kReady;
    if (!actor_state.cas(expected, ActorState::kRunning)) {
        if (actor_state.is_terminated()) {
            actor.set_exit_reason(errors::actor_down);
            actor.on_exit();
            return {ActorRunDisposition::Terminated, 0, INT64_MAX};
        }
        return {ActorRunDisposition::Skipped, 0, INT64_MAX};
    }

    auto mailbox = system_.get_mailbox(item.actor);
    if (!mailbox) {
        actor_state.set(ActorState::kIdle);
        return {ActorRunDisposition::Skipped, 0, INT64_MAX};
    }

    TypedMessage msg;
    if (mailbox->try_pop(msg)) {
        uint64_t now_ns = steady_now_ns();
        if (mailbox::is_expired(msg.deadline_ns(), now_ns)) {
            emit_expired_metric(context, item.actor, now_ns);
        } else {
            actor.receive(msg);
        }
    }

    if (!mailbox->empty()) {
        auto admission = ready_gate_.mark_ready_already_admitted(actor);
        if (admission.accepted() ||
            admission.code == ReadyAdmissionCode::AlreadyReady) {
            return {ActorRunDisposition::RequeueReady, 0, INT64_MAX};
        }
        return {ActorRunDisposition::Skipped, 0, INT64_MAX};
    }

    actor_state.set(ActorState::kIdle);
    if (!mailbox->empty()) {
        auto admission = ready_gate_.try_mark_ready(item.actor);
        if (admission.accepted() ||
            admission.code == ReadyAdmissionCode::AlreadyReady) {
            return {ActorRunDisposition::RequeueReady, 0, INT64_MAX};
        }
    }

    return {ActorRunDisposition::SuspendedOrIdle, 0, INT64_MAX};
}

ActorExecutionEngine::ActorExecutionEngine(ActorSystem& system,
                                           ActorReadyGate& ready_gate) noexcept
    : behavior_runner_(system, ready_gate) {}

ActorRunResult ActorExecutionEngine::run_behavior(
    EventBasedActor& actor, const WorkItem& item,
    const ActorExecutionContext& context) noexcept {
    return behavior_runner_.run(actor, item, context);
}

} // namespace hpactor::sched
```

- [ ] **Step 3: Add execution engine to CMake**

In `src/CMakeLists.txt`, add `sched/actor_execution_engine.cpp` after `sched/actor_ready_gate.cpp`:

```cmake
    sched/actor_ready_gate.cpp
    sched/actor_execution_engine.cpp
    sched/work_placement_scheduler.cpp
    sched/scheduler.cpp
```

- [ ] **Step 4: Add execution engine member to `HybridScheduler`**

In `include/hpactor/sched/scheduler.hpp`, include the execution header:

```cpp
#include <hpactor/sched/actor_execution_engine.hpp>
```

Add this member after `WorkPlacementScheduler placement_;`:

```cpp
    ActorExecutionEngine executor_;
```

Update the constructor initializer list:

```cpp
    : system_(system), ready_gate_(system), placement_(num_workers, num_priorities),
      executor_(system, ready_gate_), num_workers_(num_workers),
```

- [ ] **Step 5: Add scheduler dispatch wrapper around execution engine**

In `src/sched/scheduler.cpp`, keep the existing metrics/logging/memory setup in `execute_actor()`, but replace the behavior-mode mailbox and state logic with:

```cpp
    ActorExecutionContext execution_context{
        tl_current_worker_id,
        metrics_ring_buffer_,
        logger_,
    };

    auto result = executor_.run_behavior(*actor, item, execution_context);
    if (result.disposition == ActorRunDisposition::RequeueReady) {
        enqueue_admitted(WorkItem{item.actor, result.deadline_ns, item.sequence},
                         result.priority);
    }
```

Leave the coroutine branch in `HybridScheduler::execute_actor()` for this task. It will move in Task 6.

- [ ] **Step 6: Remove `process_actor()`**

Delete the declaration from `include/hpactor/sched/scheduler.hpp`:

```cpp
    void process_actor(ActorId actor);
```

Delete `HybridScheduler::process_actor()` from `src/sched/scheduler.cpp`.

- [ ] **Step 7: Run behavior scheduler tests**

Run:

```bash
cmake --build build --target test_unit_sched
./build/tests/unit/sched/test_unit_sched --gtest_filter="SchedulerControlTest.*:ActorStateTransferTest.*:ActorReadyGateTest.*:WorkPlacementSchedulerTest.*"
```

Expected:

```text
[  PASSED  ] all selected tests.
```

- [ ] **Step 8: Commit behavior execution extraction**

```bash
git add include/hpactor/sched/actor_execution_engine.hpp src/sched/actor_execution_engine.cpp include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp src/CMakeLists.txt
git commit -m "refactor(sched): extract behavior actor execution"
```

---

### Task 6: Move Coroutine Execution Behind `ActorExecutionEngine`

**Files:**
- Modify: `include/hpactor/sched/actor_execution_engine.hpp`
- Modify: `src/sched/actor_execution_engine.cpp`
- Modify: `src/sched/scheduler.cpp`
- Modify: `tests/integration/sched/test_coroutine_scheduling.cpp`

- [ ] **Step 1: Add coroutine integration test**

Replace `tests/integration/sched/test_coroutine_scheduling.cpp` with:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#include <gtest/gtest.h>

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/sched/coroutine_awaiters.hpp>
#include <scheduler_test_driver.hpp>

#include <atomic>
#include <chrono>

using namespace hpactor;

TEST(CoroutineSchedulingTest, SchedulerComponentsVerify) {
    Config config;
    config.scheduler_threads = 2;
    ActorSystem system(config);

    EXPECT_TRUE(system.scheduler()->is_running());
    EXPECT_EQ(system.scheduler()->worker_count(), 2u);
}

#if HPACTOR_SUPPORT_COROUTINES

class CoroutineCountingActor : public EventBasedActor {
  public:
    CoroutineCountingActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    sched::CoroutineTask act() override {
        for (;;) {
            auto msg = co_await make_mailbox_awaiter();
            if (msg.type_id() == TypeTag::User) {
                received_.fetch_add(1, std::memory_order_relaxed);
            }
            if (received_.load(std::memory_order_relaxed) >= 2) {
                co_return;
            }
        }
    }

    int received() const {
        return received_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<int> received_{0};
};

TEST(CoroutineSchedulingTest, CoroutineActorWakesThroughSchedulerReadiness) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = true;
    cfg.use_coroutines = true;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    hpactor::test::SchedulerTestDriver driver(system);

    auto actor = system.spawn<CoroutineCountingActor>();
    auto* concrete = static_cast<CoroutineCountingActor*>(actor.get().get());

    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    bool done = driver.drain_until([&] { return concrete->received() == 2; }, 10);

    EXPECT_TRUE(done);
    EXPECT_EQ(concrete->received(), 2);
}

#else

TEST(CoroutineSchedulingTest, CoroutineRuntimeSkippedWithoutCoroutineSupport) {
    GTEST_SKIP() << "Coroutine runtime requires C++20 coroutine support";
}

#endif
```

- [ ] **Step 2: Run coroutine integration test and observe current behavior**

Run:

```bash
cmake --build build --target test_integration_sched
./build/tests/integration/sched/test_integration_sched --gtest_filter="CoroutineSchedulingTest.*"
```

Expected before extraction:

```text
CoroutineSchedulingTest.SchedulerComponentsVerify passes.
CoroutineSchedulingTest.CoroutineActorWakesThroughSchedulerReadiness is an
observational regression: keep it as a passing guard if it passes, and keep it
as the red test if it fails.
```

Record the observed result in the implementation notes for the commit.

- [ ] **Step 3: Extend execution engine header for coroutine runner**

In `include/hpactor/sched/actor_execution_engine.hpp`, include coroutine config:

```cpp
#include <hpactor/hpactor_config.hpp>
```

Add this class after `BehaviorActorRunner`:

```cpp
#if HPACTOR_SUPPORT_COROUTINES
class CoroutineActorRunner {
  public:
    explicit CoroutineActorRunner(ActorSystem& system) noexcept;

    ActorRunResult run(EventBasedActor& actor, const WorkItem& item,
                       const ActorExecutionContext& context) noexcept;

  private:
    ActorSystem& system_;
};
#endif
```

Update `ActorExecutionEngine`:

```cpp
    ActorRunResult run(EventBasedActor& actor, const WorkItem& item,
                       const ActorExecutionContext& context,
                       bool use_coroutines) noexcept;
```

Add the coroutine member:

```cpp
#if HPACTOR_SUPPORT_COROUTINES
    CoroutineActorRunner coroutine_runner_;
#endif
```

- [ ] **Step 4: Implement coroutine runner**

In `src/sched/actor_execution_engine.cpp`, add after `BehaviorActorRunner`:

```cpp
#if HPACTOR_SUPPORT_COROUTINES
CoroutineActorRunner::CoroutineActorRunner(ActorSystem& system) noexcept
    : system_(system) {}

ActorRunResult CoroutineActorRunner::run(
    EventBasedActor& actor, const WorkItem& item,
    const ActorExecutionContext& context) noexcept {
    (void)item;
    (void)context;
    (void)system_;

    actor.ensure_coroutine_started();

    auto& coroutine = actor.get_actor_coroutine();
    if (!coroutine) {
        return {ActorRunDisposition::Skipped, 0, INT64_MAX};
    }

    auto& promise = coroutine.task().handle().promise();
    if (promise.actor_state->is_idle() ||
        promise.actor_state->is_io_waiting()) {
        promise.actor_state->set(ActorState::kReady);
    }

    uint32_t expected = ActorState::kReady;
    if (!promise.actor_state->cas(expected, ActorState::kRunning)) {
        if (promise.actor_state->is_terminated()) {
            actor.set_exit_reason(errors::actor_down);
            actor.on_exit();
            return {ActorRunDisposition::Terminated, 0, INT64_MAX};
        }
        return {ActorRunDisposition::Skipped, 0, INT64_MAX};
    }

    coroutine.resume();

    if (coroutine.done()) {
        actor.on_exit();
        return {ActorRunDisposition::Terminated, 0, INT64_MAX};
    }

    return {ActorRunDisposition::SuspendedOrIdle, 0, INT64_MAX};
}
#endif
```

Update the `ActorExecutionEngine` constructor and dispatch method:

```cpp
ActorExecutionEngine::ActorExecutionEngine(ActorSystem& system,
                                           ActorReadyGate& ready_gate) noexcept
    : behavior_runner_(system, ready_gate)
#if HPACTOR_SUPPORT_COROUTINES
      ,
      coroutine_runner_(system)
#endif
{}

ActorRunResult ActorExecutionEngine::run(
    EventBasedActor& actor, const WorkItem& item,
    const ActorExecutionContext& context, bool use_coroutines) noexcept {
#if HPACTOR_SUPPORT_COROUTINES
    if (use_coroutines) {
        return coroutine_runner_.run(actor, item, context);
    }
#else
    (void)use_coroutines;
#endif
    return behavior_runner_.run(actor, item, context);
}
```

- [ ] **Step 5: Replace scheduler coroutine branch with execution engine call**

In `HybridScheduler::execute_actor()`, remove the inline coroutine branch and call:

```cpp
    ActorExecutionContext execution_context{
        tl_current_worker_id,
        metrics_ring_buffer_,
        logger_,
    };

    auto result = executor_.run(*actor, item, execution_context,
                                system_.use_coroutines());
    if (result.disposition == ActorRunDisposition::RequeueReady) {
        enqueue_admitted(WorkItem{item.actor, result.deadline_ns, item.sequence},
                         result.priority);
    }
```

After this replacement, `src/sched/scheduler.cpp` should not include `hpactor/sched/coroutine_task.hpp`.

- [ ] **Step 6: Run coroutine and scheduler tests**

Run:

```bash
cmake --build build --target test_unit_sched test_integration_sched
./build/tests/unit/sched/test_unit_sched --gtest_filter="SchedulerControlTest.*:ActorReadyGateTest.*:WorkPlacementSchedulerTest.*"
./build/tests/integration/sched/test_integration_sched --gtest_filter="CoroutineSchedulingTest.*"
```

Expected:

```text
[  PASSED  ] all selected tests.
```

- [ ] **Step 7: Commit coroutine execution extraction**

```bash
git add include/hpactor/sched/actor_execution_engine.hpp src/sched/actor_execution_engine.cpp src/sched/scheduler.cpp tests/integration/sched/test_coroutine_scheduling.cpp
git commit -m "refactor(sched): move coroutine execution into actor engine"
```

---

### Task 7: Narrow Awaiter Dependencies

**Files:**
- Create: `include/hpactor/sched/scheduler_interfaces.hpp`
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `include/hpactor/sched/coroutine_awaiters.hpp`
- Modify: `include/hpactor/sched/yield_awaiter.hpp`
- Modify: `tests/unit/sched/test_mailbox_awaiter.cpp`

- [ ] **Step 1: Create narrow scheduler interfaces**

Create `include/hpactor/sched/scheduler_interfaces.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#pragma once

#include <hpactor/adt/id.hpp>
#include <hpactor/adt/tags.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <functional>

namespace hpactor::sched {

using TimerHandle = Id<TimerTag>;
using timer_callback = std::function<void()>;

class IActorReadyNotifier {
  public:
    virtual ~IActorReadyNotifier() = default;
    virtual void notify_ready(ActorId actor, uint8_t priority,
                              int64_t deadline_ns) = 0;
};

class ITimerService {
  public:
    virtual ~ITimerService() = default;
    virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;
    virtual void cancel_timer(TimerHandle handle) = 0;
};

class IActorYieldScheduler {
  public:
    virtual ~IActorYieldScheduler() = default;
    virtual void yield(ActorId actor, uint8_t priority) = 0;
};

} // namespace hpactor::sched
```

- [ ] **Step 2: Make `IScheduler` inherit narrow interfaces**

In `include/hpactor/sched/scheduler.hpp`, include the new header:

```cpp
#include <hpactor/sched/scheduler_interfaces.hpp>
```

Remove the local `TimerHandle` and `timer_callback` aliases from `scheduler.hpp`.

Change `IScheduler` declaration:

```cpp
class IScheduler : public IActorReadyNotifier,
                   public ITimerService,
                   public IActorYieldScheduler {
```

Keep all existing virtual method declarations in `IScheduler`; the inherited interfaces use the same signatures.

- [ ] **Step 3: Narrow `TimerAwaiter` dependencies**

In `include/hpactor/sched/coroutine_awaiters.hpp`, include:

```cpp
#include <hpactor/sched/scheduler_interfaces.hpp>
```

Replace the `TimerAwaiter` constructor and members with:

```cpp
    TimerAwaiter(int64_t delay_ns, ITimerService& timer_service,
                 IActorReadyNotifier& ready_notifier, ActorId actor_id,
                 uint8_t priority = 0) noexcept
        : timer_service_(timer_service), ready_notifier_(ready_notifier),
          actor_id_(actor_id), delay_ns_(delay_ns), priority_(priority) {}
```

Replace the timer scheduling call:

```cpp
        timer_handle_ = timer_service_.schedule_after(
            [this] {
                ready_notifier_.notify_ready(actor_id_, priority_, INT64_MAX);
            },
            delay_ns_);
```

Replace `await_cancel()`:

```cpp
    void await_cancel() noexcept {
        timer_service_.cancel_timer(timer_handle_);
    }
```

Replace the private members:

```cpp
    ITimerService& timer_service_;
    IActorReadyNotifier& ready_notifier_;
    ActorId actor_id_;
    int64_t delay_ns_;
    uint8_t priority_;
    TimerHandle timer_handle_{};
    std::coroutine_handle<> continuation_;
```

- [ ] **Step 4: Narrow yield awaiter dependency**

In `include/hpactor/sched/yield_awaiter.hpp`, replace `#include <hpactor/sched/scheduler.hpp>` with:

```cpp
#include <hpactor/sched/scheduler_interfaces.hpp>
```

Replace `IScheduler* scheduler_` members with:

```cpp
    IActorYieldScheduler* scheduler_;
```

Replace constructors to take `IActorYieldScheduler*`:

```cpp
    YieldAwaiter(IActorYieldScheduler* scheduler, ActorId actor_id,
                 uint8_t priority = 0) noexcept
        : scheduler_(scheduler), actor_id_(actor_id), priority_(priority) {}
```

```cpp
    explicit SchedulerYield(IActorYieldScheduler* scheduler,
                            uint8_t priority = 0) noexcept
        : scheduler_(scheduler), priority_(priority) {}
```

- [ ] **Step 5: Run awaiter and coroutine tests**

Run:

```bash
cmake --build build --target test_unit_sched test_integration_sched
./build/tests/unit/sched/test_unit_sched --gtest_filter="MailboxAwaiterTest.*:CoroutineTaskTest.*:SchedulerControlTest.YieldFromRunningActorRequeuesAdmittedWork"
./build/tests/integration/sched/test_integration_sched --gtest_filter="CoroutineSchedulingTest.*"
```

Expected:

```text
[  PASSED  ] all selected tests.
```

- [ ] **Step 6: Commit awaiter dependency cleanup**

```bash
git add include/hpactor/sched/scheduler_interfaces.hpp include/hpactor/sched/scheduler.hpp include/hpactor/sched/coroutine_awaiters.hpp include/hpactor/sched/yield_awaiter.hpp tests/unit/sched/test_mailbox_awaiter.cpp
git commit -m "refactor(sched): narrow coroutine awaiter scheduler dependencies"
```

---

### Task 8: Clean Up Scheduler Facade and Headers

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `src/sched/scheduler.cpp`
- Modify: `include/hpactor/sched/coroutine_task.hpp`
- Modify: `include/hpactor/sched/worker_thread.hpp`

- [ ] **Step 1: Remove unused includes from scheduler facade**

In `src/sched/scheduler.cpp`, remove includes that are no longer used after the extraction:

```cpp
#include <hpactor/sched/coroutine_task.hpp>
#include <hpactor/sched/dedicated_thread_pool.hpp>
```

Keep `event_based_actor.hpp` if `execute_actor()` still casts to `EventBasedActor`.

- [ ] **Step 2: Remove unused public accessors**

In `include/hpactor/sched/scheduler.hpp`, remove these accessors if no source or test references them:

```cpp
    A2WS& a2ws();
    std::vector<WorkerState>& workers();
```

If `WorkerThread` still calls those accessors, update `WorkerThread::try_steal()` to call its owner scheduler's public `try_steal(WorkItem&)` and keep only:

```cpp
    friend class WorkerThread;
```

- [ ] **Step 3: Remove unused coroutine owner state**

Search:

```bash
rg -n "owner" include/hpactor/sched src/sched
```

If `CoroutinePromise::owner` is still unused, remove it from both coroutine-enabled and fallback promise definitions in `include/hpactor/sched/coroutine_task.hpp`:

```cpp
    WorkerThread* owner{nullptr};
```

Also remove the forward declaration:

```cpp
class WorkerThread;
```

Keep `WorkerThread` forward-declared if another declaration in the same header still uses it.

- [ ] **Step 4: Run compile-focused scheduler tests**

Run:

```bash
cmake --build build --target test_unit_sched test_integration_sched
./build/tests/unit/sched/test_unit_sched --gtest_filter="HybridSchedulerTest.*:WorkerThreadTest.*:SchedulerControlTest.*"
./build/tests/integration/sched/test_integration_sched --gtest_filter="WorkerThreadTest.*:PrioritySchedulerTest.*:CoroutineSchedulingTest.*"
```

Expected:

```text
[  PASSED  ] all selected tests.
```

- [ ] **Step 5: Commit cleanup**

```bash
git add include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp include/hpactor/sched/coroutine_task.hpp include/hpactor/sched/worker_thread.hpp
git commit -m "refactor(sched): clean scheduler facade after extraction"
```

---

### Task 9: Final Targeted Verification

**Files:**
- No source edits unless verification reveals a concrete failure.

- [ ] **Step 1: Run targeted scheduler unit tests**

Run:

```bash
cmake --build build --target test_unit_sched
./build/tests/unit/sched/test_unit_sched
```

Expected:

```text
[  PASSED  ] all tests in test_unit_sched.
```

- [ ] **Step 2: Run targeted scheduler integration tests**

Run:

```bash
cmake --build build --target test_integration_sched
./build/tests/integration/sched/test_integration_sched
```

Expected:

```text
[  PASSED  ] all tests in test_integration_sched.
```

- [ ] **Step 3: Run diff hygiene**

Run:

```bash
git diff --check
```

Expected:

```text
no output
```

- [ ] **Step 4: Inspect scheduler coupling**

Run:

```bash
rg -n "ActorCoroutine::resume|ensure_coroutine_started|receive\\(" src/sched/work_placement_scheduler.cpp include/hpactor/sched/work_placement_scheduler.hpp
rg -n "workers_|pinned_ready_|a2ws_" src/sched/actor_execution_engine.cpp include/hpactor/sched/actor_execution_engine.hpp
```

Expected:

```text
no output
```

- [ ] **Step 5: Close verification loop**

If Task 9 required a source or documentation edit, return to the task that owns
that file, apply the fix there, rerun that task's targeted verification command,
and use that task's commit step. If Task 9 made no edits, verify the worktree
contains only the intended committed changes:

```bash
git status --short
```

Do not create an empty commit.

---

## Spec Coverage Self-Review

- Worker placement independence: Tasks 2 and 4 route admitted work through one placement path and extract `WorkPlacementScheduler`.
- Actor activation independence: Tasks 5 and 6 move behavior and coroutine execution behind `ActorExecutionEngine`.
- Coroutine isolation: Tasks 6 and 7 move coroutine resume out of `HybridScheduler` and narrow awaiter dependencies.
- Requeue correctness: Tasks 1 and 2 cover pinned behavior requeue and yield requeue.
- Deterministic worker controls: Tasks 1, 4, 5, and 8 keep `run_one_ready()`, `run_actor()`, and `run_one_on_worker()` under targeted tests.
- Observability: Tasks 4 and 5 preserve steal, dispatch, and delivery-expired metrics in the extracted components.
- Cleanup: Task 8 removes duplicate/dead scheduler code and unused coupling.

No separate full-project rebuild is required by this plan unless targeted scheduler builds expose a dependency or CMake source-list issue that cannot be diagnosed with `test_unit_sched` and `test_integration_sched`.
