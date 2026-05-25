# Mailbox Overflow Policies — Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `RejectNewest`, `SignalOnly`, and `SpillToOverflowQueue` overflow policies in `MPSCActorMailbox`, create the `OverflowQueue<T>` auxiliary data structure, extend TOML config parsing, and wire observability (metrics, CLI snapshots, backpressure signals).

**Architecture:** New header-only `OverflowQueue<T>` class, modifications to `MPSCActorMailbox` (policy cases in `try_push()`, drain mechanism in `try_pop()`), config propagation through `ActorSystem` and `MailboxConfigParser`, CLI snapshot extension, and actor_system backpressure signal routing for `SignalOnly`.

**Tech Stack:** C++20, no-exceptions, no-RTTI, LLVM style.

**Files map:**

| File | Role |
|------|------|
| `include/hpactor/mailbox/overflow_queue.hpp` (new) | `OverflowQueue<T>` class with `OverflowQueueSnapshot` |
| `tests/unit/mailbox/test_mailbox_overflow_queue.cpp` (new) | Unit tests for OverflowQueue |
| `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` (modify) | Policy cases in `try_push()`, `drain_overflow()`, snapshot |
| `include/hpactor/cli/cli_types.hpp` (modify) | Overflow fields in `MboxSnapshot` |
| `include/hpactor/config/mailbox_fields.def` (modify) | New config fields |
| `src/actor/actor_system.cpp` (modify) | Config propagation, SignalOnly backpressure routing |
| `src/config/parsers/mailbox_config_parser.cpp` (modify) | TOML parsing for new fields |
| `tests/unit/mailbox/test_mailbox_overflow_policies.cpp` (modify) | 6 new policy test cases |
| `tests/unit/mailbox/CMakeLists.txt` (modify) | Register new test file |

---

### Task 1: Create `OverflowQueue<T>` class

**Files:**
- Create: `include/hpactor/mailbox/overflow_queue.hpp`

- [x] **Step 1: Write the header**

```cpp
template <typename T>
class OverflowQueue {
  public:
    explicit OverflowQueue(uint32_t max_depth = 0) noexcept;
    void set_max_depth(uint32_t max_depth) noexcept;
    bool try_push(T&& msg) noexcept;
    bool try_pop(T& out) noexcept;
    OverflowQueueSnapshot snapshot() const noexcept;
    // depth(), empty(), max_depth() accessors
  private:
    mutable std::mutex mutex_;
    std::deque<T> queue_;
    uint32_t max_depth_{0};
    uint64_t total_pushed_{0}, total_popped_{0}, total_lost_{0};
};
```

`try_push` evicts oldest element when `max_depth_ > 0 && queue_.size() >= max_depth_`.
`snapshot()` returns all counters atomically under the mutex.

### Task 2: Write OverflowQueue unit tests

**Files:**
- Create: `tests/unit/mailbox/test_mailbox_overflow_queue.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

- [x] **Step 1: 10 test cases**

| Test | What it validates |
|------|-------------------|
| `DefaultConstructionEmpty` | Initial state, snapshot zeroed |
| `PushPopSingle` | Single element push/pop round-trip |
| `PushPopFifoOrder` | FIFO ordering preserved |
| `TryPopEmptyReturnsFalse` | Empty pop returns false, out untouched |
| `MaxDepthZeroUnlimited` | max_depth=0 allows unlimited growth |
| `MaxDepthEnforcesBoundedCapacity` | Eviction at capacity, oldest dropped |
| `SetMaxDepthDynamically` | Runtime max_depth change |
| `SnapshotCounters` | All counters (pushed, popped, lost) accurate |
| `ConcurrentProducerConsumer` | Single producer, single consumer, 1000 msgs |
| `MoveSemanticsPreserved` | Works with move-only types and std::string |

### Task 3: Implement policy cases in MPSCActorMailbox

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

- [x] **Step 1: Add `#include <hpactor/mailbox/overflow_queue.hpp>`**

- [x] **Step 2: Add `OverflowQueue<T> overflow_queue_` member**

- [x] **Step 3: Set overflow_queue max_depth in constructor**

```cpp
overflow_queue_.set_max_depth(config_.max_overflow_depth);
```

- [x] **Step 4: Replace default case with three new policy cases**

- `RejectNewest`: reject + `kMailboxRejected` metric
- `SignalOnly`: reject + `kMailboxRejected` metric + `retry_after = signal_min_interval_ms`
- `SpillToOverflowQueue`: `overflow_queue_.try_push()` → `ReroutedToOverflow`; on overflow queue full → reject + metric
- Default case simplified to only cover `DropLowestPriority` and `BlockWhenAllowed` (still stubs)

- [x] **Step 5: Add `drain_overflow()` private method**

Called at end of `try_pop()`:
```
while (overflow_queue non-empty):
  reserve main capacity
  pop from overflow
  allocate + enqueue into main mailbox
  if any step fails: re-queue + break
```

- [x] **Step 6: Extend `snapshot()` to include overflow queue stats**

```cpp
auto oq_snap = overflow_queue_.snapshot();
s.overflow_depth = oq_snap.depth;
s.overflow_max_depth = oq_snap.max_depth;
s.overflow_total_pushed = oq_snap.total_pushed;
```

### Task 4: Extend config surface

**Files:**
- Modify: `include/hpactor/config/mailbox_fields.def`
- Modify: `src/config/parsers/mailbox_config_parser.cpp`
- Modify: `src/actor/actor_system.cpp`

- [x] **Step 1: Add field definitions to mailbox_fields.def**

```
HPACTOR_MAILBOX_FIELD(max_overflow_depth, uint32_t, "max_overflow_depth", 0)
HPACTOR_MAILBOX_FIELD(signal_min_interval_ms, uint32_t, "signal_min_interval_ms", 100)
```

- [x] **Step 2: Parse in MailboxConfigParser**

```cpp
out.mailbox.max_overflow_depth = mt.read_uint32("max_overflow_depth", 0);
out.mailbox.signal_min_interval_ms = mt.read_uint32("signal_min_interval_ms", 100);
```

- [x] **Step 3: Propagate in mailbox_config_for_spawn()**

```cpp
cfg.max_overflow_depth = config_.mailbox.max_overflow_depth;
cfg.signal_min_interval_ms = config_.mailbox.signal_min_interval_ms;
```

### Task 5: Wire SignalOnly backpressure in actor_system

**Files:**
- Modify: `src/actor/actor_system.cpp`

- [x] **Step 1: Extend backpressure signal condition**

```cpp
bool should_signal =
    (result.code == AcceptedWithSoftPressure) ||
    (result.code == Rejected && result.retry_after.count() > 0);
if (should_signal && options.emit_backpressure) {
    signal.reason = result.code == Rejected
        ? BackpressureReason::OverflowPolicy
        : BackpressureReason::HighWatermark;
    ...
}
```

### Task 6: Extend CLI MboxSnapshot

**Files:**
- Modify: `include/hpactor/cli/cli_types.hpp`

- [x] **Step 1: Add overflow fields**

```cpp
uint32_t overflow_depth = 0;
uint32_t overflow_max_depth = 0;
uint64_t overflow_total_pushed = 0;
```

### Task 7: Add policy integration tests

**Files:**
- Modify: `tests/unit/mailbox/test_mailbox_overflow_policies.cpp`

- [x] **Step 1: 6 new test cases**

| Test | Policy | What it validates |
|------|--------|-------------------|
| `RejectNewestRejectsAtCapacity` | RejectNewest | Rejection, retryable, counters |
| `SignalOnlyRejectsWithRetryAfter` | SignalOnly | `retry_after` set from config |
| `SignalOnlySystemMessageReserveNotRejected` | SignalOnly | System msgs bypass policy |
| `SpillToOverflowQueueWhenFull` | SpillToOverflowQueue | `ReroutedToOverflow`, overflow depth counters |
| `SpillToOverflowDrainsOnDequeue` | SpillToOverflowQueue | Drain-back restores message to main queue |
| `OverflowQueueAlwaysAcceptsSpills` | SpillToOverflowQueue | Bounded overflow still accepts (evicts oldest) |

---

### Task 8: Build and verify

- [x] Configure and build: `cmake -S . -B build -GNinja && ninja -C build`
- [x] Run overflow queue tests: `./build/tests/unit/mailbox/test_unit_mailbox --gtest_filter="*Overflow*"`
- [x] Run full test suite: `ctest --output-on-failure --parallel 8` (1077/1077 passed)
- [x] Verify no regressions in existing mailbox tests
