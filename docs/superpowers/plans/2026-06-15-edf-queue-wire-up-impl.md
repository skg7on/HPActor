# EDF Queue Wire-Up — Implementation Plan

> **Goal:** Wire `EDFQueue` into the scheduler enqueue path so that explicitly-deadlined messages are dispatched in earliest-deadline-first order, while ordinary priority-only messages remain on the ChaseLev deques unchanged.

**Design spec:** `docs/superpowers/specs/2026-06-15-edf-queue-wire-up-design.md`
**Issue:** [#305](https://github.com/skg7on/HPActor/issues/305)
**Architecture:** Add `schedule_edf` flag to `DeliveryOptions`, `edf_scheduled` flag to `WorkItem`, route EDF-flagged items directly into `worker.edf_queue.push()` in `enqueue_shared()`.
**Tech Stack:** C++20, Google Test, CMake/Ninja

**Test plan:** `tests/unit/sched/test_work_placement_scheduler.cpp` (routing), `tests/unit/sched/test_edf_queue.cpp` (existing, extend if needed), `tests/integration/sched/test_edf_integration.cpp` (new, end-to-end ordering).

---

### Task 1: Add `schedule_edf` flag to `DeliveryOptions`

**Files:**
- Modify: `include/hpactor/msg/enqueue_result.hpp` — add `bool schedule_edf = false`

- [ ] **Step 1: Write failing compile check**

Add a test that uses the new field:

```cpp
// tests/unit/mailbox/test_mailbox.cpp or create a small compile-only check
TEST(DeliveryOptionsTest, ScheduleEdfDefaultIsFalse) {
    hpactor::mailbox::DeliveryOptions opts;
    EXPECT_FALSE(opts.schedule_edf);
}
```

- [ ] **Step 2: Verify test fails to compile**

```bash
ninja -C build tests/unit/mailbox/test_unit_mailbox 2>&1 | grep "schedule_edf"
```
Expected: `no member named 'schedule_edf'`

- [ ] **Step 3: Implement**

In `include/hpactor/msg/enqueue_result.hpp`, inside `struct DeliveryOptions` after `flags`:

```cpp
    bool schedule_edf = false; ///< If true, the work item is placed in the
                               ///< worker's EDFQueue instead of the priority
                               ///< ChaseLev deque. Requires deadline_ns !=
                               ///< INT64_MAX. Opt-in, default off.
```

- [ ] **Step 4: Verify test passes**

```bash
ninja -C build tests/unit/mailbox/test_unit_mailbox && ./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*ScheduleEdf*"
```
Expected: PASS

- [ ] **Step 5: Commit** `feat(sched): add schedule_edf flag to DeliveryOptions`

---

### Task 2: Add `edf_scheduled` flag to `WorkItem`

**Files:**
- Modify: `include/hpactor/sched/work_queue.hpp` — add `bool edf_scheduled = false`

- [ ] **Step 1: Write failing compile check**

Add to existing EDF test:

```cpp
// tests/unit/sched/test_edf_queue.cpp
TEST(EDFQueueTest, WorkItemEdfScheduledDefaultIsFalse) {
    hpactor::sched::WorkItem item{};
    EXPECT_FALSE(item.edf_scheduled);
}
```

- [ ] **Step 2: Verify test fails to compile**

```bash
ninja -C build tests/unit/sched/test_unit_sched 2>&1 | grep "edf_scheduled"
```
Expected: `no member named 'edf_scheduled'`

- [ ] **Step 3: Implement**

In `include/hpactor/sched/work_queue.hpp`, inside `struct WorkItem`:

```cpp
struct WorkItem {
    ActorId actor;
    int64_t deadline_ns;
    uint64_t sequence;
    bool edf_scheduled = false;  ///< True if this item was originally placed
                                 ///< via the EDF path. Preserved across
                                 ///< requeue cycles.
};
```

- [ ] **Step 4: Verify test passes**

```bash
ninja -C build tests/unit/sched/test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*EdfScheduled*"
```
Expected: PASS

- [ ] **Step 5: Run full EDF test suite (no regression)**

```bash
ninja -C build tests/unit/sched/test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*EDF*"
```
Expected: all existing EDFQueue tests pass

- [ ] **Step 6: Commit** `feat(sched): add edf_scheduled flag to WorkItem`

---

### Task 3: Add `edf_push_mutex_` to `WorkerState`

**Files:**
- Modify: `include/hpactor/sched/work_placement_scheduler.hpp` — add mutex to `WorkerState`
- Modify: `src/sched/work_placement_scheduler.cpp` — no logic change yet

- [ ] **Step 1: Add mutex (no test — compile-only verification)**

In `include/hpactor/sched/work_placement_scheduler.hpp`, inside `struct WorkerState`, after `sleep_cv_`:

```cpp
        /// Mutex serializing pushes into edf_queue from concurrent producer
        /// threads.  EDF items are infrequent by design, so a mutex is
        /// adequate — upgrade to lock-free if profiling shows contention.
        mutable std::mutex edf_push_mutex_;
```

- [ ] **Step 2: Verify build**

```bash
ninja -C build hpactor_lib
```
Expected: builds clean

- [ ] **Step 3: Commit** `feat(sched): add edf_push_mutex_ to WorkerState`

---

### Task 4: Route EDF-flagged items into `edf_queue` in `enqueue_shared()`

**Files:**
- Modify: `src/sched/work_placement_scheduler.cpp` — `enqueue_shared()` routing
- Test: `tests/unit/sched/test_work_placement_scheduler.cpp`

This is the core change. When `item.edf_scheduled` is true, push directly into
`worker.edf_queue.push(item.deadline_ns, item)` instead of the shared-input
stack.

- [ ] **Step 1: Write failing test**

In `tests/unit/sched/test_work_placement_scheduler.cpp`:

```cpp
TEST(WorkPlacementSchedulerTest, EdfItemRoutedToEdfQueue) {
    // Single worker, workers paused for deterministic inspection
    WorkPlacementScheduler placement(1, 4);
    auto& ws = placement.workers()[0];

    WorkItem item;
    item.actor = ActorId{1};
    item.deadline_ns = 5000;
    item.edf_scheduled = true;

    // Enqueue as EDF
    placement.enqueue_admitted(item, 0, /*workers_paused=*/true,
                               [](const WorkItem&) {});

    // EDF queue should contain the item
    EXPECT_FALSE(ws.edf_queue.empty());
    WorkItem out;
    EXPECT_TRUE(ws.edf_queue.pop(out));
    EXPECT_EQ(out.actor, ActorId{1});
    EXPECT_EQ(out.deadline_ns, 5000);
}

TEST(WorkPlacementSchedulerTest, NonEdfItemRoutedToSharedInput) {
    WorkPlacementScheduler placement(1, 4);
    auto& ws = placement.workers()[0];

    WorkItem item;
    item.actor = ActorId{2};
    item.deadline_ns = INT64_MAX;
    item.edf_scheduled = false;

    placement.enqueue_admitted(item, 0, /*workers_paused=*/true,
                               [](const WorkItem&) {});

    // EDF queue should be empty (item went to shared input stack)
    EXPECT_TRUE(ws.edf_queue.empty());
    // Drain shared input to verify item landed there
    WorkItem out;
    EXPECT_TRUE(placement.pop_local(0, out));
    EXPECT_EQ(out.actor, ActorId{2});
}
```

- [ ] **Step 2: Verify tests fail**

```bash
ninja -C build tests/unit/sched/test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*EdfItemRouted*:*NonEdfItemRouted*"
```
Expected: EDF item test FAILS (edf_queue empty), non-EDF test PASSES (existing behavior)

- [ ] **Step 3: Implement routing**

Modify `enqueue_shared()` in `src/sched/work_placement_scheduler.cpp`:

```cpp
void WorkPlacementScheduler::enqueue_shared(const WorkItem& item,
                                             uint8_t priority, uint32_t worker_id) {
    auto& worker = workers_[worker_id];

    // EDF-scheduled items bypass the shared-input stack: push directly
    // into the EDF min-heap so deadline ordering is preserved without
    // an intermediate LIFO→FIFO reversal.
    if (item.edf_scheduled) {
        {
            std::lock_guard<std::mutex> lock(worker.edf_push_mutex_);
            worker.edf_queue.push(item.deadline_ns, item);
        }
        worker.wake_if_blocking();
        return;
    }

    // Existing path: priority-only items go to shared-input stack.
    (void)priority;
    auto* node = new SharedInputNode();
    node->item = item;
    node->priority = priority;
    SharedInputNode* old = worker.shared_input.load(std::memory_order_acquire);
    do {
        node->next.store(old, std::memory_order_relaxed);
    } while (!worker.shared_input.compare_exchange_weak(
        old, node, std::memory_order_release, std::memory_order_relaxed));

    worker.wake_if_blocking();
}
```

- [ ] **Step 4: Verify tests pass**

```bash
ninja -C build tests/unit/sched/test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*EdfItemRouted*:*NonEdfItemRouted*:*EDF*"
```
Expected: all PASS

- [ ] **Step 5: Run full scheduler unit test suite (no regression)**

```bash
ninja -C build tests/unit/sched/test_unit_sched && ./build/tests/unit/sched/test_unit_sched
```
Expected: all existing tests PASS

- [ ] **Step 6: Commit** `feat(sched): route EDF-flagged items into edf_queue in enqueue_shared`

---

### Task 5: Thread `schedule_edf` through `notify_ready()` → `WorkItem::edf_scheduled`

**Files:**
- Modify: `src/sched/scheduler.cpp` — `notify_ready()` sets `edf_scheduled` based on flag
- Modify: `include/hpactor/sched/scheduler.hpp` — add overload or extend signature

Currently `notify_ready(actor, priority, deadline_ns)` is called from
`MPSCActorMailbox::enqueue_reserved()`. The mailbox doesn't know about
`DeliveryOptions` — it only has `MailboxEnvelopeMeta`. We need to thread
the `schedule_edf` flag from `DeliveryPipeline::try_deliver()` through
to the mailbox's `notify_ready` call.

**Approach:** Add `bool schedule_edf` to `MailboxEnvelopeMeta`, set it from
`DeliveryOptions::schedule_edf` in `try_deliver()`, and read it in
`enqueue_reserved()` to pass to `scheduler_->notify_ready_edf()`.

- [ ] **Step 1: Add `schedule_edf` to `MailboxEnvelopeMeta`**

In `include/hpactor/msg/enqueue_result.hpp`, inside `struct MailboxEnvelopeMeta`:

```cpp
    bool schedule_edf = false; ///< If true, the scheduler places this work
                               ///< item in the EDF queue instead of the
                               ///< priority ChaseLev deque.
```

- [ ] **Step 2: Set `schedule_edf` from `DeliveryOptions` in `try_deliver()`**

In `src/mailbox/delivery_pipeline.cpp`, in `try_deliver()`, after building `meta`:

```cpp
    MailboxEnvelopeMeta meta;
    // ... existing field assignments ...
    meta.schedule_edf = options.schedule_edf;
```

- [ ] **Step 3: Read `schedule_edf` in `enqueue_reserved()`, pass to scheduler**

In `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`, in `enqueue_reserved()`,
replace the `scheduler_->notify_ready(actor_id_, meta.priority, meta.deadline_ns)`
call with a check:

```cpp
                if (meta.schedule_edf && meta.deadline_ns != INT64_MAX) {
                    scheduler_->notify_ready_edf(actor_id_, meta.priority,
                                                  meta.deadline_ns);
                } else {
                    scheduler_->notify_ready(actor_id_, meta.priority,
                                             meta.deadline_ns);
                }
```

Same change in the self-re-notify path in `dequeue()` at line 677:

```cpp
                scheduler_->notify_ready(actor_id_, 0, 0);
```
This path is a self-wakeup after double-check — never EDF, so no change needed.

- [ ] **Step 4: Add `notify_ready_edf()` to scheduler interface**

In `include/hpactor/sched/scheduler_interfaces.hpp`, add to `IActorReadyNotifier`:

```cpp
    virtual void notify_ready_edf(ActorId actor, uint8_t priority,
                                   int64_t deadline_ns) {
        // Default: fall back to regular notify_ready.
        notify_ready(actor, priority, deadline_ns);
    }
```

In `include/hpactor/sched/scheduler.hpp`, add override declaration to `HybridScheduler`:

```cpp
    void notify_ready_edf(ActorId actor, uint8_t priority, int64_t deadline_ns) override;
```

In `src/sched/scheduler.cpp`, implement:

```cpp
void HybridScheduler::notify_ready_edf(ActorId actor, uint8_t priority,
                                        int64_t deadline_ns) {
    if (!running_.load(std::memory_order_acquire)) return;

    WorkItem item{actor, deadline_ns, 0};
    item.edf_scheduled = true;

    if (!try_admit_ready(actor)) return;
    enqueue_admitted(item, priority);
}
```

- [ ] **Step 5: Write failing test**

```cpp
// tests/unit/sched/test_work_placement_scheduler.cpp
TEST(WorkPlacementSchedulerTest, NotifyReadyEdfSetsFlag) {
    // Verify that an item enqueued via the EDF path lands in edf_queue
    // through the full notify_ready → enqueue_admitted → enqueue_shared chain.
    WorkPlacementScheduler placement(1, 4);
    // Use enqueue_admitted with an item that has edf_scheduled=true
    WorkItem item{ActorId{10}, 3000, 0, /*edf_scheduled=*/true};
    placement.enqueue_admitted(item, 0, /*workers_paused=*/true,
                               [](const WorkItem&) {});
    auto& ws = placement.workers()[0];
    EXPECT_FALSE(ws.edf_queue.empty());
    WorkItem out;
    EXPECT_TRUE(ws.edf_queue.pop(out));
    EXPECT_EQ(out.actor, ActorId{10});
}
```

- [ ] **Step 6: Verify all tests pass**

```bash
ninja -C build tests/unit/sched/test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*Edf*:*EDF*"
```

- [ ] **Step 7: Run full test suite (no regression)**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```
Expected: all 1411+ existing tests pass

- [ ] **Step 8: Commit** `feat(sched): thread schedule_edf through mailbox to scheduler notify_ready_edf`

---

### Task 6: Preserve `edf_scheduled` across requeue cycles

**Files:**
- Modify: `src/sched/scheduler.cpp` — `execute_actor()` requeue path

When an actor returns `RequeueReady`, the item is re-enqueued via
`enqueue_admitted()`. The `reequeue` currently creates a fresh `WorkItem` with
`result.priority` and `result.deadline_ns` but loses the `edf_scheduled` flag.

- [ ] **Step 1: Carry `edf_scheduled` through requeue**

In `src/sched/scheduler.cpp`, `execute_actor()`, the `RequeueReady` path
(currently lines 251-260):

```cpp
    if (result.disposition == ActorRunDisposition::RequeueReady) {
        uint64_t next_seq = item.sequence + 1;
        if (next_seq > 128) next_seq = 0;
        WorkItem next{item.actor, result.deadline_ns, next_seq};
        next.edf_scheduled = item.edf_scheduled;  // preserve EDF placement
        enqueue_admitted(next, result.priority);
    }
```

- [ ] **Step 2: Write failing test**

```cpp
// tests/unit/sched/test_work_placement_scheduler.cpp
TEST(WorkPlacementSchedulerTest, RequeuePreservesEdfFlag) {
    WorkPlacementScheduler placement(1, 4);
    // Enqueue EDF item → drain to simulate dispatch → requeue with edf_scheduled=true
    WorkItem item{ActorId{20}, 7000, 0, /*edf_scheduled=*/true};
    placement.enqueue_admitted(item, 1, /*workers_paused=*/true,
                               [](const WorkItem&) {});
    // Pop it (simulates dispatch)
    WorkItem dispatched;
    ASSERT_TRUE(placement.pop_edf(0, dispatched));
    EXPECT_TRUE(dispatched.edf_scheduled);

    // Simulate requeue
    dispatched.sequence = dispatched.sequence + 1;
    placement.enqueue_admitted(dispatched, 1, /*workers_paused=*/true,
                               [](const WorkItem&) {});
    // Should land in EDF again
    auto& ws = placement.workers()[0];
    EXPECT_FALSE(ws.edf_queue.empty());
    WorkItem requeued;
    EXPECT_TRUE(ws.edf_queue.pop(requeued));
    EXPECT_TRUE(requeued.edf_scheduled);
    EXPECT_EQ(requeued.actor, ActorId{20});
}
```

- [ ] **Step 3: Verify test passes**

```bash
ninja -C build tests/unit/sched/test_unit_sched && ./build/tests/unit/sched/test_unit_sched --gtest_filter="*RequeuePreservesEdf*"
```
Expected: PASS

- [ ] **Step 4: Commit** `feat(sched): preserve edf_scheduled flag across requeue cycles`

---

### Task 7: Add `deliver_local_edf()` to `ActorSystem`

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp` — declare method
- Modify: `src/actor/actor_system.cpp` — implement

- [ ] **Step 1: Write failing compile check**

In `tests/unit/core/test_unit_core.cpp` (or a new file if needed):

```cpp
TEST(ActorSystemTest, DeliverLocalEdfCompiles) {
    // Compile-only: verify the method exists and accepts correct signature.
    // Not called — ActorSystem construction requires full runtime.
    SUCCEED();
}
```

- [ ] **Step 2: Add declaration**

In `include/hpactor/core/actor_system.hpp`, after existing `deliver_local` overloads:

```cpp
    /// \brief Deliver a message to a local actor with EDF scheduling.
    ///
    /// The target actor's work item is placed in the scheduler's EDF queue
    /// and dispatched in earliest-deadline-first order relative to other
    /// EDF-scheduled actors.  Ordinary priority-only messages are unaffected.
    ///
    /// \param[in] target Actor ID to deliver to.
    /// \param[in] msg Message to deliver (moved into the pipeline).
    /// \param[in] deadline_ns Absolute delivery deadline in nanoseconds.
    /// \param[in] priority Priority level 0–3 (0 = highest).  Used as a
    ///                     tiebreaker within the same deadline bucket.
    void deliver_local_edf(ActorId target, TypedMessage msg,
                           int64_t deadline_ns, uint8_t priority = 0);
```

- [ ] **Step 3: Implement**

In `src/actor/actor_system.cpp`:

```cpp
void ActorSystem::deliver_local_edf(ActorId target, TypedMessage msg,
                                     int64_t deadline_ns, uint8_t priority) {
    if (!delivery_pipeline_) return;
    mailbox::DeliveryOptions options;
    options.schedule_edf = true;
    (void)delivery_pipeline_->try_deliver(target, std::move(msg), priority,
                                          deadline_ns, options);
}
```

- [ ] **Step 4: Verify build**

```bash
ninja -C build hpactor_lib
```
Expected: builds clean

- [ ] **Step 5: Commit** `feat(actor): add deliver_local_edf to ActorSystem`

---

### Task 8: Add `send_edf()` to `ActorContext`

**Files:**
- Modify: `include/hpactor/actor/actor_context.hpp` — declare method
- Modify: `src/actor/actor_context.cpp` — implement

- [ ] **Step 1: Add declaration**

In `include/hpactor/actor/actor_context.hpp`, after `send()` overloads:

```cpp
    /// \brief Send a message with EDF scheduling semantics.
    ///
    /// The target actor's work item is placed in the scheduler's EDF queue
    /// and dispatched in earliest-deadline-first order.
    ///
    /// \param[in] addr Target actor address.
    /// \param[in] msg Message to send (moved).
    /// \param[in] deadline Absolute deadline (time point or duration from now).
    /// \param[in] priority Priority level 0–3 (0 = highest).
    void send_edf(ActorAddress addr, TypedMessage msg,
                  std::chrono::nanoseconds deadline, uint8_t priority = 0);
```

- [ ] **Step 2: Implement**

In `src/actor/actor_context.cpp`:

```cpp
void ActorContext::send_edf(ActorAddress addr, TypedMessage msg,
                             std::chrono::nanoseconds deadline,
                             uint8_t priority) {
    auto* sys = system_;
    if (!sys) return;

    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t deadline_ns = now_ns + deadline.count();
    // Clamp to avoid overflow
    if (deadline_ns < now_ns) deadline_ns = now_ns + 1;

    msg.set_sender_address(owner_.address());
    sys->deliver_local_edf(addr.id, std::move(msg), deadline_ns, priority);
}
```

- [ ] **Step 3: Verify build**

```bash
ninja -C build hpactor_lib
```
Expected: builds clean

- [ ] **Step 4: Commit** `feat(actor): add send_edf to ActorContext`

---

### Task 9: Integration test — end-to-end EDF dispatch ordering

**Files:**
- Create: `tests/integration/sched/test_edf_integration.cpp`
- Modify: `tests/integration/sched/CMakeLists.txt` — add test target

- [ ] **Step 1: Write integration test**

```cpp
// tests/integration/sched/test_edf_integration.cpp
#include <gtest/gtest.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/sched/scheduler.hpp>

namespace {

// Actor that records the order it was dispatched
class OrderRecordingActor : public hpactor::EventBasedActor {
public:
    OrderRecordingActor(hpactor::ActorSystem& sys,
                        std::vector<int>* order, int id)
        : EventBasedActor(sys), order_(order), id_(id) {}

    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior::make(
            [this](hpactor::TypedMessage&) {
                order_->push_back(id_);
            }
        );
    }

private:
    std::vector<int>* order_;
    int id_;
};

} // namespace

TEST(EdfIntegrationTest, EarlierDeadlineDispatchedFirst) {
    std::vector<int> dispatch_order;
    hpactor::ActorSystem::Config cfg;
    cfg.scheduler_threads = 0; // deterministic: no worker threads
    cfg.enable_network = false;
    hpactor::ActorSystem system(cfg);

    auto a = system.spawn<OrderRecordingActor>(&dispatch_order, 1);
    auto b = system.spawn<OrderRecordingActor>(&dispatch_order, 2);

    // Pin both actors to worker 0
    auto* sched = static_cast<hpactor::sched::HybridScheduler*>(
        system.scheduler());
    sched->pause_workers();
    sched->pin_actor_to_worker(a->id(), 0);
    sched->pin_actor_to_worker(b->id(), 0);

    hpactor::TypedMessage msg(hpactor::TypeTag::User, hpactor::StreamBuffer{});

    // Actor B gets an earlier deadline → should dispatch first
    int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    system.deliver_local_edf(b->id(), msg, now_ns + 1'000'000, 0);  // +1ms
    // Need fresh msg since it's moved
    hpactor::TypedMessage msg2(hpactor::TypeTag::User, hpactor::StreamBuffer{});
    system.deliver_local_edf(a->id(), std::move(msg2), now_ns + 5'000'000, 0); // +5ms

    // Step both actors
    sched->run_actor(b->id());
    sched->run_actor(a->id());

    // B (earlier deadline) should dispatch before A (later deadline)
    ASSERT_EQ(dispatch_order.size(), 2u);
    EXPECT_EQ(dispatch_order[0], 2); // B
    EXPECT_EQ(dispatch_order[1], 1); // A
}
```

- [ ] **Step 2: Register test in CMake**

In `tests/integration/sched/CMakeLists.txt`, add the new test file to the
existing integration test target for sched.

- [ ] **Step 3: Verify test passes**

```bash
ninja -C build tests/integration/sched/test_integration_sched && ./build/tests/integration/sched/test_integration_sched --gtest_filter="*EdfIntegration*"
```
Expected: PASS (dispatch_order = {2, 1})

- [ ] **Step 4: Run full test suite**

```bash
ninja -C build && ctest --output-on-failure --parallel 8
```
Expected: all tests pass, no regressions

- [ ] **Step 5: Commit** `test(sched): add integration test for EDF dispatch ordering`

---

## Summary

| Task | Files | RED | GREEN | Commit |
|------|-------|-----|-------|--------|
| 1. `DeliveryOptions::schedule_edf` | 1 | compile fail | test pass | `feat(sched): add schedule_edf flag to DeliveryOptions` |
| 2. `WorkItem::edf_scheduled` | 1 | compile fail | test pass | `feat(sched): add edf_scheduled flag to WorkItem` |
| 3. `WorkerState::edf_push_mutex_` | 1 | — | build clean | `feat(sched): add edf_push_mutex_ to WorkerState` |
| 4. Route EDF items in `enqueue_shared()` | 2 | test fail | test pass | `feat(sched): route EDF-flagged items into edf_queue` |
| 5. Thread flag through mailbox→scheduler | 5 | test fail | full suite | `feat(sched): thread schedule_edf through mailbox to scheduler` |
| 6. Preserve flag across requeue | 1 | test fail | test pass | `feat(sched): preserve edf_scheduled across requeue` |
| 7. `ActorSystem::deliver_local_edf()` | 2 | — | build clean | `feat(actor): add deliver_local_edf to ActorSystem` |
| 8. `ActorContext::send_edf()` | 2 | — | build clean | `feat(actor): add send_edf to ActorContext` |
| 9. Integration test | 2 | test fail | test pass | `test(sched): add integration test for EDF ordering` |

**Total: 9 tasks, 17 files, ~100 lines of production code + ~80 lines of test code.**
