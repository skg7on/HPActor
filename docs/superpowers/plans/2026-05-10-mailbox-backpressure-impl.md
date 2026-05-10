# Mailbox Backpressure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add bounded actor mailboxes, explicit delivery admission results, backpressure signaling, and dead-letter capture so overloaded or unreachable actors cannot cause unbounded memory growth or silent message loss.

**Architecture:** Keep `ActorSystem::deliver_local()` as the source-compatible fire-and-forget wrapper, and add `try_deliver_local()` as the single result-returning local admission boundary. Extend `MPSCActorMailbox<TypedMessage>` with bounded admission and pressure state, add a node-level `DeadLetterQueue`, then layer in config, metrics, CLI snapshots, priority-aware policies, and remote failure capture.

**Tech Stack:** C++20, HPActor actor runtime, lock-free MPSC mailbox, `ActorSystem`, `ActorContext`, `ActorProxy`, TOML topology parser, metrics ring buffer, CLI snapshots, CMake/Ninja tests.

---

## Scope And Sequencing

This plan is intentionally split into independently testable slices:

1. Define shared mailbox/backpressure/dead-letter types.
2. Add bounded FIFO admission to `MPSCActorMailbox`.
3. Route local delivery through `ActorSystem::try_deliver_local()`.
4. Add opt-in `try_send()` APIs.
5. Add the bounded dead-letter queue.
6. Capture local and remote delivery failures as dead letters.
7. Add pressure transitions and local backpressure signal hooks.
8. Add priority-aware lanes, drop policies, and system reserve.
9. Add TOML and runtime configuration.
10. Add metrics, logs, and CLI snapshot fields.
11. Add remote send failure feedback and ingress behavior.
12. Add final stress and regression coverage.

Each task ends with focused tests and a commit. Run the full test suite only at the end or when a task touches shared runtime behavior.

## File Structure

Create:

- `include/hpactor/mailbox/mailbox_policy.hpp` - shared config, admission result, pressure, overflow, and signal types.
- `include/hpactor/mailbox/dead_letter_queue.hpp` - dead-letter record, config, bounded queue, and sink interface.
- `src/mailbox/dead_letter_queue.cpp` - bounded queue implementation.
- `tests/mailbox/test_mailbox_policy.cpp` - default config and helper tests.
- `tests/mailbox/test_bounded_mailbox.cpp` - bounded FIFO mailbox tests.
- `tests/mailbox/test_dead_letter_queue.cpp` - dead-letter queue tests.
- `tests/actor/test_actor_system_backpressure.cpp` - `try_deliver_local()` and actor-system integration tests.
- `tests/actor/test_actor_context_try_send.cpp` - opt-in producer API tests.
- `tests/ref/test_actor_proxy_dead_letters.cpp` - remote route failure capture tests.
- `tests/config/test_mailbox_config.cpp` - TOML mailbox/dead-letter config tests.

Modify:

- `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` - bounded admission, counters, pressure state, priority lanes, snapshot fields.
- `include/hpactor/core/actor_system.hpp` - config structs, `try_deliver_local()`, `dead_letter()`, dead-letter queue ownership.
- `src/actor/actor_system.cpp` - mailbox construction, local delivery admission, dead-letter capture.
- `include/hpactor/actor_context.hpp` and `src/actor/actor_context.cpp` - `try_send()` APIs and backpressure handler registration.
- `include/hpactor/ref/actor_ref.hpp`, `src/ref/actor_ref.cpp`, `include/hpactor/ref/actor_proxy.hpp`, `src/ref/actor_proxy.cpp` - result-returning local/remote send path.
- `include/hpactor/net/transport.hpp`, `include/hpactor/net/tcp_transport.hpp`, `src/net/tcp_transport.cpp`, `include/hpactor/net/connection_pool.hpp`, `src/net/connection_pool.cpp` - optional `try_send()` transport status.
- `include/hpactor/types/types.hpp` - `BackpressureSignalTag` system tag.
- `include/hpactor/config/topology_model.hpp` and `src/config/toml_parser.cpp` - mailbox and dead-letter config parsing.
- `include/hpactor/metrics/metrics_event.hpp`, `include/hpactor/metrics/metrics_aggregator.hpp`, `src/metrics/metrics_aggregator.cpp` - mailbox/dead-letter metrics.
- `include/hpactor/log/log_category.hpp` and its implementation - mailbox pressure and dead-letter log event ids.
- `include/hpactor/cli/cli_types.hpp` and CLI formatters/commands that print mailbox snapshots.
- `CMakeLists.txt` - add `src/mailbox/dead_letter_queue.cpp` to `hpactor_lib`.
- `tests/CMakeLists.txt` - add the new test executables.

---

### Task 1: Shared Mailbox Policy Types

**Files:**
- Create: `include/hpactor/mailbox/mailbox_policy.hpp`
- Create: `tests/mailbox/test_mailbox_policy.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing policy test**

Create `tests/mailbox/test_mailbox_policy.cpp`:

```cpp
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::mailbox;

int main() {
    MailboxConfig cfg;
    assert(cfg.capacity.max_messages == 1024);
    assert(cfg.overflow_policy == OverflowPolicy::RejectNewest);
    assert(cfg.high_watermark == 0.80);
    assert(cfg.low_watermark == 0.50);
    assert(cfg.backpressure_mode == BackpressureMode::LocalAndRemoteSignal);

    EnqueueResult accepted;
    accepted.code = EnqueueResultCode::Accepted;
    assert(accepted.accepted());
    assert(!accepted.retryable());

    EnqueueResult soft;
    soft.code = EnqueueResultCode::AcceptedWithSoftPressure;
    assert(soft.accepted());

    EnqueueResult rejected;
    rejected.code = EnqueueResultCode::Rejected;
    rejected.retry_after = std::chrono::milliseconds{5};
    assert(!rejected.accepted());
    assert(rejected.retryable());

    TypedMessage user_msg(TypeTag::User, StreamBuffer{1, 2, 3, 4});
    assert(estimate_message_bytes(user_msg) >= sizeof(TypedMessage) + 4);
    assert(is_system_message(TypeTag::DownMsg));
    assert(!is_system_message(TypeTag::User));

    return 0;
}
```

- [ ] **Step 2: Register and run the failing test**

Modify the mailbox section of `tests/CMakeLists.txt`:

```cmake
add_executable(test_mailbox_policy mailbox/test_mailbox_policy.cpp)
target_link_libraries(test_mailbox_policy hpactor)
add_test(NAME test_mailbox_policy COMMAND test_mailbox_policy)
```

Run:

```bash
cmake -S . -B build -GNinja
ninja -C build test_mailbox_policy
./build/tests/test_mailbox_policy
```

Expected: compile fails because `hpactor/mailbox/mailbox_policy.hpp` does not exist.

- [ ] **Step 3: Add the policy header**

Create `include/hpactor/mailbox/mailbox_policy.hpp`:

```cpp
#pragma once

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>

namespace hpactor::mailbox {

struct MailboxCapacity {
    uint32_t max_messages = 1024;
    uint64_t max_bytes = 0;
};

enum class OverflowPolicy : uint8_t {
    RejectNewest,
    DropNewest,
    DropOldest,
    DropLowestPriority,
    DeadLetter,
    SpillToOverflowQueue,
    SignalOnly,
    BlockWhenAllowed,
};

enum class BackpressureMode : uint8_t {
    Disabled,
    LocalSignal,
    RemoteSignal,
    LocalAndRemoteSignal,
};

enum class MailboxPressureState : uint8_t {
    Normal,
    SoftPressure,
    HardPressure,
    Recovering,
};

struct MailboxConfig {
    MailboxCapacity capacity;
    uint8_t priority_levels = 4;
    OverflowPolicy overflow_policy = OverflowPolicy::RejectNewest;
    BackpressureMode backpressure_mode = BackpressureMode::LocalAndRemoteSignal;
    double high_watermark = 0.80;
    double low_watermark = 0.50;
    uint32_t protected_system_messages = 32;
    uint32_t max_overflow_depth = 0;
    uint32_t signal_min_interval_ms = 100;
    bool priority_aware = false;
    bool enable_dead_letters = true;
};

struct MessagePriority {
    uint8_t value = 0;
};

struct DeliveryOptions {
    bool no_drop = false;
    bool allow_blocking = false;
    bool emit_backpressure = true;
    uint64_t message_id = 0;
    uint32_t flags = 0;
};

struct MailboxEnvelopeMeta {
    ActorAddress sender;
    TypeTag type_tag = TypeTag::Invalid;
    uint64_t message_id = 0;
    uint8_t priority = 0;
    int64_t deadline_ns = INT64_MAX;
    uint32_t flags = 0;
    uint64_t estimated_bytes = 0;
    uint64_t sequence = 0;
};

enum class EnqueueResultCode : uint8_t {
    Accepted,
    AcceptedWithSoftPressure,
    Rejected,
    DroppedNewest,
    DroppedExisting,
    ReroutedToDeadLetter,
    ReroutedToOverflow,
    MailboxClosed,
    ActorNotFound,
};

struct EnqueueResult {
    EnqueueResultCode code = EnqueueResultCode::Accepted;
    ActorId target;
    uint32_t depth = 0;
    uint32_t capacity = 0;
    double pressure_ratio = 0.0;
    std::chrono::milliseconds retry_after{0};
    TypeTag affected_type = TypeTag::Invalid;
    uint64_t affected_message_id = 0;

    bool accepted() const noexcept {
        return code == EnqueueResultCode::Accepted ||
               code == EnqueueResultCode::AcceptedWithSoftPressure;
    }

    bool retryable() const noexcept {
        return code == EnqueueResultCode::Rejected ||
               code == EnqueueResultCode::MailboxClosed ||
               code == EnqueueResultCode::ReroutedToOverflow;
    }
};

enum class BackpressureReason : uint8_t {
    HighWatermark,
    HardCapacity,
    ByteCapacity,
    OverflowPolicy,
    NodeMemoryPressure,
};

struct BackpressureSignal {
    ActorAddress target;
    ActorAddress sender;
    BackpressureReason reason = BackpressureReason::HighWatermark;
    uint32_t depth = 0;
    uint32_t capacity = 0;
    uint64_t bytes = 0;
    uint64_t byte_capacity = 0;
    double pressure_ratio = 0.0;
    std::chrono::milliseconds retry_after{0};
    uint64_t sequence = 0;
};

inline bool is_system_message(TypeTag tag) noexcept {
    return static_cast<uint32_t>(tag) > 0 &&
           static_cast<uint32_t>(tag) < static_cast<uint32_t>(TypeTag::User);
}

inline uint64_t estimate_message_bytes(const TypedMessage& msg) noexcept {
    return sizeof(TypedMessage) + msg.payload().size();
}

inline const char* to_string(OverflowPolicy policy) noexcept {
    switch (policy) {
    case OverflowPolicy::RejectNewest: return "reject_newest";
    case OverflowPolicy::DropNewest: return "drop_newest";
    case OverflowPolicy::DropOldest: return "drop_oldest";
    case OverflowPolicy::DropLowestPriority: return "drop_lowest_priority";
    case OverflowPolicy::DeadLetter: return "dead_letter";
    case OverflowPolicy::SpillToOverflowQueue: return "spill_to_overflow_queue";
    case OverflowPolicy::SignalOnly: return "signal_only";
    case OverflowPolicy::BlockWhenAllowed: return "block_when_allowed";
    }
    return "reject_newest";
}

inline const char* to_string(MailboxPressureState state) noexcept {
    switch (state) {
    case MailboxPressureState::Normal: return "normal";
    case MailboxPressureState::SoftPressure: return "soft_pressure";
    case MailboxPressureState::HardPressure: return "hard_pressure";
    case MailboxPressureState::Recovering: return "recovering";
    }
    return "normal";
}

} // namespace hpactor::mailbox
```

- [ ] **Step 4: Run the test**

Run:

```bash
ninja -C build test_mailbox_policy
./build/tests/test_mailbox_policy
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/mailbox/mailbox_policy.hpp tests/mailbox/test_mailbox_policy.cpp tests/CMakeLists.txt
git commit -m "feat: add mailbox policy types"
```

---

### Task 2: Bounded FIFO Mailbox Admission

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`
- Create: `tests/mailbox/test_bounded_mailbox.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing bounded mailbox test**

Create `tests/mailbox/test_bounded_mailbox.cpp`:

```cpp
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <cassert>
#include <atomic>

struct MockScheduler : public hpactor::sched::IScheduler {
    void start() override {}
    void stop() override {}
    void notify_ready(hpactor::ActorId actor, uint8_t priority, int64_t deadline) override {
        last_actor = actor;
        last_priority = priority;
        last_deadline = deadline;
        notify_ready_count.fetch_add(1, std::memory_order_relaxed);
    }
    void notify_idle(hpactor::ActorId) override {}
    void yield(hpactor::ActorId actor, uint8_t priority) override {
        notify_ready(actor, priority, INT64_MAX);
    }
    hpactor::sched::TimerHandle schedule_after(hpactor::sched::timer_callback, int64_t) override {
        return {};
    }
    hpactor::sched::TimerHandle schedule_every(hpactor::sched::timer_callback, int64_t) override {
        return {};
    }
    void cancel_timer(hpactor::sched::TimerHandle) override {}
    size_t worker_count() const override { return 1; }
    bool is_running() const override { return true; }
    void register_dedicated_thread(hpactor::ActorId, int) override {}
    void register_dedicated_pool(hpactor::ActorId, uint32_t) override {}
    void unregister_dedicated(hpactor::ActorId) override {}

    std::atomic<int> notify_ready_count{0};
    hpactor::ActorId last_actor{};
    uint8_t last_priority = 255;
    int64_t last_deadline = 0;
};

int main() {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    MockScheduler scheduler;
    MailboxConfig cfg;
    cfg.capacity.max_messages = 2;
    cfg.high_watermark = 0.50;
    cfg.low_watermark = 0.25;

    MPSCActorMailbox<TypedMessage> mb(ActorId{77}, &scheduler, cfg);

    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::User;
    meta.priority = 2;
    meta.deadline_ns = 1234;

    auto r1 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{1}), meta);
    assert(r1.accepted());
    assert(r1.depth == 1);
    assert(scheduler.notify_ready_count.load() == 1);
    assert(scheduler.last_priority == 2);
    assert(scheduler.last_deadline == 1234);

    auto r2 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}), meta);
    assert(r2.accepted());
    assert(r2.depth == 2);

    auto r3 = mb.try_push(TypedMessage(TypeTag::User, StreamBuffer{3}), meta);
    assert(!r3.accepted());
    assert(r3.code == EnqueueResultCode::Rejected);
    assert(r3.capacity == 2);

    TypedMessage out;
    assert(mb.try_pop(out));
    assert(out.payload()[0] == 1);
    assert(mb.try_pop(out));
    assert(out.payload()[0] == 2);
    assert(!mb.try_pop(out));

    auto s = mb.snapshot();
    assert(s.depth == 0);
    assert(s.capacity == 2);
    assert(s.total_enqueued == 2);
    assert(s.total_rejected == 1);

    return 0;
}
```

- [ ] **Step 2: Register and run the failing test**

Add to `tests/CMakeLists.txt` near the mailbox tests:

```cmake
add_executable(test_bounded_mailbox mailbox/test_bounded_mailbox.cpp)
target_link_libraries(test_bounded_mailbox hpactor)
add_test(NAME test_bounded_mailbox COMMAND test_bounded_mailbox)
```

Run:

```bash
ninja -C build test_bounded_mailbox
./build/tests/test_bounded_mailbox
```

Expected: compile fails because `MPSCActorMailbox` has no config constructor and no `try_push()`.

- [ ] **Step 3: Extend `cli::MboxSnapshot`**

Modify `include/hpactor/cli/cli_types.hpp`:

```cpp
struct MboxSnapshot {
    uint32_t depth = 0;
    uint32_t capacity = 0;
    uint64_t queued_bytes = 0;
    uint64_t byte_capacity = 0;
    uint32_t pressure_ratio_ppm = 0;
    uint64_t total_enqueued = 0;
    uint64_t total_dequeued = 0;
    uint64_t total_rejected = 0;
    uint64_t total_dropped = 0;
    uint64_t total_dead_letters = 0;
    uint64_t max_depth = 0;
    uint32_t high_priority_depth = 0;
    std::string pressure_state;
    std::string overflow_policy;
};
```

Use parts-per-million for `pressure_ratio_ppm` so snapshots remain plain integer data and do not introduce floating point formatting into CLI transport structs.

- [ ] **Step 4: Add bounded admission fields and constructor**

Modify `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`:

```cpp
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <type_traits>
```

Replace the constructor with:

```cpp
MPSCActorMailbox(ActorId actor_id, sched::IScheduler* scheduler,
                 MailboxConfig config = {}) noexcept
    : actor_id_(actor_id), scheduler_(scheduler), config_(config) {
    if (config_.capacity.max_messages == 0) {
        config_.capacity.max_messages = 1024;
    }
}
```

Add these public methods before `enqueue(T* node)`:

```cpp
void set_config(const MailboxConfig& config) noexcept {
    config_ = config;
    if (config_.capacity.max_messages == 0) {
        config_.capacity.max_messages = 1024;
    }
}

const MailboxConfig& config() const noexcept {
    return config_;
}

EnqueueResult try_push(T&& msg, MailboxEnvelopeMeta meta = {}) noexcept {
    if (meta.estimated_bytes == 0) {
        if constexpr (std::is_same_v<T, TypedMessage>) {
            meta.estimated_bytes = estimate_message_bytes(msg);
        } else {
            meta.estimated_bytes = sizeof(T);
        }
    }

    EnqueueResult rejected = make_result(EnqueueResultCode::Rejected);
    if (!try_reserve(meta.estimated_bytes)) {
        total_rejected_.fetch_add(1, std::memory_order_relaxed);
        return rejected;
    }

    void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(T), actor_id_);
    auto* node = new (raw) T(std::move(msg));
    enqueue_reserved(node, meta);
    return make_result(pressure_code_after_accept());
}
```

Add these private helpers:

```cpp
bool try_reserve(uint64_t bytes) noexcept {
    for (;;) {
        uint32_t cur = reserved_messages_.load(std::memory_order_acquire);
        if (cur >= config_.capacity.max_messages) {
            return false;
        }
        if (reserved_messages_.compare_exchange_weak(
                cur, cur + 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (config_.capacity.max_bytes > 0) {
                queued_bytes_.fetch_add(bytes, std::memory_order_acq_rel);
            }
            return true;
        }
    }
}

void release_reservation(uint64_t bytes) noexcept {
    reserved_messages_.fetch_sub(1, std::memory_order_acq_rel);
    if (config_.capacity.max_bytes > 0) {
        queued_bytes_.fetch_sub(bytes, std::memory_order_acq_rel);
    }
}

EnqueueResultCode pressure_code_after_accept() noexcept {
    auto depth = reserved_messages_.load(std::memory_order_acquire);
    const auto cap = config_.capacity.max_messages;
    const double ratio = cap == 0 ? 0.0 : static_cast<double>(depth) / cap;
    if (ratio >= 1.0) {
        pressure_state_.store(MailboxPressureState::HardPressure,
                              std::memory_order_release);
        return EnqueueResultCode::AcceptedWithSoftPressure;
    }
    if (ratio >= config_.high_watermark) {
        pressure_state_.store(MailboxPressureState::SoftPressure,
                              std::memory_order_release);
        return EnqueueResultCode::AcceptedWithSoftPressure;
    }
    return EnqueueResultCode::Accepted;
}

EnqueueResult make_result(EnqueueResultCode code) const noexcept {
    EnqueueResult r;
    r.code = code;
    r.target = actor_id_;
    r.depth = reserved_messages_.load(std::memory_order_acquire);
    r.capacity = config_.capacity.max_messages;
    r.pressure_ratio = r.capacity == 0
        ? 0.0
        : static_cast<double>(r.depth) / static_cast<double>(r.capacity);
    if (code == EnqueueResultCode::Rejected) {
        r.retry_after = std::chrono::milliseconds{config_.signal_min_interval_ms};
    }
    return r;
}
```

Add fields:

```cpp
MailboxConfig config_;
std::atomic<uint32_t> reserved_messages_{0};
std::atomic<uint64_t> queued_bytes_{0};
std::atomic<uint64_t> total_enqueued_{0};
std::atomic<uint64_t> total_dequeued_{0};
std::atomic<uint64_t> total_rejected_{0};
std::atomic<uint64_t> total_dropped_{0};
std::atomic<uint64_t> total_dead_letters_{0};
std::atomic<uint64_t> max_depth_{0};
std::atomic<MailboxPressureState> pressure_state_{MailboxPressureState::Normal};
```

- [ ] **Step 5: Split reserved enqueue from legacy raw enqueue**

In `MPSCActorMailbox`, keep `enqueue(T* node)` for existing low-level tests, but route normal production through `try_push()`:

```cpp
void enqueue_reserved(T* node, const MailboxEnvelopeMeta& meta) noexcept {
    bool was_empty = empty();
    mailbox_.enqueue(node);

    uint32_t depth = reserved_messages_.load(std::memory_order_acquire);
    total_enqueued_.fetch_add(1, std::memory_order_relaxed);
    update_max_depth(depth);
    emit_metric(metrics::MetricEventType::kMailboxEnqueue, 1);

    if (depth > config_.capacity.max_messages) [[unlikely]] {
        HPACTOR_LOG_WARNING(
            log::LogCategory::kMailbox, actor_id_,
            static_cast<uint32_t>(log::LogEventId::kMailboxDepthHigh),
            "mailbox depth high",
            log::field("depth", static_cast<uint64_t>(depth)));
    }

    if (was_empty) {
        bool expected = true;
        if (mailbox_was_empty_.compare_exchange_strong(
                expected, false, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            if (continuation_callback_) {
                continuation_callback_();
            }
            scheduler_->notify_ready(actor_id_, meta.priority, meta.deadline_ns);
        }
    }
}

void enqueue(T* node) noexcept {
    MailboxEnvelopeMeta meta;
    if (try_reserve(sizeof(T))) {
        enqueue_reserved(node, meta);
    }
}

void push(T&& msg) noexcept {
    (void)try_push(std::move(msg));
}

void update_max_depth(uint32_t depth) noexcept {
    uint64_t cur = max_depth_.load(std::memory_order_relaxed);
    while (depth > cur &&
           !max_depth_.compare_exchange_weak(cur, depth,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
    }
}

void emit_metric(metrics::MetricEventType type, uint32_t value) noexcept {
    if (metrics_ring_buffer_) [[unlikely]] {
        metrics::MetricEvent evt{};
        evt.actor_id = actor_id_;
        evt.event_type = type;
        evt.value_hi = value;
        metrics_ring_buffer_->try_push(evt);
    }
}
```

- [ ] **Step 6: Update dequeue and snapshot**

In `dequeue()` after a successful node:

```cpp
if (node != nullptr) {
    uint64_t bytes = sizeof(T);
    if constexpr (std::is_same_v<T, TypedMessage>) {
        bytes = estimate_message_bytes(*node);
    }
    release_reservation(bytes);
    total_dequeued_.fetch_add(1, std::memory_order_relaxed);
}
```

Replace `snapshot()` with:

```cpp
cli::MboxSnapshot snapshot() const {
    cli::MboxSnapshot s;
    s.depth = reserved_messages_.load(std::memory_order_acquire);
    s.capacity = config_.capacity.max_messages;
    s.queued_bytes = queued_bytes_.load(std::memory_order_acquire);
    s.byte_capacity = config_.capacity.max_bytes;
    s.pressure_ratio_ppm = s.capacity == 0
        ? 0
        : static_cast<uint32_t>((static_cast<uint64_t>(s.depth) * 1000000ULL) /
                                static_cast<uint64_t>(s.capacity));
    s.total_enqueued = total_enqueued_.load(std::memory_order_acquire);
    s.total_dequeued = total_dequeued_.load(std::memory_order_acquire);
    s.total_rejected = total_rejected_.load(std::memory_order_acquire);
    s.total_dropped = total_dropped_.load(std::memory_order_acquire);
    s.total_dead_letters = total_dead_letters_.load(std::memory_order_acquire);
    s.max_depth = max_depth_.load(std::memory_order_acquire);
    s.pressure_state =
        to_string(pressure_state_.load(std::memory_order_acquire));
    s.overflow_policy = to_string(config_.overflow_policy);
    return s;
}
```

- [ ] **Step 7: Run bounded mailbox tests**

Run:

```bash
ninja -C build test_bounded_mailbox test_mpsc_actor_mailbox
./build/tests/test_bounded_mailbox
./build/tests/test_mpsc_actor_mailbox
```

Expected: both tests pass.

- [ ] **Step 8: Commit**

```bash
git add include/hpactor/cli/cli_types.hpp include/hpactor/mailbox/mpsc_actor_mailbox.hpp tests/mailbox/test_bounded_mailbox.cpp tests/CMakeLists.txt
git commit -m "feat: add bounded mailbox admission"
```

---

### Task 3: ActorSystem Local Admission Boundary

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `include/hpactor/actor/abstract_actor.hpp`
- Modify: `include/hpactor/actor/event_based_actor.hpp`
- Create: `tests/actor/test_actor_system_backpressure.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing ActorSystem test**

Create `tests/actor/test_actor_system_backpressure.cpp`:

```cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.mailbox.default_capacity = 1;

    ActorSystem system(cfg);
    auto actor = system.spawn<EventBasedActor>();

    auto ok = system.try_deliver_local(actor.id(),
        TypedMessage(TypeTag::User, StreamBuffer{1}));
    assert(ok.accepted());

    auto full = system.try_deliver_local(actor.id(),
        TypedMessage(TypeTag::User, StreamBuffer{2}));
    assert(!full.accepted());
    assert(full.code == mailbox::EnqueueResultCode::Rejected);

    auto missing = system.try_deliver_local(ActorId{99999},
        TypedMessage(TypeTag::User, StreamBuffer{3}));
    assert(!missing.accepted());
    assert(missing.code == mailbox::EnqueueResultCode::ActorNotFound);

    TypedMessage popped;
    auto* mailbox = system.get_mailbox(actor.id());
    assert(mailbox != nullptr);
    assert(mailbox->try_pop(popped));
    assert(popped.payload()[0] == 1);

    return 0;
}
```

- [ ] **Step 2: Register and run the failing test**

Add to actor tests in `tests/CMakeLists.txt`:

```cmake
add_executable(test_actor_system_backpressure actor/test_actor_system_backpressure.cpp)
target_link_libraries(test_actor_system_backpressure hpactor)
add_test(NAME test_actor_system_backpressure COMMAND test_actor_system_backpressure)
```

Run:

```bash
ninja -C build test_actor_system_backpressure
./build/tests/test_actor_system_backpressure
```

Expected: compile fails because `Config::mailbox` and `try_deliver_local()` do not exist.

- [ ] **Step 3: Add runtime mailbox defaults**

Modify `include/hpactor/core/actor_system.hpp` includes:

```cpp
#include <hpactor/mailbox/mailbox_policy.hpp>
```

Add before `struct Config`:

```cpp
struct MailboxDefaults {
    uint32_t default_capacity = 1024;
    uint64_t default_byte_capacity = 0;
    mailbox::OverflowPolicy default_policy = mailbox::OverflowPolicy::RejectNewest;
    double high_watermark = 0.80;
    double low_watermark = 0.50;
    uint32_t protected_system_messages = 32;
    mailbox::BackpressureMode backpressure_mode =
        mailbox::BackpressureMode::LocalAndRemoteSignal;
};
```

Add inside `Config`:

```cpp
MailboxDefaults mailbox;
```

- [ ] **Step 4: Add public ActorSystem admission API**

Add declarations in `ActorSystem`:

```cpp
mailbox::EnqueueResult
try_deliver_local(ActorId target, TypedMessage msg, uint8_t priority = 0,
                  int64_t deadline_ns = INT64_MAX,
                  mailbox::DeliveryOptions options = {});

mailbox::MailboxConfig mailbox_config_for_spawn() const;
mailbox::MailboxConfig mailbox_config_for_actor_def(
    const config::ActorDef& def) const;
```

- [ ] **Step 5: Implement mailbox config helpers**

Add to `src/actor/actor_system.cpp`:

```cpp
mailbox::MailboxConfig ActorSystem::mailbox_config_for_spawn() const {
    mailbox::MailboxConfig cfg;
    cfg.capacity.max_messages = static_cast<uint32_t>(
        config_.mailbox.default_capacity != 0
            ? config_.mailbox.default_capacity
            : config_.max_queue_depth);
    cfg.capacity.max_bytes = config_.mailbox.default_byte_capacity;
    cfg.overflow_policy = config_.mailbox.default_policy;
    cfg.high_watermark = config_.mailbox.high_watermark;
    cfg.low_watermark = config_.mailbox.low_watermark;
    cfg.protected_system_messages = config_.mailbox.protected_system_messages;
    cfg.backpressure_mode = config_.mailbox.backpressure_mode;
    return cfg;
}

mailbox::MailboxConfig
ActorSystem::mailbox_config_for_actor_def(const config::ActorDef& def) const {
    auto cfg = mailbox_config_for_spawn();
    if (def.mailbox_capacity != 0) {
        cfg.capacity.max_messages = def.mailbox_capacity;
    }
    return cfg;
}
```

- [ ] **Step 6: Use config during mailbox construction**

In template `ActorSystem::spawn()` in `include/hpactor/core/actor_system.hpp`, replace mailbox creation with:

```cpp
auto mailbox_config = mailbox_config_for_spawn();
mailboxes_.emplace(
    id, std::make_unique<mailbox::MPSCActorMailbox<TypedMessage>>(
            id, scheduler_.get(), mailbox_config));
```

In `ActorSystem::spawn_configured()` in `src/actor/actor_system.cpp`, replace mailbox creation with:

```cpp
auto mailbox_config = mailbox_config_for_actor_def(def);
mailboxes_.emplace(
    id, std::make_unique<mailbox::MPSCActorMailbox<TypedMessage>>(
            id, scheduler_.get(), mailbox_config));
```

For `SpawnReceiverId`, call `mailbox_config_for_spawn()`.

- [ ] **Step 7: Implement `try_deliver_local()`**

Replace the current priority overload in `src/actor/actor_system.cpp` with:

```cpp
mailbox::EnqueueResult
ActorSystem::try_deliver_local(ActorId target, TypedMessage msg,
                               uint8_t priority, int64_t deadline_ns,
                               mailbox::DeliveryOptions options) {
    auto* mailbox = get_mailbox(target);
    if (mailbox == nullptr) {
        mailbox::EnqueueResult r;
        r.code = mailbox::EnqueueResultCode::ActorNotFound;
        r.target = target;
        return r;
    }

    mailbox::MailboxEnvelopeMeta meta;
    meta.sender = msg.sender_address();
    meta.type_tag = msg.type_id();
    meta.priority = priority;
    meta.deadline_ns = deadline_ns;
    meta.flags = options.flags;
    meta.message_id = options.message_id;
    if (options.no_drop) {
        meta.flags |= net::WireFrame::NoDrop;
    }

    return mailbox->try_push(std::move(msg), meta);
}

void ActorSystem::deliver_local(ActorId target, TypedMessage msg,
                                uint8_t priority, int64_t deadline_ns) {
    (void)try_deliver_local(target, std::move(msg), priority, deadline_ns, {});
}
```

- [ ] **Step 8: Run ActorSystem tests**

Run:

```bash
ninja -C build test_actor_system_backpressure test_actor_system test_actor_context
./build/tests/test_actor_system_backpressure
./build/tests/test_actor_system
./build/tests/test_actor_context
```

Expected: all pass.

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/actor/test_actor_system_backpressure.cpp tests/CMakeLists.txt
git commit -m "feat: route local delivery through mailbox admission"
```

---

### Task 4: Opt-In Producer APIs

**Files:**
- Modify: `include/hpactor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`
- Modify: `include/hpactor/ref/actor_ref.hpp`
- Modify: `src/ref/actor_ref.cpp`
- Modify: `include/hpactor/ref/actor_proxy.hpp`
- Modify: `src/ref/actor_proxy.cpp`
- Create: `tests/actor/test_actor_context_try_send.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing `try_send()` test**

Create `tests/actor/test_actor_context_try_send.cpp`:

```cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.mailbox.default_capacity = 1;

    ActorSystem system(cfg);
    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();
    ActorContext ctx(sender, &system);

    auto ok = ctx.try_send(target.address(),
        TypedMessage(TypeTag::User, StreamBuffer{1}));
    assert(ok.accepted());

    auto full = ctx.try_send(target.address(),
        TypedMessage(TypeTag::User, StreamBuffer{2}));
    assert(!full.accepted());

    auto missing_addr = target.address();
    missing_addr.id = ActorId{99999};
    auto missing = ctx.try_send(missing_addr,
        TypedMessage(TypeTag::User, StreamBuffer{3}));
    assert(missing.code == mailbox::EnqueueResultCode::ActorNotFound);

    return 0;
}
```

- [ ] **Step 2: Register and run the failing test**

Add to actor tests:

```cmake
add_executable(test_actor_context_try_send actor/test_actor_context_try_send.cpp)
target_link_libraries(test_actor_context_try_send hpactor)
add_test(NAME test_actor_context_try_send COMMAND test_actor_context_try_send)
```

Run:

```bash
ninja -C build test_actor_context_try_send
./build/tests/test_actor_context_try_send
```

Expected: compile fails because `ActorContext::try_send()` does not exist.

- [ ] **Step 3: Add result-returning send methods**

In `include/hpactor/ref/actor_ref.hpp`, add:

```cpp
mailbox::EnqueueResult try_send(const ActorAddress& target, TypedMessage msg,
                                mailbox::DeliveryOptions options = {});
```

In `src/ref/actor_ref.cpp`, implement:

```cpp
mailbox::EnqueueResult
ActorRef::try_send(const ActorAddress& target, TypedMessage msg,
                   mailbox::DeliveryOptions options) {
    if (is_local()) {
        Actor* actor = get_actor();
        if (actor != nullptr) {
            return actor->get()->system().try_deliver_local(
                target.id, std::move(msg), 0, INT64_MAX, options);
        }
    } else {
        ActorProxy* proxy = get_proxy();
        if (proxy != nullptr) {
            return proxy->try_send(target, std::move(msg), options);
        }
    }

    mailbox::EnqueueResult r;
    r.code = mailbox::EnqueueResultCode::ActorNotFound;
    r.target = target.id;
    return r;
}
```

Keep existing `ActorRef::send()` as:

```cpp
void ActorRef::send(const ActorAddress& target, TypedMessage msg) {
    (void)try_send(target, std::move(msg), {});
}
```

- [ ] **Step 4: Add `ActorProxy::try_send()`**

In `include/hpactor/ref/actor_proxy.hpp`, include `mailbox_policy.hpp`, add an `ActorSystem* system_` field, and declare:

```cpp
mailbox::EnqueueResult try_send(const ActorAddress& target, TypedMessage msg,
                                mailbox::DeliveryOptions options = {});
```

In `src/ref/actor_proxy.cpp`, set `system_` in constructors and implement:

```cpp
mailbox::EnqueueResult
ActorProxy::try_send(const ActorAddress& target, TypedMessage msg,
                     mailbox::DeliveryOptions options) {
    mailbox::EnqueueResult result;
    result.target = target.id;

    if (transport_ == nullptr) {
        result.code = mailbox::EnqueueResultCode::ActorNotFound;
        return result;
    }

    ActorAddress resolved_target = target;
    if (location_cache_) {
        auto cached = location_cache_->get(target.id);
        if (cached) {
            resolved_target.endpoint = *cached;
        }
    }

    if (discovery_) {
        auto* member = discovery_->discover(resolved_target.endpoint);
        if (!member) {
            result.code = mailbox::EnqueueResultCode::ActorNotFound;
            return result;
        }
        resolved_target.endpoint = member->endpoint;
        if (location_cache_) {
            location_cache_->put(target.id, resolved_target.endpoint);
        }
    }

    net::WireFrame frame;
    const auto& sender_addr = msg.sender_address().id != ActorId{0}
        ? msg.sender_address()
        : address_;
    net::to_proto(frame.pb_frame.mutable_sender(), sender_addr);
    net::to_proto(frame.pb_frame.mutable_receiver(), resolved_target);
    frame.pb_frame.set_message_id(options.message_id != 0
        ? options.message_id
        : MessageId::generate().value());
    frame.pb_frame.set_type_tag(static_cast<uint32_t>(msg.type_id()));
    frame.pb_frame.set_flags(options.flags);
    frame.pb_frame.set_payload(
        reinterpret_cast<const char*>(msg.payload().data()),
        msg.payload().size());

    transport_->send(resolved_target, frame.encode());
    result.code = mailbox::EnqueueResultCode::Accepted;
    return result;
}
```

Keep `ActorProxy::send(const ActorAddress&, TypedMessage)` as a wrapper that calls `try_send(target, std::move(msg), {})`.

- [ ] **Step 5: Add `ActorContext::try_send()`**

In `include/hpactor/actor_context.hpp`, declare:

```cpp
mailbox::EnqueueResult try_send(const ActorAddress& target, TypedMessage msg,
                                mailbox::DeliveryOptions options = {});

mailbox::EnqueueResult try_send_with_priority(const ActorAddress& target,
                                              TypedMessage msg,
                                              uint8_t priority,
                                              int64_t deadline_ns,
                                              mailbox::DeliveryOptions options = {});
```

In `src/actor/actor_context.cpp`, implement:

```cpp
mailbox::EnqueueResult
ActorContext::try_send(const ActorAddress& target, TypedMessage msg,
                       mailbox::DeliveryOptions options) {
    auto ref = resolve(target);
    if (!ref) {
        mailbox::EnqueueResult r;
        r.code = mailbox::EnqueueResultCode::ActorNotFound;
        r.target = target.id;
        return r;
    }

    if (owner_) {
        msg.set_sender_address(owner_.address());
    }
    return ref.try_send(target, std::move(msg), options);
}

mailbox::EnqueueResult
ActorContext::try_send_with_priority(const ActorAddress& target, TypedMessage msg,
                                     uint8_t priority, int64_t deadline_ns,
                                     mailbox::DeliveryOptions options) {
    auto ref = resolve(target);
    if (!ref) {
        mailbox::EnqueueResult r;
        r.code = mailbox::EnqueueResultCode::ActorNotFound;
        r.target = target.id;
        return r;
    }

    if (owner_) {
        msg.set_sender_address(owner_.address());
    }

    if (ref.is_local()) {
        auto* system = owner_ ? &owner_.get()->system() : system_;
        if (system != nullptr) {
            return system->try_deliver_local(
                target.id, std::move(msg), priority, deadline_ns, options);
        }
    }

    return ref.try_send(target, std::move(msg), options);
}
```

Change existing `send()` methods to call these wrappers and discard the result.

- [ ] **Step 6: Run producer API tests**

Run:

```bash
ninja -C build test_actor_context_try_send test_actor_context test_unified_message_passing
./build/tests/test_actor_context_try_send
./build/tests/test_actor_context
./build/tests/test_unified_message_passing
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/actor_context.hpp src/actor/actor_context.cpp include/hpactor/ref/actor_ref.hpp src/ref/actor_ref.cpp include/hpactor/ref/actor_proxy.hpp src/ref/actor_proxy.cpp tests/actor/test_actor_context_try_send.cpp tests/CMakeLists.txt
git commit -m "feat: add result-returning actor send APIs"
```

---

### Task 5: Bounded Dead-Letter Queue

**Files:**
- Create: `include/hpactor/mailbox/dead_letter_queue.hpp`
- Create: `src/mailbox/dead_letter_queue.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/mailbox/test_dead_letter_queue.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing dead-letter queue test**

Create `tests/mailbox/test_dead_letter_queue.cpp`:

```cpp
#include <hpactor/mailbox/dead_letter_queue.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::mailbox;

int main() {
    DeadLetterConfig cfg;
    cfg.capacity = 2;
    cfg.max_payload_sample_bytes = 3;
    cfg.overflow_policy = DeadLetterOverflowPolicy::DropOldestRecord;

    DeadLetterQueue q(cfg);

    DeadLetterRecord a;
    a.reason = DeadLetterReason::ActorNotFound;
    a.source = DeadLetterSource::LocalDelivery;
    a.message_id = 1;
    a.payload_sample = StreamBuffer{1, 2, 3, 4, 5};
    assert(q.try_push(std::move(a)));

    DeadLetterRecord b;
    b.reason = DeadLetterReason::MissingRoute;
    b.source = DeadLetterSource::ActorProxy;
    b.message_id = 2;
    assert(q.try_push(std::move(b)));

    DeadLetterRecord c;
    c.reason = DeadLetterReason::NetworkPartition;
    c.source = DeadLetterSource::Transport;
    c.message_id = 3;
    assert(q.try_push(std::move(c)));

    auto snap = q.snapshot();
    assert(snap.depth == 2);
    assert(snap.total_pushed == 3);
    assert(snap.total_lost == 1);

    DeadLetterRecord out;
    assert(q.try_pop(out));
    assert(out.message_id == 2);
    assert(q.try_pop(out));
    assert(out.message_id == 3);
    assert(!q.try_pop(out));

    return 0;
}
```

- [ ] **Step 2: Register and run the failing test**

Add to mailbox tests:

```cmake
add_executable(test_dead_letter_queue mailbox/test_dead_letter_queue.cpp)
target_link_libraries(test_dead_letter_queue hpactor)
add_test(NAME test_dead_letter_queue COMMAND test_dead_letter_queue)
```

Add to root `CMakeLists.txt` library sources:

```cmake
    src/mailbox/dead_letter_queue.cpp
```

Run:

```bash
ninja -C build test_dead_letter_queue
./build/tests/test_dead_letter_queue
```

Expected: compile fails because the dead-letter queue header does not exist.

- [ ] **Step 3: Add the header**

Create `include/hpactor/mailbox/dead_letter_queue.hpp`:

```cpp
#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <deque>
#include <mutex>

namespace hpactor::mailbox {

enum class DeadLetterReason : uint8_t {
    MailboxFull,
    MailboxClosed,
    ActorNotFound,
    ActorTerminated,
    MissingRoute,
    RemoteNodeUnreachable,
    NetworkPartition,
    TransportSendFailed,
    DecodeFailed,
    OverflowPolicy,
    NoDropRejected,
};

enum class DeadLetterSource : uint8_t {
    LocalDelivery,
    RemoteDelivery,
    ActorProxy,
    Transport,
    MailboxAdmission,
    ServiceDiscovery,
    Replay,
};

enum class DeadLetterOverflowPolicy : uint8_t {
    DropOldestRecord,
    DropNewestRecord,
    MetadataOnly,
};

struct DeadLetterConfig {
    bool enabled = true;
    uint32_t capacity = 4096;
    uint64_t byte_capacity = 0;
    uint32_t max_payload_sample_bytes = 512;
    DeadLetterOverflowPolicy overflow_policy =
        DeadLetterOverflowPolicy::DropOldestRecord;
    bool store_payload = true;
    bool alert_on_first_failure = false;
    uint32_t alert_threshold_per_minute = 100;
};

struct DeadLetterRecord {
    DeadLetterReason reason = DeadLetterReason::ActorNotFound;
    DeadLetterSource source = DeadLetterSource::LocalDelivery;
    ActorAddress sender;
    ActorAddress target;
    TypeTag type_tag = TypeTag::Invalid;
    uint64_t message_id = 0;
    uint32_t frame_flags = 0;
    uint8_t priority = 0;
    int64_t deadline_ns = INT64_MAX;
    uint64_t trace_id_hi = 0;
    uint64_t trace_id_lo = 0;
    uint64_t span_id = 0;
    uint32_t payload_size = 0;
    StreamBuffer payload_sample;
    uint32_t mailbox_depth = 0;
    uint32_t mailbox_capacity = 0;
    uint64_t timestamp_ns = 0;
};

struct DeadLetterQueueSnapshot {
    uint32_t depth = 0;
    uint32_t capacity = 0;
    uint64_t total_pushed = 0;
    uint64_t total_popped = 0;
    uint64_t total_lost = 0;
};

class IDeadLetterSink {
public:
    virtual ~IDeadLetterSink() = default;
    virtual void on_dead_letter(const DeadLetterRecord& record) noexcept = 0;
};

class DeadLetterQueue {
public:
    explicit DeadLetterQueue(DeadLetterConfig config = {});

    bool try_push(DeadLetterRecord&& record) noexcept;
    bool try_pop(DeadLetterRecord& out) noexcept;
    DeadLetterQueueSnapshot snapshot() const noexcept;

private:
    void trim_payload(DeadLetterRecord& record) const;

    DeadLetterConfig config_;
    mutable std::mutex mutex_;
    std::deque<DeadLetterRecord> records_;
    uint64_t total_pushed_{0};
    uint64_t total_popped_{0};
    uint64_t total_lost_{0};
};

} // namespace hpactor::mailbox
```

- [ ] **Step 4: Add the implementation**

Create `src/mailbox/dead_letter_queue.cpp`:

```cpp
#include <hpactor/mailbox/dead_letter_queue.hpp>

namespace hpactor::mailbox {

DeadLetterQueue::DeadLetterQueue(DeadLetterConfig config)
    : config_(config) {
    if (config_.capacity == 0) {
        config_.capacity = 4096;
    }
}

void DeadLetterQueue::trim_payload(DeadLetterRecord& record) const {
    record.payload_size = static_cast<uint32_t>(record.payload_sample.size());
    if (!config_.store_payload) {
        record.payload_sample.clear();
        return;
    }
    if (record.payload_sample.size() > config_.max_payload_sample_bytes) {
        record.payload_sample.resize(config_.max_payload_sample_bytes);
    }
}

bool DeadLetterQueue::try_push(DeadLetterRecord&& record) noexcept {
    if (!config_.enabled) {
        return false;
    }

    trim_payload(record);

    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.size() >= config_.capacity) {
        switch (config_.overflow_policy) {
        case DeadLetterOverflowPolicy::DropOldestRecord:
            records_.pop_front();
            total_lost_++;
            break;
        case DeadLetterOverflowPolicy::DropNewestRecord:
            total_lost_++;
            return false;
        case DeadLetterOverflowPolicy::MetadataOnly:
            record.payload_sample.clear();
            if (records_.size() >= config_.capacity) {
                records_.pop_front();
                total_lost_++;
            }
            break;
        }
    }

    records_.push_back(std::move(record));
    total_pushed_++;
    return true;
}

bool DeadLetterQueue::try_pop(DeadLetterRecord& out) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.empty()) {
        return false;
    }
    out = std::move(records_.front());
    records_.pop_front();
    total_popped_++;
    return true;
}

DeadLetterQueueSnapshot DeadLetterQueue::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    DeadLetterQueueSnapshot s;
    s.depth = static_cast<uint32_t>(records_.size());
    s.capacity = config_.capacity;
    s.total_pushed = total_pushed_;
    s.total_popped = total_popped_;
    s.total_lost = total_lost_;
    return s;
}

} // namespace hpactor::mailbox
```

- [ ] **Step 5: Run dead-letter tests**

Run:

```bash
ninja -C build test_dead_letter_queue
./build/tests/test_dead_letter_queue
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/mailbox/dead_letter_queue.hpp src/mailbox/dead_letter_queue.cpp CMakeLists.txt tests/mailbox/test_dead_letter_queue.cpp tests/CMakeLists.txt
git commit -m "feat: add bounded dead-letter queue"
```

---

### Task 6: Dead-Letter Capture Points

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `include/hpactor/ref/actor_proxy.hpp`
- Modify: `src/ref/actor_proxy.cpp`
- Create: `tests/ref/test_actor_proxy_dead_letters.cpp`
- Modify: `tests/actor/test_actor_system_backpressure.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Extend ActorSystem with dead-letter ownership**

In `include/hpactor/core/actor_system.hpp`, include:

```cpp
#include <hpactor/mailbox/dead_letter_queue.hpp>
```

Add to `Config`:

```cpp
mailbox::DeadLetterConfig dead_letters;
```

Add public methods:

```cpp
bool dead_letter(mailbox::DeadLetterRecord record) noexcept;
mailbox::DeadLetterQueueSnapshot dead_letter_snapshot() const noexcept;
bool pop_dead_letter(mailbox::DeadLetterRecord& out) noexcept;
```

Add private ownership:

```cpp
std::unique_ptr<mailbox::DeadLetterQueue> dead_letters_;
```

- [ ] **Step 2: Initialize the queue**

In `ActorSystem::ActorSystem` constructor body before network setup:

```cpp
dead_letters_ = std::make_unique<mailbox::DeadLetterQueue>(config_.dead_letters);
```

Add methods in `src/actor/actor_system.cpp`:

```cpp
bool ActorSystem::dead_letter(mailbox::DeadLetterRecord record) noexcept {
    if (!dead_letters_) {
        return false;
    }
    return dead_letters_->try_push(std::move(record));
}

mailbox::DeadLetterQueueSnapshot
ActorSystem::dead_letter_snapshot() const noexcept {
    if (!dead_letters_) {
        return {};
    }
    return dead_letters_->snapshot();
}

bool ActorSystem::pop_dead_letter(mailbox::DeadLetterRecord& out) noexcept {
    if (!dead_letters_) {
        return false;
    }
    return dead_letters_->try_pop(out);
}
```

- [ ] **Step 3: Capture missing local actors**

In `ActorSystem::try_deliver_local()`, before returning `ActorNotFound`:

```cpp
mailbox::DeadLetterRecord dl;
dl.reason = mailbox::DeadLetterReason::ActorNotFound;
dl.source = mailbox::DeadLetterSource::LocalDelivery;
dl.sender = msg.sender_address();
dl.target = ActorAddress{endpoint_, ActorType{0}, target, 0};
dl.type_tag = msg.type_id();
dl.message_id = options.message_id;
dl.frame_flags = options.flags;
dl.priority = priority;
dl.deadline_ns = deadline_ns;
dl.payload_sample = msg.payload();
(void)dead_letter(std::move(dl));
```

- [ ] **Step 4: Capture mailbox full when policy is `DeadLetter`**

In `ActorSystem::try_deliver_local()`, after `auto result = mailbox->try_push(std::move(msg), meta)`, add:

```cpp
if (!result.accepted() &&
    mailbox->config().overflow_policy == mailbox::OverflowPolicy::DeadLetter) {
    mailbox::DeadLetterRecord dl;
    dl.reason = mailbox::DeadLetterReason::MailboxFull;
    dl.source = mailbox::DeadLetterSource::MailboxAdmission;
    dl.sender = meta.sender;
    dl.target = ActorAddress{endpoint_, ActorType{0}, target, 0};
    dl.type_tag = meta.type_tag;
    dl.message_id = meta.message_id;
    dl.frame_flags = meta.flags;
    dl.priority = meta.priority;
    dl.deadline_ns = meta.deadline_ns;
    dl.payload_sample = msg.payload();
    dl.mailbox_depth = result.depth;
    dl.mailbox_capacity = result.capacity;
    if (dead_letter(std::move(dl))) {
        result.code = mailbox::EnqueueResultCode::ReroutedToDeadLetter;
    }
}
return result;
```

This relies on the Task 2 contract that `try_push()` does not move from `msg` when the message is rejected.

- [ ] **Step 5: Give ActorProxy access to ActorSystem for dead letters**

In `ActorProxy`, keep `ActorSystem* system_ = nullptr;`. In the `ActorProxy(const ActorAddress&, ActorSystem*)` constructor, store it.

When `transport_ == nullptr`, capture:

```cpp
if (system_) {
    mailbox::DeadLetterRecord dl;
    dl.reason = mailbox::DeadLetterReason::RemoteNodeUnreachable;
    dl.source = mailbox::DeadLetterSource::ActorProxy;
    dl.sender = msg.sender_address();
    dl.target = target;
    dl.type_tag = msg.type_id();
    dl.message_id = options.message_id;
    dl.frame_flags = options.flags;
    dl.payload_sample = msg.payload();
    (void)system_->dead_letter(std::move(dl));
}
```

When discovery returns no member, use `DeadLetterReason::MissingRoute`.

- [ ] **Step 6: Add missing local actor assertion**

Extend `tests/actor/test_actor_system_backpressure.cpp` after the missing delivery:

```cpp
auto dl_snap = system.dead_letter_snapshot();
assert(dl_snap.depth == 1);
mailbox::DeadLetterRecord dl;
assert(system.pop_dead_letter(dl));
assert(dl.reason == mailbox::DeadLetterReason::ActorNotFound);
assert(dl.type_tag == TypeTag::User);
```

- [ ] **Step 7: Add remote proxy dead-letter test**

Create `tests/ref/test_actor_proxy_dead_letters.cpp`:

```cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/ref/actor_proxy.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.enable_network = false;
    ActorSystem system(cfg);

    ActorAddress remote{endpoint_ops::parse_endpoint("10.0.0.1:9000"),
                        ActorType{1}, ActorId{44}, 0};
    ActorProxy proxy(remote, &system);
    auto result = proxy.try_send(remote,
        TypedMessage(TypeTag::User, StreamBuffer{9}));
    assert(!result.accepted());

    mailbox::DeadLetterRecord dl;
    assert(system.pop_dead_letter(dl));
    assert(dl.reason == mailbox::DeadLetterReason::RemoteNodeUnreachable);
    assert(dl.source == mailbox::DeadLetterSource::ActorProxy);
    assert(dl.target.id == ActorId{44});

    return 0;
}
```

Register:

```cmake
add_executable(test_actor_proxy_dead_letters ref/test_actor_proxy_dead_letters.cpp)
target_link_libraries(test_actor_proxy_dead_letters hpactor)
add_test(NAME test_actor_proxy_dead_letters COMMAND test_actor_proxy_dead_letters)
```

- [ ] **Step 8: Run dead-letter integration tests**

Run:

```bash
ninja -C build test_actor_system_backpressure test_actor_proxy_dead_letters
./build/tests/test_actor_system_backpressure
./build/tests/test_actor_proxy_dead_letters
```

Expected: both pass.

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp include/hpactor/ref/actor_proxy.hpp src/ref/actor_proxy.cpp tests/actor/test_actor_system_backpressure.cpp tests/ref/test_actor_proxy_dead_letters.cpp tests/CMakeLists.txt
git commit -m "feat: capture delivery failures as dead letters"
```

---

### Task 7: Pressure State And Local Backpressure Signals

**Files:**
- Modify: `include/hpactor/types/types.hpp`
- Modify: `include/hpactor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`
- Create: `tests/actor/test_backpressure_signals.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add TypeTag for pressure control**

In `include/hpactor/types/types.hpp`, add in the `0x70` reserved system range:

```cpp
BackpressureSignalTag = 0x70,
```

- [ ] **Step 2: Add ActorContext handler storage**

In `include/hpactor/actor_context.hpp`, include `<functional>` and `mailbox_policy.hpp`, then add public methods:

```cpp
using BackpressureHandler =
    std::function<void(const mailbox::BackpressureSignal&)>;

void on_backpressure(BackpressureHandler handler);
void handle_backpressure(const mailbox::BackpressureSignal& signal);
```

Add private field:

```cpp
BackpressureHandler backpressure_handler_;
```

In `src/actor/actor_context.cpp`:

```cpp
void ActorContext::on_backpressure(BackpressureHandler handler) {
    backpressure_handler_ = std::move(handler);
}

void ActorContext::handle_backpressure(
    const mailbox::BackpressureSignal& signal) {
    if (backpressure_handler_) {
        backpressure_handler_(signal);
    }
}
```

- [ ] **Step 3: Add ActorSystem signal routing**

In `ActorSystem`, declare:

```cpp
void signal_backpressure(const mailbox::BackpressureSignal& signal);
```

Implement:

```cpp
void ActorSystem::signal_backpressure(
    const mailbox::BackpressureSignal& signal) {
    if (signal.sender.id == ActorId{0}) {
        return;
    }
    std::lock_guard<std::mutex> lock(actor_contexts_mutex_);
    auto it = actor_contexts_.find(signal.sender.id);
    if (it != actor_contexts_.end() && it->second) {
        it->second->handle_backpressure(signal);
    }
}
```

- [ ] **Step 4: Emit signal from local admission**

In `ActorSystem::try_deliver_local()`, after the mailbox admission result is available and before returning:

```cpp
if (result.code == mailbox::EnqueueResultCode::AcceptedWithSoftPressure &&
    options.emit_backpressure) {
    mailbox::BackpressureSignal signal;
    signal.target = ActorAddress{endpoint_, ActorType{0}, target, 0};
    signal.sender = meta.sender;
    signal.reason = mailbox::BackpressureReason::HighWatermark;
    signal.depth = result.depth;
    signal.capacity = result.capacity;
    signal.pressure_ratio = result.pressure_ratio;
    signal.retry_after = result.retry_after;
    signal_backpressure(signal);
}
```

- [ ] **Step 5: Write the signal test**

Create `tests/actor/test_backpressure_signals.cpp`:

```cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.mailbox.default_capacity = 2;
    cfg.mailbox.high_watermark = 0.50;

    ActorSystem system(cfg);
    auto sender = system.spawn<EventBasedActor>();
    auto target = system.spawn<EventBasedActor>();

    ActorContext sender_ctx(sender, &system);
    bool signaled = false;
    sender_ctx.on_backpressure([&](const mailbox::BackpressureSignal& signal) {
        signaled = true;
        assert(signal.target.id == target.id());
        assert(signal.sender.id == sender.id());
        assert(signal.depth == 1);
        assert(signal.capacity == 2);
    });

    auto result = sender_ctx.try_send(target.address(),
        TypedMessage(TypeTag::User, StreamBuffer{1}));
    assert(result.accepted());
    assert(signaled);

    return 0;
}
```

Register and run:

```cmake
add_executable(test_backpressure_signals actor/test_backpressure_signals.cpp)
target_link_libraries(test_backpressure_signals hpactor)
add_test(NAME test_backpressure_signals COMMAND test_backpressure_signals)
```

```bash
ninja -C build test_backpressure_signals
./build/tests/test_backpressure_signals
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/types/types.hpp include/hpactor/actor_context.hpp src/actor/actor_context.cpp include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/actor/test_backpressure_signals.cpp tests/CMakeLists.txt
git commit -m "feat: emit local backpressure signals"
```

---

### Task 8: Priority Lanes, Drop Policies, And System Reserve

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`
- Create: `tests/mailbox/test_mailbox_overflow_policies.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write overflow policy tests**

Create `tests/mailbox/test_mailbox_overflow_policies.cpp` with three checks:

```cpp
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <cassert>

struct NoopScheduler : public hpactor::sched::IScheduler {
    void start() override {}
    void stop() override {}
    void notify_ready(hpactor::ActorId, uint8_t, int64_t) override {}
    void notify_idle(hpactor::ActorId) override {}
    void yield(hpactor::ActorId, uint8_t) override {}
    hpactor::sched::TimerHandle schedule_after(hpactor::sched::timer_callback, int64_t) override { return {}; }
    hpactor::sched::TimerHandle schedule_every(hpactor::sched::timer_callback, int64_t) override { return {}; }
    void cancel_timer(hpactor::sched::TimerHandle) override {}
    size_t worker_count() const override { return 1; }
    bool is_running() const override { return true; }
    void register_dedicated_thread(hpactor::ActorId, int) override {}
    void register_dedicated_pool(hpactor::ActorId, uint32_t) override {}
    void unregister_dedicated(hpactor::ActorId) override {}
};

int main() {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    NoopScheduler scheduler;

    MailboxConfig drop_newest;
    drop_newest.capacity.max_messages = 1;
    drop_newest.overflow_policy = OverflowPolicy::DropNewest;
    MPSCActorMailbox<TypedMessage> a(ActorId{1}, &scheduler, drop_newest);
    assert(a.try_push(TypedMessage(TypeTag::User, StreamBuffer{1})).accepted());
    auto dropped = a.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}));
    assert(dropped.code == EnqueueResultCode::DroppedNewest);

    MailboxConfig drop_oldest;
    drop_oldest.capacity.max_messages = 1;
    drop_oldest.overflow_policy = OverflowPolicy::DropOldest;
    MPSCActorMailbox<TypedMessage> b(ActorId{2}, &scheduler, drop_oldest);
    assert(b.try_push(TypedMessage(TypeTag::User, StreamBuffer{1})).accepted());
    auto accepted_after_drop = b.try_push(TypedMessage(TypeTag::User, StreamBuffer{2}));
    assert(accepted_after_drop.accepted());
    TypedMessage out;
    assert(b.try_pop(out));
    assert(out.payload()[0] == 2);

    MailboxConfig reserve;
    reserve.capacity.max_messages = 1;
    reserve.protected_system_messages = 1;
    MPSCActorMailbox<TypedMessage> c(ActorId{3}, &scheduler, reserve);
    assert(c.try_push(TypedMessage(TypeTag::User, StreamBuffer{1})).accepted());
    MailboxEnvelopeMeta meta;
    meta.type_tag = TypeTag::DownMsg;
    auto sys = c.try_push(TypedMessage(TypeTag::DownMsg, StreamBuffer{9}), meta);
    assert(sys.accepted());

    return 0;
}
```

- [ ] **Step 2: Register and run failing tests**

Add:

```cmake
add_executable(test_mailbox_overflow_policies mailbox/test_mailbox_overflow_policies.cpp)
target_link_libraries(test_mailbox_overflow_policies hpactor)
add_test(NAME test_mailbox_overflow_policies COMMAND test_mailbox_overflow_policies)
```

Run:

```bash
ninja -C build test_mailbox_overflow_policies
./build/tests/test_mailbox_overflow_policies
```

Expected: test fails because only `RejectNewest` is implemented.

- [ ] **Step 3: Add overflow policy branch in `try_push()`**

In `MPSCActorMailbox::try_push()`, replace direct rejection on failed reserve with:

```cpp
if (!try_reserve(meta.estimated_bytes)) {
    if (is_system_message(meta.type_tag) && try_reserve_system(meta.estimated_bytes)) {
        void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(T), actor_id_);
        auto* node = new (raw) T(std::move(msg));
        enqueue_reserved(node, meta);
        return make_result(EnqueueResultCode::Accepted);
    }

    switch (config_.overflow_policy) {
    case OverflowPolicy::DropNewest:
        total_dropped_.fetch_add(1, std::memory_order_relaxed);
        return make_result(EnqueueResultCode::DroppedNewest);
    case OverflowPolicy::DropOldest:
        if (drop_one_oldest()) {
            return try_push(std::move(msg), meta);
        }
        total_rejected_.fetch_add(1, std::memory_order_relaxed);
        return make_result(EnqueueResultCode::Rejected);
    case OverflowPolicy::DeadLetter:
        total_dead_letters_.fetch_add(1, std::memory_order_relaxed);
        return make_result(EnqueueResultCode::ReroutedToDeadLetter);
    case OverflowPolicy::RejectNewest:
    case OverflowPolicy::DropLowestPriority:
    case OverflowPolicy::SpillToOverflowQueue:
    case OverflowPolicy::SignalOnly:
    case OverflowPolicy::BlockWhenAllowed:
        total_rejected_.fetch_add(1, std::memory_order_relaxed);
        return make_result(EnqueueResultCode::Rejected);
    }
}
```

Add helpers:

```cpp
bool try_reserve_system(uint64_t bytes) noexcept {
    for (;;) {
        uint32_t cur = reserved_system_messages_.load(std::memory_order_acquire);
        if (cur >= config_.protected_system_messages) {
            return false;
        }
        if (reserved_system_messages_.compare_exchange_weak(
                cur, cur + 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            queued_bytes_.fetch_add(bytes, std::memory_order_acq_rel);
            return true;
        }
    }
}

bool drop_one_oldest() noexcept {
    T* node = mailbox_.dequeue();
    if (!node) {
        return false;
    }
    node->~T();
    mem::deallocate(node);
    reserved_messages_.fetch_sub(1, std::memory_order_acq_rel);
    total_dropped_.fetch_add(1, std::memory_order_relaxed);
    return true;
}
```

Add field:

```cpp
std::atomic<uint32_t> reserved_system_messages_{0};
```

When dequeuing a system message, decrement `reserved_system_messages_` if it is non-zero and `is_system_message(node->type_id())`.

- [ ] **Step 4: Add priority lane support**

If `config_.priority_aware` is true, store accepted messages in a fixed array:

```cpp
static constexpr uint8_t kMaxPriorityLevels = 4;
std::array<MPSCMailbox<T>, kMaxPriorityLevels> priority_lanes_;
std::array<std::atomic<uint32_t>, kMaxPriorityLevels> priority_depths_{};
```

In `enqueue_reserved()`, choose lane:

```cpp
uint8_t lane = config_.priority_aware
    ? static_cast<uint8_t>(std::min<uint8_t>(meta.priority, kMaxPriorityLevels - 1))
    : 0;
if (config_.priority_aware) {
    priority_lanes_[lane].enqueue(node);
    priority_depths_[lane].fetch_add(1, std::memory_order_release);
} else {
    mailbox_.enqueue(node);
}
```

In `dequeue()`, scan priority lanes from 0 to 3 when priority-aware mode is enabled.

- [ ] **Step 5: Implement `DropLowestPriority`**

Add helper:

```cpp
bool drop_lowest_priority() noexcept {
    if (!config_.priority_aware) {
        return drop_one_oldest();
    }
    for (int lane = kMaxPriorityLevels - 1; lane >= 0; --lane) {
        T* node = priority_lanes_[static_cast<size_t>(lane)].dequeue();
        if (node) {
            priority_depths_[static_cast<size_t>(lane)].fetch_sub(
                1, std::memory_order_release);
            node->~T();
            mem::deallocate(node);
            reserved_messages_.fetch_sub(1, std::memory_order_acq_rel);
            total_dropped_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}
```

Route `OverflowPolicy::DropLowestPriority` to this helper.

- [ ] **Step 6: Run overflow tests**

Run:

```bash
ninja -C build test_mailbox_overflow_policies test_bounded_mailbox
./build/tests/test_mailbox_overflow_policies
./build/tests/test_bounded_mailbox
```

Expected: both pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp tests/mailbox/test_mailbox_overflow_policies.cpp tests/CMakeLists.txt
git commit -m "feat: add mailbox overflow policies"
```

---

### Task 9: TOML And Runtime Configuration

**Files:**
- Modify: `include/hpactor/config/topology_model.hpp`
- Modify: `src/config/toml_parser.cpp`
- Modify: `src/actor/actor_system.cpp`
- Create: `tests/config/test_mailbox_config.cpp`
- Create: `tests/data/toml/mailbox_config.toml`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the config fixture**

Create `tests/data/toml/mailbox_config.toml`:

```toml
[system]
version = "1.0"
scheduler_threads = 2

[system.mailbox]
default_capacity = 64
default_policy = "dead_letter"
high_watermark = 0.75
low_watermark = 0.25
protected_system_messages = 8
backpressure = "local_and_remote"

[system.dead_letters]
enabled = true
capacity = 16
max_payload_sample_bytes = 12
overflow_policy = "metadata_only"
store_payload = false

[[actor]]
id = "worker"
behavior = "WorkerActor"
mailbox_capacity = 7

[actor.mailbox]
policy = "drop_newest"
priority_aware = true
max_overflow_depth = 3
```

- [ ] **Step 2: Write the failing parser test**

Create `tests/config/test_mailbox_config.cpp`:

```cpp
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>

#include <cassert>
#include <string>

int main() {
    auto path = std::string(TEST_DATA_DIR) + "/mailbox_config.toml";
    auto parsed = hpactor::config::TomlParser::parse(path);
    assert(parsed.has_value());
    const auto& model = parsed.value();

    assert(model.system.mailbox.default_capacity == 64);
    assert(model.system.mailbox.default_policy ==
           hpactor::mailbox::OverflowPolicy::DeadLetter);
    assert(model.system.mailbox.high_watermark == 0.75);
    assert(model.system.dead_letters.capacity == 16);
    assert(model.system.dead_letters.store_payload == false);

    assert(model.actors.size() == 1);
    assert(model.actors[0].mailbox_capacity == 7);
    assert(model.actors[0].mailbox.policy ==
           hpactor::mailbox::OverflowPolicy::DropNewest);
    assert(model.actors[0].mailbox.priority_aware);
    assert(model.actors[0].mailbox.max_overflow_depth == 3);

    return 0;
}
```

Register:

```cmake
add_executable(test_mailbox_config config/test_mailbox_config.cpp)
target_link_libraries(test_mailbox_config hpactor)
target_compile_definitions(test_mailbox_config PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/toml")
add_test(NAME test_mailbox_config COMMAND test_mailbox_config)
```

Run:

```bash
ninja -C build test_mailbox_config
./build/tests/test_mailbox_config
```

Expected: compile fails because topology config fields do not exist.

- [ ] **Step 3: Add config model structs**

In `include/hpactor/config/topology_model.hpp`, include mailbox headers and add:

```cpp
struct MailboxPolicyDef {
    hpactor::mailbox::OverflowPolicy policy =
        hpactor::mailbox::OverflowPolicy::RejectNewest;
    bool priority_aware{false};
    uint32_t max_overflow_depth{0};
};

struct SystemMailboxDef {
    uint32_t default_capacity{1024};
    uint64_t default_byte_capacity{0};
    hpactor::mailbox::OverflowPolicy default_policy =
        hpactor::mailbox::OverflowPolicy::RejectNewest;
    double high_watermark{0.80};
    double low_watermark{0.50};
    uint32_t protected_system_messages{32};
    hpactor::mailbox::BackpressureMode backpressure =
        hpactor::mailbox::BackpressureMode::LocalAndRemoteSignal;
};
```

Add `MailboxPolicyDef mailbox;` to `ActorDef`. Add to `SystemDef`:

```cpp
SystemMailboxDef mailbox;
hpactor::mailbox::DeadLetterConfig dead_letters;
```

- [ ] **Step 4: Add parser helpers**

In `src/config/toml_parser.cpp`, add:

```cpp
static double read_double(const toml::table& tbl, const char* key,
                          double default_val = 0.0) {
    auto node = tbl.get(key);
    if (node && node->is_floating_point()) {
        return node->value<double>().value_or(default_val);
    }
    if (node && node->is_integer()) {
        return static_cast<double>(node->value<int64_t>().value_or(0));
    }
    return default_val;
}

static mailbox::OverflowPolicy parse_overflow_policy(const std::string& s) {
    if (s == "drop_newest") return mailbox::OverflowPolicy::DropNewest;
    if (s == "drop_oldest") return mailbox::OverflowPolicy::DropOldest;
    if (s == "drop_lowest_priority") return mailbox::OverflowPolicy::DropLowestPriority;
    if (s == "dead_letter") return mailbox::OverflowPolicy::DeadLetter;
    if (s == "spill_to_overflow_queue") return mailbox::OverflowPolicy::SpillToOverflowQueue;
    if (s == "signal_only") return mailbox::OverflowPolicy::SignalOnly;
    if (s == "block_when_allowed") return mailbox::OverflowPolicy::BlockWhenAllowed;
    return mailbox::OverflowPolicy::RejectNewest;
}

static mailbox::BackpressureMode parse_backpressure_mode(const std::string& s) {
    if (s == "disabled") return mailbox::BackpressureMode::Disabled;
    if (s == "local") return mailbox::BackpressureMode::LocalSignal;
    if (s == "remote") return mailbox::BackpressureMode::RemoteSignal;
    return mailbox::BackpressureMode::LocalAndRemoteSignal;
}
```

- [ ] **Step 5: Parse system and actor mailbox tables**

Inside system parsing:

```cpp
if (auto* mb_node = st.get("mailbox")) {
    if (mb_node->is_table()) {
        auto& mt = *mb_node->as_table();
        data.system.mailbox.default_capacity =
            read_uint32(mt, "default_capacity", 1024);
        data.system.mailbox.default_byte_capacity =
            read_uint32(mt, "default_byte_capacity", 0);
        data.system.mailbox.default_policy =
            parse_overflow_policy(read_string(mt, "default_policy", "reject_newest"));
        data.system.mailbox.high_watermark =
            read_double(mt, "high_watermark", 0.80);
        data.system.mailbox.low_watermark =
            read_double(mt, "low_watermark", 0.50);
        data.system.mailbox.protected_system_messages =
            read_uint32(mt, "protected_system_messages", 32);
        data.system.mailbox.backpressure =
            parse_backpressure_mode(read_string(mt, "backpressure", "local_and_remote"));
    }
}

if (auto* dl_node = st.get("dead_letters")) {
    if (dl_node->is_table()) {
        auto& dt = *dl_node->as_table();
        data.system.dead_letters.enabled = read_bool(dt, "enabled", true);
        data.system.dead_letters.capacity = read_uint32(dt, "capacity", 4096);
        data.system.dead_letters.max_payload_sample_bytes =
            read_uint32(dt, "max_payload_sample_bytes", 512);
        data.system.dead_letters.store_payload =
            read_bool(dt, "store_payload", true);
        std::string op = read_string(dt, "overflow_policy", "drop_oldest_record");
        if (op == "drop_newest_record")
            data.system.dead_letters.overflow_policy =
                mailbox::DeadLetterOverflowPolicy::DropNewestRecord;
        else if (op == "metadata_only")
            data.system.dead_letters.overflow_policy =
                mailbox::DeadLetterOverflowPolicy::MetadataOnly;
        else
            data.system.dead_letters.overflow_policy =
                mailbox::DeadLetterOverflowPolicy::DropOldestRecord;
    }
}
```

Inside `parse_actor()`:

```cpp
if (auto* mb = tbl.get("mailbox")) {
    if (mb->is_table()) {
        auto& mt = *mb->as_table();
        def.mailbox.policy =
            parse_overflow_policy(read_string(mt, "policy", "reject_newest"));
        def.mailbox.priority_aware = read_bool(mt, "priority_aware", false);
        def.mailbox.max_overflow_depth =
            read_uint32(mt, "max_overflow_depth", 0);
    }
}
```

- [ ] **Step 6: Apply parsed config to ActorSystem**

In `ActorSystem::load_topology()` after parsing model:

```cpp
config_.mailbox.default_capacity = model.system.mailbox.default_capacity;
config_.mailbox.default_byte_capacity = model.system.mailbox.default_byte_capacity;
config_.mailbox.default_policy = model.system.mailbox.default_policy;
config_.mailbox.high_watermark = model.system.mailbox.high_watermark;
config_.mailbox.low_watermark = model.system.mailbox.low_watermark;
config_.mailbox.protected_system_messages =
    model.system.mailbox.protected_system_messages;
config_.mailbox.backpressure_mode = model.system.mailbox.backpressure;
config_.dead_letters = model.system.dead_letters;
dead_letters_ = std::make_unique<mailbox::DeadLetterQueue>(config_.dead_letters);
```

In `mailbox_config_for_actor_def()`:

```cpp
cfg.overflow_policy = def.mailbox.policy;
cfg.priority_aware = def.mailbox.priority_aware;
cfg.max_overflow_depth = def.mailbox.max_overflow_depth;
```

- [ ] **Step 7: Run config tests**

Run:

```bash
ninja -C build test_mailbox_config test_toml_parser
./build/tests/test_mailbox_config
./build/tests/test_toml_parser
```

Expected: both pass.

- [ ] **Step 8: Commit**

```bash
git add include/hpactor/config/topology_model.hpp src/config/toml_parser.cpp src/actor/actor_system.cpp tests/config/test_mailbox_config.cpp tests/data/toml/mailbox_config.toml tests/CMakeLists.txt
git commit -m "feat: parse mailbox backpressure config"
```

---

### Task 10: Metrics, Logs, And CLI Snapshots

**Files:**
- Modify: `include/hpactor/metrics/metrics_event.hpp`
- Modify: `include/hpactor/metrics/metrics_aggregator.hpp`
- Modify: `src/metrics/metrics_aggregator.cpp`
- Modify: `include/hpactor/log/log_category.hpp`
- Modify: log category implementation file
- Modify: `include/hpactor/cli/cli_types.hpp`
- Modify: CLI formatter tests if snapshot output changes
- Modify: `tests/metrics/test_metrics_integration.cpp`

- [ ] **Step 1: Extend metric events without changing event size**

Modify `MetricEvent` in `include/hpactor/metrics/metrics_event.hpp`:

```cpp
enum class MetricEventType : uint8_t {
    kMailboxEnqueue    = 0,
    kMailboxDequeue    = 1,
    kMessageProcessed  = 2,
    kActorSpawned      = 3,
    kActorTerminated   = 4,
    kSchedulerDispatch = 5,
    kSchedulerSteal    = 6,
    kSupervisorRestart = 7,
    kMemoryAlloc       = 8,
    kMemoryFree        = 9,
    kMailboxRejected   = 10,
    kMailboxDropped    = 11,
    kMailboxDeadLetter = 12,
    kBackpressureSignal = 13,
    kDeadLetterLost    = 14,
};

struct alignas(32) MetricEvent {
    uint64_t        timestamp_ns;
    ActorId         actor_id;
    MetricEventType event_type;
    uint8_t         code;
    uint8_t         aux;
    uint8_t         _pad[1];
    uint32_t        value_hi;
};
```

Keep:

```cpp
static_assert(sizeof(MetricEvent) == 32, "MetricEvent must be 32 bytes");
```

- [ ] **Step 2: Emit mailbox metric events**

In `MPSCActorMailbox`, emit:

```cpp
emit_metric(metrics::MetricEventType::kMailboxRejected, 1);
emit_metric(metrics::MetricEventType::kMailboxDropped, 1);
emit_metric(metrics::MetricEventType::kMailboxDeadLetter, 1);
```

at rejection, drop, and dead-letter result points.

- [ ] **Step 3: Register metric families**

In `metrics_aggregator.hpp`, add family pointers:

```cpp
MetricFamily* mailbox_rejected_family_ = nullptr;
MetricFamily* mailbox_dropped_family_ = nullptr;
MetricFamily* mailbox_dead_letter_family_ = nullptr;
MetricFamily* backpressure_signal_family_ = nullptr;
MetricFamily* dead_letter_lost_family_ = nullptr;
```

In `ensure_families_registered()`:

```cpp
mailbox_rejected_family_ = &registry_.register_family(
    "hpactor_mailbox_rejected_total", "Mailbox admission rejections.",
    MetricType::kCounter);
mailbox_dropped_family_ = &registry_.register_family(
    "hpactor_mailbox_dropped_total", "Mailbox policy drops.",
    MetricType::kCounter);
mailbox_dead_letter_family_ = &registry_.register_family(
    "hpactor_mailbox_dead_letters_total", "Mailbox messages routed to dead letters.",
    MetricType::kCounter);
backpressure_signal_family_ = &registry_.register_family(
    "hpactor_backpressure_signals_total", "Backpressure signals emitted.",
    MetricType::kCounter);
dead_letter_lost_family_ = &registry_.register_family(
    "hpactor_dead_letter_lost_total", "Dead-letter records lost.",
    MetricType::kCounter);
```

In `on_event()` add cases that increment counters with actor labels.

- [ ] **Step 4: Add log event ids**

In `include/hpactor/log/log_category.hpp`, add after `kMailboxDepthHigh`:

```cpp
kMailboxHighWatermark,
kMailboxLowWatermarkRecovered,
kMailboxFull,
kMailboxMessageRejected,
kMailboxMessageDropped,
kMailboxOverflowRerouted,
kBackpressureSignalSent,
kSystemReserveExhausted,
kDeadLetterQueued,
kDeadLetterLost,
```

Update `to_string(LogEventId)` implementation to return stable names for each new id.

- [ ] **Step 5: Add metrics assertions**

Extend `tests/metrics/test_metrics_integration.cpp` with direct event aggregation:

```cpp
MetricEvent rejected{};
rejected.actor_id = ActorId{42};
rejected.event_type = MetricEventType::kMailboxRejected;
rejected.value_hi = 1;
aggregator.on_event(rejected);

MetricEvent dead{};
dead.actor_id = ActorId{42};
dead.event_type = MetricEventType::kMailboxDeadLetter;
dead.value_hi = 1;
aggregator.on_event(dead);

auto snap = registry.snapshot();
bool saw_rejected = false;
bool saw_dead = false;
for (const auto& fam : snap.families) {
    if (fam.name == "hpactor_mailbox_rejected_total") saw_rejected = true;
    if (fam.name == "hpactor_mailbox_dead_letters_total") saw_dead = true;
}
assert(saw_rejected);
assert(saw_dead);
```

- [ ] **Step 6: Run observability tests**

Run:

```bash
ninja -C build test_metrics_integration test_log_category test_bounded_mailbox
./build/tests/test_metrics_integration
./build/tests/test_log_category
./build/tests/test_bounded_mailbox
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/metrics/metrics_event.hpp include/hpactor/metrics/metrics_aggregator.hpp src/metrics/metrics_aggregator.cpp include/hpactor/log/log_category.hpp tests/metrics/test_metrics_integration.cpp
git commit -m "feat: expose mailbox pressure metrics"
```

---

### Task 11: Remote Send Failure Feedback And Ingress Boundaries

**Files:**
- Modify: `include/hpactor/net/transport.hpp`
- Modify: `include/hpactor/net/tcp_transport.hpp`
- Modify: `src/net/tcp_transport.cpp`
- Modify: `include/hpactor/net/connection_pool.hpp`
- Modify: `src/net/connection_pool.cpp`
- Modify: `src/ref/actor_proxy.cpp`
- Modify: `src/actor/http_gateway_actor.cpp`
- Modify: RPC tests where admission failure is exercised

- [ ] **Step 1: Add non-breaking transport `try_send()`**

In `include/hpactor/net/transport.hpp`, add:

```cpp
virtual bool try_send(const ActorAddress& target,
                      const StreamBuffer& encoded) = 0;

virtual void send(const ActorAddress& target,
                  const StreamBuffer& encoded) {
    (void)try_send(target, encoded);
}
```

Remove the old pure virtual `send()` declaration. This preserves existing callers.

- [ ] **Step 2: Implement `TcpTransport::try_send()`**

In `TcpTransport`, move current send body into `try_send()` and return false on no pool, no connection, or rejected write. Keep `send()` inherited from `Transport`.

Expected shape:

```cpp
bool TcpTransport::try_send(const ActorAddress& target,
                            const StreamBuffer& encoded) {
    auto* pool = get_or_create_pool(target.endpoint);
    if (pool == nullptr) {
        return false;
    }
    return pool->try_send(target, encoded);
}
```

Add `ConnectionPool::try_send()` that returns false when no connection can be acquired.

- [ ] **Step 3: Capture transport send failures in ActorProxy**

In `ActorProxy::try_send()` replace the fire-and-forget transport send call with:

```cpp
if (!transport_->try_send(resolved_target, frame.encode())) {
    result.code = mailbox::EnqueueResultCode::ActorNotFound;
    if (system_) {
        mailbox::DeadLetterRecord dl;
        dl.reason = mailbox::DeadLetterReason::TransportSendFailed;
        dl.source = mailbox::DeadLetterSource::Transport;
        dl.sender = sender_addr;
        dl.target = resolved_target;
        dl.type_tag = msg.type_id();
        dl.message_id = frame.pb_frame.message_id();
        dl.frame_flags = frame.pb_frame.flags();
        dl.payload_sample = msg.payload();
        (void)system_->dead_letter(std::move(dl));
    }
    return result;
}
```

- [ ] **Step 4: Translate HTTP ingress overload**

In `HTTPGatewayActor`, where it calls `system().deliver_local(target.id, std::move(correlated_msg))`, switch to `try_deliver_local()`. On non-accepted result, reply with status 429 and `Retry-After` when the result is retryable.

Expected branch:

```cpp
auto enqueue = system().try_deliver_local(target.id, std::move(correlated_msg),
                                          static_cast<uint8_t>(route.priority),
                                          INT64_MAX, {});
if (!enqueue.accepted()) {
    send_error_response(request_id, 429, "Too Many Requests");
    return;
}
```

Use the existing HTTP gateway response helper names from `src/actor/http_gateway_actor.cpp`.

- [ ] **Step 5: Run remote and HTTP tests**

Run:

```bash
ninja -C build test_actor_proxy_dead_letters test_http_gateway test_rpc_channel
./build/tests/test_actor_proxy_dead_letters
./build/tests/test_http_gateway
./build/tests/test_rpc_channel
```

Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/net/transport.hpp include/hpactor/net/tcp_transport.hpp src/net/tcp_transport.cpp include/hpactor/net/connection_pool.hpp src/net/connection_pool.cpp src/ref/actor_proxy.cpp src/actor/http_gateway_actor.cpp
git commit -m "feat: report remote delivery failures"
```

---

### Task 12: Final Stress Tests And Regression Sweep

**Files:**
- Create: `tests/mailbox/test_mailbox_backpressure_stress.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: docs if any API signatures changed from the architecture design

- [ ] **Step 1: Add stress test**

Create `tests/mailbox/test_mailbox_backpressure_stress.cpp`:

```cpp
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <cassert>
#include <thread>
#include <vector>

struct NoopScheduler : public hpactor::sched::IScheduler {
    void start() override {}
    void stop() override {}
    void notify_ready(hpactor::ActorId, uint8_t, int64_t) override {}
    void notify_idle(hpactor::ActorId) override {}
    void yield(hpactor::ActorId, uint8_t) override {}
    hpactor::sched::TimerHandle schedule_after(hpactor::sched::timer_callback, int64_t) override { return {}; }
    hpactor::sched::TimerHandle schedule_every(hpactor::sched::timer_callback, int64_t) override { return {}; }
    void cancel_timer(hpactor::sched::TimerHandle) override {}
    size_t worker_count() const override { return 1; }
    bool is_running() const override { return true; }
    void register_dedicated_thread(hpactor::ActorId, int) override {}
    void register_dedicated_pool(hpactor::ActorId, uint32_t) override {}
    void unregister_dedicated(hpactor::ActorId) override {}
};

int main() {
    using namespace hpactor;
    using namespace hpactor::mailbox;

    NoopScheduler scheduler;
    MailboxConfig cfg;
    cfg.capacity.max_messages = 128;
    cfg.overflow_policy = OverflowPolicy::RejectNewest;

    MPSCActorMailbox<TypedMessage> mailbox(ActorId{99}, &scheduler, cfg);

    std::vector<std::thread> producers;
    for (int p = 0; p < 8; ++p) {
        producers.emplace_back([&mailbox, p]() {
            for (int i = 0; i < 1000; ++i) {
                (void)mailbox.try_push(TypedMessage(
                    TypeTag::User,
                    StreamBuffer{static_cast<uint8_t>(p),
                                 static_cast<uint8_t>(i & 0xFF)}));
            }
        });
    }
    for (auto& t : producers) {
        t.join();
    }

    auto snap = mailbox.snapshot();
    assert(snap.depth <= 128);
    assert(snap.total_rejected > 0);

    TypedMessage out;
    uint32_t drained = 0;
    while (mailbox.try_pop(out)) {
        drained++;
    }
    assert(drained <= 128);
    assert(mailbox.snapshot().depth == 0);
    return 0;
}
```

Register:

```cmake
add_executable(test_mailbox_backpressure_stress mailbox/test_mailbox_backpressure_stress.cpp)
target_link_libraries(test_mailbox_backpressure_stress hpactor)
add_test(NAME test_mailbox_backpressure_stress COMMAND test_mailbox_backpressure_stress)
```

- [ ] **Step 2: Run focused test suite**

Run:

```bash
ninja -C build test_mailbox_policy test_bounded_mailbox test_mailbox_overflow_policies test_dead_letter_queue test_actor_system_backpressure test_actor_context_try_send test_actor_proxy_dead_letters test_backpressure_signals test_mailbox_config test_mailbox_backpressure_stress
./build/tests/test_mailbox_policy
./build/tests/test_bounded_mailbox
./build/tests/test_mailbox_overflow_policies
./build/tests/test_dead_letter_queue
./build/tests/test_actor_system_backpressure
./build/tests/test_actor_context_try_send
./build/tests/test_actor_proxy_dead_letters
./build/tests/test_backpressure_signals
./build/tests/test_mailbox_config
./build/tests/test_mailbox_backpressure_stress
```

Expected: all pass.

- [ ] **Step 3: Run full build and tests**

Run:

```bash
ninja -C build
ctest --test-dir build --output-on-failure
```

Expected: build succeeds and all tests pass.

- [ ] **Step 4: Documentation consistency check**

Run:

```bash
rg -n "try_deliver_local|DeadLetterQueue|BackpressureSignal|OverflowPolicy" docs/architecture/actor/mailbox-management-backpressure-design.md docs/architecture/core/unified-message-passing.md
```

Expected: command prints the architecture sections that describe the implemented names.

- [ ] **Step 5: Commit**

```bash
git add tests/mailbox/test_mailbox_backpressure_stress.cpp tests/CMakeLists.txt docs/architecture/actor/mailbox-management-backpressure-design.md docs/architecture/core/unified-message-passing.md
git commit -m "test: add mailbox backpressure stress coverage"
```

---

## Final Verification Checklist

- [ ] `ninja -C build` exits 0.
- [ ] `ctest --test-dir build --output-on-failure` exits 0.
- [ ] `./build/tests/test_mailbox_backpressure_stress` exits 0.
- [ ] Run the same red-flag scan used during plan review against all modified source, test, and docs files, and confirm it prints no matches introduced by this feature.
- [ ] Existing `context()->send(target, msg)` call sites compile unchanged.
- [ ] Existing `ActorSystem::deliver_local(target, msg)` call sites compile unchanged.
- [ ] `try_deliver_local()` returns `ActorNotFound` for missing local actors.
- [ ] A mailbox with capacity `N` never reports depth greater than `N + protected_system_messages`.
- [ ] `OverflowPolicy::DeadLetter` records the failed message in `DeadLetterQueue`.
- [ ] Missing routes and unavailable remote transports produce dead-letter records.
- [ ] Mailbox pressure metrics and dead-letter metrics appear in OpenMetrics output.
