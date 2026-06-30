<!--
Copyright 2026 HPActor Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Fixed-Message Disruptor Actor Mailbox Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in, local-only fixed-message actor whose user traffic is stored allocation-free in a bounded Disruptor-style MPSC ring while lifecycle and control traffic remains on a protected MPSC system lane.

**Architecture:** `FixedMailboxActor<Capacity, Messages...>` owns a shared `FixedActorMailboxCore` containing a fixed user ring, a protected `TypedMessage` system lane, and one wakeup gate. Existing variable actors keep their concrete `MPSCActorMailbox<TypedMessage>` path; fixed actors publish narrow RTTI-free delivery and execution ports through `ActorDirectoryEntry`.

**Tech Stack:** C++20, CMake/Ninja, GoogleTest 1.14, Relacy, HPActor actor runtime, custom memory regions, lock-free MPSC primitives, metrics/CLI/DLQ infrastructure.

## Global Constraints

- Work only in the repository-required isolated worktree and keep build output in that worktree's `build/` directory.
- Follow RED → GREEN → REFACTOR for every production behavior. Record each RED and GREEN command.
- C++20 only; no `dynamic_cast`, `typeid`, exception-based control flow, or public APIs requiring RTTI/exceptions.
- `MPSCActorMailbox<TypedMessage>` remains the default and its current hot path must not gain a virtual mailbox call.
- The fixed backend is opt-in, local-only, and guarded by `ENABLE_FIXED_DISRUPTOR_MAILBOX`, default `OFF` while experimental.
- Fixed user messages must be standard-layout, trivially default constructible, trivially copyable, and trivially destructible.
- Ring capacity is a compile-time power of two in `[2, 1 << 20]`.
- User-ring overflow is non-blocking `RejectNewest` only. No waiting, eviction, spill, retry, or silent fallback.
- The single actor activation is the only consumer. A claimed ring slot remains leased through handler dispatch.
- The protected system lane accepts only `TypeTag < TypeTag::User` and retains variable-length `TypedMessage` compatibility.
- The supported fixed path preserves sender, trace, deadline, message ID, lifecycle/quarantine admission, metrics, logging, backpressure, and metadata-only DLQ evidence.
- Remote delivery, dynamic/protobuf user messages, ask/reply, reliable delivery, priority lanes, EDF, live resizing, and fixed-payload DLQ replay remain unsupported in version 1.
- All new public files require Apache 2.0 headers and Doxygen concurrency/ownership contracts.
- Use the narrowest verification after each task; run the full cross-cutting suite only at the final gate.

## Reference Documents

- Design: `docs/superpowers/specs/2026-06-30-fixed-disruptor-actor-mailbox-design.md`
- Concurrency rules: `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- Existing mailbox: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`
- Existing sequence ring: `include/hpactor/adt/mpsc_ring_buffer.hpp`
- Actor adoption: `src/runtime/actor_spawner.cpp`
- Scheduler runner: `src/sched/actor_execution_engine.cpp`
- Messaging ownership: `src/runtime/messaging_runtime.hpp`

## File Structure

### New public components

- `include/hpactor/mailbox/mailbox_kind.hpp` — stable backend discriminator.
- `include/hpactor/mailbox/fixed_message_envelope.hpp` — message concept, fixed metadata, closed variant, send options.
- `include/hpactor/mailbox/disruptor_mpsc_ring.hpp` — standalone claim/publish/read-lease primitive.
- `include/hpactor/mailbox/fixed_mailbox_ports.hpp` — delivery, control-ingress, execution, lifecycle, and adoption ports.
- `include/hpactor/mailbox/fixed_actor_mailbox.hpp` — hybrid core, system lane, wakeup, pressure, and lifecycle.
- `include/hpactor/actor/fixed_mailbox_actor.hpp` — actor base and fixed behavior dispatch.
- `include/hpactor/ref/fixed_actor_ref.hpp` — typed local reference and non-blocking send.

### New compiled components

- `src/sched/fixed_mailbox_actor_runner.cpp` — fixed activation state machine.
- `src/runtime/fixed_mailbox_delivery.cpp` — MessagingRuntime port adapters and outcome recording.

### Existing files modified

- `CMakeLists.txt` and `include/hpactor/hpactor_config.hpp.in` — experimental build gate.
- `include/hpactor/actor/abstract_actor.hpp` — RTTI-free mailbox kind/adoption hooks.
- `include/hpactor/actor/event_based_actor.hpp` and `src/actor/event_based_actor.cpp` — virtual immediate-drain seam.
- `include/hpactor/actor/actor_directory.hpp` and `src/actor/actor_directory.cpp` — fixed ports in atomic publication records.
- `src/runtime/spawn_spec.hpp`, `src/runtime/actor_spawner.hpp`, and `src/runtime/actor_spawner.cpp` — fixed-core adoption.
- `include/hpactor/actor/actor_system.hpp` and `src/actor/actor_system.cpp` — `spawn_fixed` and fixed port wiring.
- `include/hpactor/actor/actor_context.hpp` — metadata-populating fixed `try_send` overload.
- `src/runtime/messaging_runtime.hpp` and `src/runtime/messaging_runtime.cpp` — preflight/outcome port.
- `include/hpactor/mailbox/local_delivery_engine.hpp` and `src/mailbox/local_delivery_engine.cpp` — fixed control-ingress routing.
- `include/hpactor/sched/actor_execution_engine.hpp` and `src/sched/actor_execution_engine.cpp` — backend selection and fixed runner.
- `include/hpactor/msg/enqueue_result.hpp` — exact failure-reason override for fixed rejection paths.
- `include/hpactor/msg/failure_reason.hpp` and `src/msg/failure_reason.cpp` — unsupported-remote/dynamic reasons.
- `include/hpactor/msg/dead_letter_record.hpp` — metadata-only fixed-message evidence.
- `include/hpactor/cli/cli_types.hpp` and `src/cli/commands/actor_commands.cpp` — backend-aware snapshots.
- `include/hpactor/metrics/metrics_event.hpp` and `src/metrics/metrics_aggregator.cpp` — backend and ring diagnostics.
- `tests/architecture/CMakeLists.txt` — no-RTTI/no-facade/no-late-setter guards.
- subsystem CMake files — new source and test registration.

### New tests and benchmark

- `tests/unit/mailbox/test_fixed_message_envelope.cpp`
- `tests/unit/mailbox/test_disruptor_mpsc_ring.cpp`
- `tests/unit/mailbox/test_disruptor_stress.cpp`
- `tests/unit/mailbox/test_disruptor_relacy.cpp`
- `tests/unit/mailbox/test_fixed_actor_mailbox.cpp`
- `tests/unit/actor/test_fixed_mailbox_actor.cpp`
- `tests/unit/core/test_fixed_mailbox_failure_reason.cpp`
- `tests/integration/actor/test_fixed_mailbox_delivery.cpp`
- `tests/integration/actor/test_fixed_mailbox_lifecycle.cpp`
- `tests/integration/actor/test_fixed_mailbox_scheduler.cpp`
- `tests/integration/actor/test_fixed_mailbox_observability.cpp`
- `tests/compile/fixed_mailbox_valid.cpp`
- `tests/compile/fixed_mailbox_invalid_capacity.cpp`
- `tests/compile/fixed_mailbox_invalid_message.cpp`
- `tests/compile/fixed_mailbox_undeclared_send.cpp`
- `tests/compile/CMakeLists.txt`
- `apps/bench_fixed_mailbox/CMakeLists.txt`
- `apps/bench_fixed_mailbox/19_bench_fixed_mailbox.cpp`
- `apps/bench_fixed_mailbox/README.md`

---

### Task 1: Add the feature gate and fixed-message type contract

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `include/hpactor/hpactor_config.hpp.in`
- Create: `include/hpactor/mailbox/mailbox_kind.hpp`
- Create: `include/hpactor/mailbox/fixed_message_envelope.hpp`
- Create: `tests/unit/mailbox/test_fixed_message_envelope.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

**Interfaces:**
- Produces: `MailboxKind`, `FixedMailboxMessage`, `valid_fixed_ring_capacity()`, `FixedSendOptions`, `FixedEnvelopeMeta`, and `FixedMessageEnvelope<Messages...>`.
- Consumers: Tasks 2, 4, 5, 6, 7, 9, and 10.

- [ ] **Step 1: Write compile-time and runtime envelope tests**

Add `test_fixed_message_envelope.cpp`:

```cpp
namespace {
struct Quote {
    uint64_t instrument{};
    int64_t price{};
};
struct NonFixed {
    std::string value;
};
} // namespace

static_assert(hpactor::mailbox::FixedMailboxMessage<Quote>);
static_assert(!hpactor::mailbox::FixedMailboxMessage<NonFixed>);
static_assert(hpactor::mailbox::valid_fixed_ring_capacity(2));
static_assert(hpactor::mailbox::valid_fixed_ring_capacity(1024));
static_assert(!hpactor::mailbox::valid_fixed_ring_capacity(3));
static_assert(!hpactor::mailbox::valid_fixed_ring_capacity((1u << 20) + 1));

TEST(FixedMessageEnvelopeTest, PreservesMessageAndMetadata) {
    using Envelope = hpactor::mailbox::FixedMessageEnvelope<Quote>;
    Envelope env;
    env.message = Quote{7, 10125};
    env.meta.message_id = 44;
    env.meta.deadline_ns = 9000;
    ASSERT_TRUE(std::holds_alternative<Quote>(env.message));
    EXPECT_EQ(std::get<Quote>(env.message).instrument, 7u);
    EXPECT_EQ(env.meta.message_id, 44u);
    EXPECT_EQ(env.meta.deadline_ns, 9000);
}
```

- [ ] **Step 2: Register and run the test to verify RED**

Add `test_fixed_message_envelope.cpp` to `test_unit_mailbox`.

Run:

```bash
cmake -S . -B build -GNinja -DENABLE_FIXED_DISRUPTOR_MAILBOX=ON
ninja -C build test_unit_mailbox
```

Expected: compilation fails because `fixed_message_envelope.hpp` and the new CMake option do not exist.

- [ ] **Step 3: Add the build gate and generated macro**

Add:

```cmake
option(ENABLE_FIXED_DISRUPTOR_MAILBOX
       "Enable experimental fixed-message Disruptor actor mailbox" OFF)
set(HPACTOR_ENABLE_FIXED_DISRUPTOR_MAILBOX
    ${ENABLE_FIXED_DISRUPTOR_MAILBOX})
```

Add `#cmakedefine01 HPACTOR_ENABLE_FIXED_DISRUPTOR_MAILBOX` to
`hpactor_config.hpp.in`.

- [ ] **Step 4: Implement the type contract**

Use these exact public definitions:

```cpp
enum class MailboxKind : uint8_t {
    VariableMpsc = 0,
    FixedDisruptor = 1,
};

inline constexpr size_t kMaxFixedRingCapacity = 1u << 20;

[[nodiscard]] consteval bool valid_fixed_ring_capacity(size_t capacity) {
    return capacity >= 2 && capacity <= kMaxFixedRingCapacity &&
           (capacity & (capacity - 1)) == 0;
}

template <typename T>
concept FixedMailboxMessage =
    std::is_standard_layout_v<T> &&
    std::is_trivially_default_constructible_v<T> &&
    std::is_trivially_copyable_v<T> &&
    std::is_trivially_destructible_v<T>;

struct FixedSendOptions {
    int64_t deadline_ns{INT64_MAX};
    uint64_t message_id{0};
    uint32_t flags{0};
};

struct FixedEnvelopeMeta {
    ActorAddress sender{};
    TraceContext trace{};
    int64_t deadline_ns{INT64_MAX};
    uint64_t message_id{0};
    uint64_t enqueue_sequence{0};
    uint32_t flags{0};
    bool has_trace{false};
};

template <FixedMailboxMessage... Messages>
struct FixedMessageEnvelope {
    static_assert(sizeof...(Messages) > 0);
    static_assert(detail::are_unique_v<Messages...>);
    using message_type = std::variant<Messages...>;
    message_type message{};
    FixedEnvelopeMeta meta{};
};
```

Implement `detail::are_unique_v` with a recursive type comparison, not RTTI.

- [ ] **Step 5: Run GREEN and refactor**

Run:

```bash
ninja -C build test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='FixedMessageEnvelopeTest.*'
```

Expected: the target builds and the fixed-envelope test passes.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt include/hpactor/hpactor_config.hpp.in \
  include/hpactor/mailbox/mailbox_kind.hpp \
  include/hpactor/mailbox/fixed_message_envelope.hpp \
  tests/unit/mailbox/test_fixed_message_envelope.cpp \
  tests/unit/mailbox/CMakeLists.txt
git commit -m "feat: define fixed mailbox message contract"
```

---

### Task 2: Implement the standalone Disruptor MPSC ring

**Files:**
- Create: `include/hpactor/mailbox/disruptor_mpsc_ring.hpp`
- Create: `tests/unit/mailbox/test_disruptor_mpsc_ring.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

**Interfaces:**
- Consumes: `valid_fixed_ring_capacity()` from Task 1.
- Produces: `DisruptorMpscRing<T, Capacity>`, nested `ReadLease`, `try_publish()`, `try_acquire()`, `close()`, `empty()`, and `snapshot()`.
- Consumers: Tasks 3 and 4.

- [ ] **Step 1: Write focused RED tests**

Cover FIFO, exact full detection, lease-blocked reuse, wraparound, and close:

```cpp
using Ring = hpactor::mailbox::DisruptorMpscRing<uint64_t, 4>;

TEST(DisruptorMpscRingTest, RejectsAtExactCapacityAndReusesAfterLease) {
    Ring ring;
    EXPECT_TRUE(ring.try_publish(10).accepted());
    EXPECT_TRUE(ring.try_publish(11).accepted());
    EXPECT_TRUE(ring.try_publish(12).accepted());
    EXPECT_TRUE(ring.try_publish(13).accepted());
    EXPECT_FALSE(ring.try_publish(14).accepted());

    {
        auto lease = ring.try_acquire();
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease.value(), 10u);
        EXPECT_FALSE(ring.try_publish(14).accepted());
    }

    EXPECT_TRUE(ring.try_publish(14).accepted());
}

TEST(DisruptorMpscRingTest, CloseRejectsWithoutChangingDepth) {
    Ring ring;
    ring.close();
    auto result = ring.try_publish(1);
    EXPECT_EQ(result.code, RingPublishCode::Closed);
    EXPECT_EQ(ring.snapshot().published_depth, 0u);
}
```

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_mailbox
```

Expected: compilation fails because `DisruptorMpscRing` is undefined.

- [ ] **Step 3: Implement slot ownership and publication**

The producer loop must be:

```cpp
template <typename U>
[[nodiscard]] RingPublishResult try_publish(U&& value) noexcept {
    static_assert(std::is_nothrow_assignable_v<T&, U&&>);
    if (closed_.load(std::memory_order_acquire))
        return {RingPublishCode::Closed, 0};

    uint64_t sequence = claim_cursor_.load(std::memory_order_relaxed);
    for (;;) {
        Slot& slot = slots_[sequence & kMask];
        if (slot.sequence.load(std::memory_order_acquire) != sequence)
            return {RingPublishCode::Full, sequence};
        if (claim_cursor_.compare_exchange_weak(
                sequence, sequence + 1, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            slot.value = std::forward<U>(value);
            slot.sequence.store(sequence + 1, std::memory_order_release);
            published_cursor_.store(sequence + 1, std::memory_order_release);
            update_max_depth(sequence + 1);
            return {RingPublishCode::Published, sequence};
        }
        claim_retries_.fetch_add(1, std::memory_order_relaxed);
    }
}
```

`ReadLease` must be move-only. Its destructor calls a private `release(sequence)`
exactly once:

```cpp
void release(uint64_t sequence) noexcept {
    slots_[sequence & kMask].sequence.store(sequence + Capacity,
                                            std::memory_order_release);
    consumer_sequence_ = sequence + 1;
    consumer_mirror_.store(sequence + 1, std::memory_order_release);
}
```

`try_acquire()` performs one acquire load and never spins.

- [ ] **Step 4: Run GREEN**

```bash
ninja -C build test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='DisruptorMpscRingTest.*'
```

Expected: all ring tests pass.

- [ ] **Step 5: Add Doxygen invariants and run formatting checks**

Document producer roles, single-consumer requirement, claim and publication
linearization points, memory ordering, lease lifetime, close semantics, and the
stalled-producer gap tradeoff.

Run:

```bash
git diff --check
```

Expected: no whitespace errors.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/mailbox/disruptor_mpsc_ring.hpp \
  tests/unit/mailbox/test_disruptor_mpsc_ring.cpp \
  tests/unit/mailbox/CMakeLists.txt
git commit -m "feat: add fixed-slot disruptor MPSC ring"
```

---

### Task 3: Model-check and stress the ring

**Files:**
- Create: `tests/unit/mailbox/test_disruptor_relacy.cpp`
- Create: `tests/unit/mailbox/test_disruptor_stress.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

**Interfaces:**
- Consumes: `DisruptorMpscRing<uint64_t, Capacity>`.
- Produces: exhaustive ordering evidence and high-contention accounting evidence.

- [ ] **Step 1: Add the stalled-producer deterministic test**

Expose a test-only publication hook under `HPACTOR_TESTING` that pauses after
claim. Test sequence 0 paused, sequence 1 published, consumer blocked, then
sequence 0 published:

```cpp
TEST(DisruptorMpscRingStressTest, DoesNotSkipClaimedPublicationGap) {
    DisruptorMpscRing<uint64_t, 4> ring;
    std::latch claimed{1};
    std::latch release{1};
    ring.set_after_claim_hook_for_test([&](uint64_t seq) {
        if (seq == 0) {
            claimed.count_down();
            release.wait();
        }
    });

    std::thread first([&] { EXPECT_TRUE(ring.try_publish(10).accepted()); });
    claimed.wait();
    EXPECT_TRUE(ring.try_publish(11).accepted());
    EXPECT_FALSE(ring.try_acquire());
    release.count_down();
    first.join();

    auto zero = ring.try_acquire();
    ASSERT_TRUE(zero);
    EXPECT_EQ(zero.value(), 10u);
    zero.reset();
    auto one = ring.try_acquire();
    ASSERT_TRUE(one);
    EXPECT_EQ(one.value(), 11u);
}
```

- [ ] **Step 2: Add multi-producer exact accounting stress**

Use 8 producers × 50,000 attempts. Encode producer and sequence in each
`uint64_t`. Track accepted/rejected counts and assert every consumed value is
unique, `consumed == accepted`, and `max_depth <= Capacity`. Use a condition
variable or atomic completion flags; do not prove progress with sleeps.

- [ ] **Step 3: Run stress RED then GREEN**

Run before adding the test hook:

```bash
ninja -C build test_unit_mailbox
```

Expected RED: `set_after_claim_hook_for_test` is missing.

Add the hook only under `HPACTOR_TESTING`, after claim and before assignment.
It must return before production publication continues and must not support an
early return.

Run:

```bash
ninja -C build test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='DisruptorMpscRingStressTest.*'
```

Expected GREEN: both deterministic gap and contention tests pass.

- [ ] **Step 4: Add the Relacy model**

Create a two-producer/one-consumer model using the production header through
Relacy's fake `<atomic>` include path. Model capacity 2 and assert:

```cpp
struct DisruptorRingModel : rl::test_suite<DisruptorRingModel, 3> {
    DisruptorMpscRing<uint32_t, 2> ring;
    rl::var<uint32_t> consumed[2];
    rl::var<uint32_t> count;

    void before() { count($) = 0; }
    void thread(unsigned index) {
        if (index < 2) {
            (void)ring.try_publish(index + 1);
            return;
        }
        for (unsigned i = 0; i < 2; ++i) {
            auto lease = ring.try_acquire();
            if (lease)
                consumed[count($)++]($) = lease.value();
        }
    }
    void after() {
        RL_ASSERT(count($) <= 2);
        if (count($) == 2)
            RL_ASSERT(consumed[0]($) != consumed[1]($));
    }
};
```

Register `test_disruptor_relacy` alongside `test_mpsc_relacy` with the same
Relacy include and exception/RTTI overrides. The production target remains
`-fno-exceptions -fno-rtti`.

- [ ] **Step 5: Run Relacy**

```bash
cmake -S . -B build -GNinja \
  -DENABLE_FIXED_DISRUPTOR_MAILBOX=ON \
  -DENABLE_RELACY_TESTS=ON
ninja -C build test_disruptor_relacy
./build/tests/unit/mailbox/test_disruptor_relacy
```

Expected: Relacy completes its configured iteration set with no assertion,
data-race, or livelock report.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/mailbox/disruptor_mpsc_ring.hpp \
  tests/unit/mailbox/test_disruptor_relacy.cpp \
  tests/unit/mailbox/test_disruptor_stress.cpp \
  tests/unit/mailbox/CMakeLists.txt
git commit -m "test: verify disruptor ring concurrency"
```

---

### Task 4: Build the hybrid fixed actor mailbox core

**Files:**
- Create: `include/hpactor/mailbox/fixed_mailbox_ports.hpp`
- Create: `include/hpactor/mailbox/fixed_actor_mailbox.hpp`
- Create: `tests/unit/mailbox/test_fixed_actor_mailbox.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`

**Interfaces:**
- Consumes: fixed envelope and ring from Tasks 1–3; `MPSCMailbox<TypedMessage>`,
  `PressureStateMachine`, `BackpressureSignalGate`, and `EnqueueResult`.
- Produces: `FixedDeliveryPort`, `FixedControlIngressPort`,
  `FixedMailboxExecutionPort`, `FixedMailboxLifecyclePort`,
  `FixedMailboxBinding`, and `FixedActorMailboxCore<Capacity, Messages...>`.
- Consumers: Tasks 5–10.

- [ ] **Step 1: Define ports before the templated core**

Use function-pointer/context ports with no `std::function` on production paths:

```cpp
struct FixedDeliveryPort {
    void* context{nullptr};
    DeliveryPreflightResult (*preflight)(
        void*, const ActorAddress&, const FixedEnvelopeMeta&) noexcept{nullptr};
    void (*record_accepted)(
        void*, ActorId, const FixedDeliveryObservation&) noexcept{nullptr};
    void (*record_rejected)(
        void*, ActorId, const FixedDeliveryFailure&) noexcept{nullptr};
};

struct FixedControlIngressPort {
    void* context{nullptr};
    EnqueueResult (*try_push)(void*, TypedMessage&&) noexcept{nullptr};
};

struct FixedMailboxExecutionPort {
    void* context{nullptr};
    bool (*consume_one)(void*, EventBasedActor&,
                        const sched::ActorExecutionContext&) noexcept{nullptr};
    bool (*empty)(const void*) noexcept{nullptr};
    cli::MboxSnapshot (*snapshot)(const void*) noexcept{nullptr};
};

struct FixedMailboxLifecyclePort {
    void* context{nullptr};
    void (*begin_drain)(void*) noexcept{nullptr};
    void (*drain_immediate)(void*, EventBasedActor&) noexcept{nullptr};
    void (*close)(void*) noexcept{nullptr};
    bool (*publishers_quiescent)(const void*) noexcept{nullptr};
};
```

`FixedMailboxBinding` owns `std::shared_ptr<void> lifetime` plus all three
ports. Empty ports are invalid and must fail spawn validation.

- [ ] **Step 2: Write mailbox RED tests**

Use a mock `IScheduler` and fixed messages `UserA`/`UserB`. Prove:

- user FIFO and variant integrity;
- system-first consumption;
- one notification for a nonempty epoch shared by both lanes;
- exact ring-full rejection with `FailureReason::MailboxFull`;
- closed and draining rejection;
- in-flight publisher quiescence; and
- snapshot backend/capacity/system/user depths.

The wakeup assertion must be:

```cpp
EXPECT_TRUE(core.try_push_user(UserA{1}, {}).accepted());
EXPECT_TRUE(core.try_push_control(make_system_message()).accepted());
EXPECT_EQ(scheduler.notify_count(), 1u);
core.consume_one(actor, exec_context); // system
core.consume_one(actor, exec_context); // user
EXPECT_TRUE(core.empty());
EXPECT_TRUE(core.try_push_user(UserA{2}, {}).accepted());
EXPECT_EQ(scheduler.notify_count(), 2u);
```

- [ ] **Step 3: Run RED**

```bash
ninja -C build test_unit_mailbox
```

Expected: compilation fails because the ports and core are missing.

- [ ] **Step 4: Implement user publication and in-flight admission**

The final admission sequence must be:

```cpp
template <FixedMailboxMessage Message>
EnqueueResult try_push_user(Message message, FixedEnvelopeMeta meta) noexcept {
    PublisherGuard publisher{in_flight_publishers_};
    if (!accepting_user_.load(std::memory_order_acquire))
        return reject(FailureReason::MailboxClosed, meta);

    auto preflight =
        delivery_.preflight(delivery_.context, actor_address_, meta);
    if (!preflight.accepted)
        return reject(preflight.reason, meta);

    envelope_type envelope;
    envelope.message = message;
    envelope.meta = meta;
    auto published = user_ring_.try_publish(std::move(envelope));
    if (!published.accepted())
        return reject(published.closed() ? FailureReason::MailboxClosed
                                         : FailureReason::MailboxFull,
                      meta);
    arm_wakeup_after_publish();
    record_accept(meta, published.sequence);
    return make_accept_result();
}
```

`PublisherGuard` increments before the accepting check and decrements on every
return. When its decrement reaches zero after drain starts, it invokes the
non-blocking ready notification so drain can recheck quiescence.

- [ ] **Step 5: Implement the protected system lane and shared gate**

Allocate system `TypedMessage` nodes from `RegionType::kMessage`, reject
`TypeTag >= TypeTag::User`, enforce `protected_system_messages` atomically, and
reuse `MultiLaneQueue<TypedMessage>` only for its system sentinel and deferred
reclamation.

`consume_one` must acquire one consumer right and execute:

```cpp
if (TypedMessage* system = system_lane_.dequeue()) {
    actor.receive(*system);
    system_lane_.set_pending_free(system);
    return true;
}
if (auto user = user_ring_.try_acquire()) {
    if (is_expired(user->meta.deadline_ns, steady_now_ns()))
        record_expired(*user);
    else
        dispatch_fixed_(actor, user->message, user->meta);
    return true; // lease releases on scope exit
}
return false;
```

Deferred system nodes must be reclaimed only through
`MultiLaneQueue::set_pending_free`/`drain_pending_free`; never destroy the
just-dequeued intrusive node directly while a producer may still hold its link.

Implement one `work_signaled_` exchange after either lane publishes. Clearing
must follow clear → acquire recheck → re-arm/self-notify.

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='FixedActorMailboxTest.*'
```

Expected: all hybrid-core tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/mailbox/fixed_mailbox_ports.hpp \
  include/hpactor/mailbox/fixed_actor_mailbox.hpp \
  tests/unit/mailbox/test_fixed_actor_mailbox.cpp \
  tests/unit/mailbox/CMakeLists.txt
git commit -m "feat: add hybrid fixed actor mailbox core"
```

---

### Task 5: Add the fixed actor behavior and typed reference API

**Files:**
- Create: `include/hpactor/ref/fixed_actor_ref.hpp`
- Create: `include/hpactor/actor/fixed_mailbox_actor.hpp`
- Modify: `include/hpactor/actor/actor_fwd.hpp`
- Modify: `include/hpactor/actor/abstract_actor.hpp`
- Modify: `src/actor/abstract_actor.cpp`
- Create: `tests/unit/actor/test_fixed_mailbox_actor.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`

**Interfaces:**
- Consumes: `FixedActorMailboxCore` and ports from Task 4.
- Produces: `FixedActorRef<Capacity, Messages...>`,
  `FixedBehavior<Messages...>`, and
  `FixedMailboxActor<Capacity, Messages...>`.
- Consumers: Tasks 6–12.

- [ ] **Step 1: Write API RED tests**

Create an actor with two handlers and prove exact typed-reference behavior:

```cpp
struct Increment { uint64_t value{}; };
struct Reset { uint64_t value{}; };
struct Undeclared { uint64_t value{}; };

class CounterActor final
    : public FixedMailboxActor<8, Increment, Reset> {
  public:
    using FixedMailboxActor::FixedMailboxActor;
  protected:
    fixed_behavior_type make_fixed_behavior() override {
        return {
            on_fixed<Increment>(
                [this](const Increment& msg) { total_ += msg.value; }),
            on_fixed<Reset>(
                [this](const Reset& msg) { total_ = msg.value; }),
        };
    }
  private:
    uint64_t total_{0};
};

static_assert(requires(CounterActor::fixed_actor_ref_type ref) {
    ref.try_send(Increment{1});
});
static_assert(!requires(CounterActor::fixed_actor_ref_type ref) {
    ref.try_send(Undeclared{1});
});
```

Also assert duplicate/missing handlers make `create_fixed_mailbox()` return a
typed initialization error before directory publication rather than leaving an
empty function for activation-time dispatch.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_actor
```

Expected: compilation fails because fixed actor/ref types are missing.

- [ ] **Step 3: Add RTTI-free actor hooks**

Add default hooks to `AbstractActor`:

```cpp
virtual mailbox::MailboxKind mailbox_kind() const noexcept {
    return mailbox::MailboxKind::VariableMpsc;
}
virtual result<mailbox::FixedMailboxBinding>
create_fixed_mailbox(const mailbox::FixedMailboxSpawnContext&) noexcept;
virtual mailbox::FixedMailboxExecutionPort fixed_execution_port() noexcept {
    return {};
}
```

The default `create_fixed_mailbox` returns `errors::invalid_argument`. Do not
add RTTI queries.

- [ ] **Step 4: Implement the reference**

`FixedActorRef` has a private constructor used by `FixedMailboxActor` and stores
`ActorAddress` plus `shared_ptr<core_type>`. Its only user delivery API is:

```cpp
template <typename Message>
requires detail::one_of_v<std::remove_cvref_t<Message>, Messages...>
[[nodiscard]] EnqueueResult
try_send(Message&& message, FixedSendOptions options = {}) const noexcept {
    if (!core_) {
        EnqueueResult result;
        result.code = EnqueueResultCode::MailboxClosed;
        result.target = address_.id;
        return result;
    }
    FixedEnvelopeMeta meta;
    meta.deadline_ns = options.deadline_ns;
    meta.message_id = options.message_id;
    meta.flags = options.flags;
    return core_->try_push_user(std::forward<Message>(message), meta);
}
```

No generic `TypedMessage` or remote constructor is public.

- [ ] **Step 5: Implement behavior dispatch**

`FixedBehavior` stores one handler per alternative and validates completeness at
activation. `dispatch(message_type&, meta)` uses `std::visit` and invokes
`const Message&`. `FixedMailboxActor` creates/stores the core in
`create_fixed_mailbox`, returns `FixedDisruptor` from `mailbox_kind()`, and
exposes `fixed_ref()` only after adoption assigned an address.

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build test_unit_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='FixedMailboxActorTest.*'
```

Expected: fixed behavior and compile-time API tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/ref/fixed_actor_ref.hpp \
  include/hpactor/actor/fixed_mailbox_actor.hpp \
  include/hpactor/actor/actor_fwd.hpp \
  include/hpactor/actor/abstract_actor.hpp src/actor/abstract_actor.cpp \
  tests/unit/actor/test_fixed_mailbox_actor.cpp \
  tests/unit/actor/CMakeLists.txt
git commit -m "feat: add fixed mailbox actor API"
```

---

### Task 6: Publish fixed mailboxes through unified spawning and ActorDirectory

**Files:**
- Modify: `include/hpactor/actor/actor_directory.hpp`
- Modify: `src/actor/actor_directory.cpp`
- Modify: `src/runtime/spawn_spec.hpp`
- Modify: `src/runtime/actor_spawner.hpp`
- Modify: `src/runtime/actor_spawner.cpp`
- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/unit/actor/test_actor_directory.cpp`
- Create: `tests/integration/actor/test_fixed_mailbox_delivery.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

**Interfaces:**
- Consumes: actor/ref/core types from Tasks 4–5.
- Produces: atomic fixed binding publication, `find_fixed_binding()`, and
  `ActorSystem::spawn_fixed<T>()`.
- Consumers: Tasks 7–12.

- [ ] **Step 1: Write directory publication RED tests**

Extend `test_actor_directory.cpp` to publish one variable entry and one fixed
entry. Assert `find_mailbox` still returns the variable mailbox only and
`find_fixed_binding` returns only the fixed binding. Verify duplicate-name
rollback publishes neither partial binding nor name.

- [ ] **Step 2: Write spawn RED test**

In `test_fixed_mailbox_delivery.cpp`:

```cpp
ActorSystem::Config cfg;
cfg.scheduler_threads = 0;
cfg.scheduler_start_paused = true;
ActorSystem system(cfg);

auto ref = system.spawn_fixed<CounterActor>();
ASSERT_TRUE(ref);
EXPECT_EQ(ref.address().endpoint, system.endpoint());
EXPECT_TRUE(ref.try_send(Increment{3}).accepted());
EXPECT_TRUE(system.scheduler_test_driver().run_one_ready());
EXPECT_EQ(observed_total.load(), 3u);
```

- [ ] **Step 3: Run RED**

```bash
ninja -C build test_unit_actor test_integration_actor
```

Expected: missing `find_fixed_binding` and `spawn_fixed`.

- [ ] **Step 4: Extend the directory entry without slowing variable lookup**

Use:

```cpp
struct ActorDirectoryEntry {
    Actor actor;
    std::shared_ptr<AbstractActor> instance;
    mailbox::MailboxKind mailbox_kind{mailbox::MailboxKind::VariableMpsc};
    std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>> mailbox;
    mailbox::FixedMailboxBinding fixed_mailbox;
    std::shared_ptr<ActorContext> context;
};
```

`find_mailbox` remains a direct concrete lookup. Add
`std::optional<FixedMailboxBinding> find_fixed_binding(ActorId) const`. The
existing mutex makes publication atomic.

- [ ] **Step 5: Branch mailbox construction inside ActorSpawner**

Extend dependencies with immutable `FixedDeliveryPort fixed_delivery`. In
`adopt`:

```cpp
if (actor->mailbox_kind() == MailboxKind::VariableMpsc) {
    // Preserve the existing mailbox construction and injection unchanged.
} else {
    FixedMailboxSpawnContext spawn_ctx{
        .actor_id = id,
        .actor_address = actor->address(),
        .scheduler = &deps_.scheduler,
        .delivery = deps_.fixed_delivery,
        .metrics = deps_.metrics,
        .logger = deps_.logger,
        .config = FixedMailboxRuntimeConfig::from_general(spec.mailbox),
    };
    auto created = actor->create_fixed_mailbox(spawn_ctx);
    if (created.is_error())
        return result<Actor>::make(created.error_value());
    entry.fixed_mailbox = std::move(created.value());
    entry.mailbox_kind = MailboxKind::FixedDisruptor;
}
```

Validate nonempty ports, checked ring bytes, memory-region admission, and
`RejectNewest`. For `SpawnOrigin::Topology`, reject explicit priority-aware,
overflow-depth, non-RejectNewest, or mismatched capacity settings. Do not copy
the global default priority-level count into the fixed core.

- [ ] **Step 6: Add `spawn_fixed`**

```cpp
template <typename T, typename... Args>
requires FixedMailboxActorType<T>
typename T::fixed_actor_ref_type
ActorSystem::spawn_fixed(Args&&... args) {
    auto actor = std::make_shared<T>(nullptr, *this,
                                     std::forward<Args>(args)...);
    Actor adopted;
    if constexpr (requires { T::kActorTypeName; })
        adopted = adopt_preconstructed_actor(actor, T::kActorTypeName);
    else
        adopted = adopt_preconstructed_actor(actor, "unknown");
    if (!adopted)
        return {};
    return actor->fixed_ref();
}
```

Do not route `spawn<T>` automatically; explicit opt-in keeps source behavior
obvious.

- [ ] **Step 7: Wire spawner dependencies after MessagingRuntime exists**

The early spawner instance receives an empty fixed port and may adopt variable
system actors only. The reconstructed spawner after `messaging_` creation uses
`impl_->messaging_->fixed_delivery_port()`. Add an assertion that fixed adoption
with an empty port returns a typed spawn error.

- [ ] **Step 8: Run GREEN**

```bash
ninja -C build test_unit_actor test_integration_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='*ActorDirectory*Fixed*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='FixedMailboxDeliveryTest.SpawnAndDeliverLocally'
```

Expected: all selected tests pass.

- [ ] **Step 9: Commit**

```bash
git add include/hpactor/actor/actor_directory.hpp \
  src/actor/actor_directory.cpp src/runtime/spawn_spec.hpp \
  src/runtime/actor_spawner.hpp src/runtime/actor_spawner.cpp \
  include/hpactor/actor/actor_system.hpp src/actor/actor_system.cpp \
  tests/unit/actor/test_actor_directory.cpp \
  tests/integration/actor/test_fixed_mailbox_delivery.cpp \
  tests/integration/actor/CMakeLists.txt
git commit -m "feat: adopt fixed mailbox actors"
```

---

### Task 7: Integrate fixed delivery with MessagingRuntime and ActorContext

**Files:**
- Modify: `src/runtime/messaging_runtime.hpp`
- Modify: `src/runtime/messaging_runtime.cpp`
- Create: `src/runtime/fixed_mailbox_delivery.cpp`
- Modify: `src/runtime/CMakeLists.txt`
- Modify: `include/hpactor/mailbox/local_delivery_engine.hpp`
- Modify: `src/mailbox/local_delivery_engine.cpp`
- Modify: `include/hpactor/actor/actor_context.hpp`
- Modify: `include/hpactor/msg/enqueue_result.hpp`
- Modify: `include/hpactor/msg/failure_reason.hpp`
- Modify: `src/msg/failure_reason.cpp`
- Create: `tests/unit/core/test_fixed_mailbox_failure_reason.cpp`
- Modify: `tests/unit/core/CMakeLists.txt`
- Modify: `tests/integration/actor/test_fixed_mailbox_delivery.cpp`

**Interfaces:**
- Consumes: `FixedDeliveryPort` and directory binding from Tasks 4 and 6.
- Produces: `MessagingRuntime::fixed_delivery_port()` and
  `ActorContext::try_send(FixedActorRef, Message, FixedSendOptions)`.
- Consumers: Tasks 8–12.

- [ ] **Step 1: Add failure vocabulary RED tests**

Reserve unused stable numeric values:

```cpp
EXPECT_EQ(static_cast<uint8_t>(
              FailureReason::FixedMailboxRemoteUnsupported), 5u);
EXPECT_EQ(static_cast<uint8_t>(
              FailureReason::UnsupportedMessageType), 54u);
EXPECT_EQ(to_string(FailureReason::FixedMailboxRemoteUnsupported),
          "fixed_mailbox_remote_unsupported");
EXPECT_FALSE(retryable(FailureReason::UnsupportedMessageType));
EnqueueResult result;
result.code = EnqueueResultCode::Rejected;
result.reason_override = FailureReason::UnsupportedMessageType;
EXPECT_EQ(result.failure_reason(), FailureReason::UnsupportedMessageType);
```

Do not add a duplicate fixed-ring-full reason.

- [ ] **Step 2: Add delivery preflight RED tests**

Extend the integration fixture to cover local acceptance, expired-before-claim,
quarantined, draining, closed, full, and a dynamic user `TypedMessage` sent to a
fixed actor. Assert the exact failure reason and unchanged ring depth.

- [ ] **Step 3: Run RED**

```bash
ninja -C build test_unit_core test_integration_actor
```

Expected: new failure values and fixed delivery port are missing.

- [ ] **Step 4: Extend EnqueueResult with a canonical override**

Add `FailureReason reason_override{FailureReason::Unknown}`. Preserve existing
callers by changing only the accessor:

````cpp
[[nodiscard]] FailureReason failure_reason() const noexcept {
    return reason_override != FailureReason::Unknown
               ? reason_override
               : mailbox::failure_reason(code);
}
````

Fixed rejection helpers set both the closest `EnqueueResultCode` and the exact
override. Variable-mailbox results leave it `Unknown` and retain their current
mapping.

- [ ] **Step 5: Implement fixed preflight and outcome adapters**

`MessagingRuntime::fixed_delivery_port()` returns callbacks whose context is the
stable runtime. `preflight_fixed` must:

1. reject a nonlocal address as `FixedMailboxRemoteUnsupported`;
2. find the same directory entry/incarnation;
3. reject terminated/draining/recovering states;
4. apply quarantine/circuit admission;
5. reject an already-expired deadline; and
6. return accepted without touching message bytes.

`record_fixed_accepted` emits `kMailboxEnqueue` with
`MetricEvent::aux = FixedDisruptor`. `record_fixed_rejected` emits delivery
failure, rate-limited structured logging, pressure/backoff, and optional
metadata-only DLQ evidence.

- [ ] **Step 6: Route TypedMessage control delivery correctly**

Change `LocalDeliveryEngine::try_deliver` to load the full directory entry once.
Keep the concrete MPSC call for `VariableMpsc`. For `FixedDisruptor`:

````cpp
if (static_cast<uint32_t>(msg->type_id()) >=
    static_cast<uint32_t>(TypeTag::User)) {
    EnqueueResult result;
    result.code = EnqueueResultCode::Rejected;
    result.target = target;
    result.reason_override = FailureReason::UnsupportedMessageType;
    return result;
}
return entry->fixed_mailbox.control.try_push(
    entry->fixed_mailbox.control.context, std::move(*msg));
````

This makes lifecycle, supervision, and CLI control traffic reach the protected
lane and prevents dynamic user traffic from being misreported as `NoRoute`.

- [ ] **Step 7: Implement ActorContext metadata propagation**

```cpp
template <size_t Capacity, FixedMailboxMessage... Messages, typename Message>
requires detail::one_of_v<std::remove_cvref_t<Message>, Messages...>
EnqueueResult ActorContext::try_send(
    const FixedActorRef<Capacity, Messages...>& target,
    Message&& message, FixedSendOptions options) noexcept {
    FixedEnvelopeMeta meta;
    meta.sender = self().address();
    meta.deadline_ns = options.deadline_ns;
    meta.message_id = options.message_id != 0
                          ? options.message_id
                          : generate_message_id().value();
    meta.flags = options.flags;
    if (has_current_trace_context()) {
        meta.trace = current_trace_context();
        meta.has_trace = true;
    }
    return target.try_send_with_meta(std::forward<Message>(message), meta);
}
```

The public reference keeps `try_send_with_meta` private and befriends
`ActorContext`.

- [ ] **Step 8: Run GREEN**

```bash
ninja -C build test_unit_core test_integration_actor
./build/tests/unit/core/test_unit_core \
  --gtest_filter='FixedMailboxFailureReasonTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='FixedMailboxDeliveryTest.*'
```

Expected: all fixed delivery cases pass.

- [ ] **Step 9: Commit**

```bash
git add src/runtime/messaging_runtime.hpp src/runtime/messaging_runtime.cpp \
  src/runtime/fixed_mailbox_delivery.cpp src/runtime/CMakeLists.txt \
  include/hpactor/mailbox/local_delivery_engine.hpp \
  src/mailbox/local_delivery_engine.cpp \
  include/hpactor/actor/actor_context.hpp \
  include/hpactor/msg/enqueue_result.hpp \
  include/hpactor/msg/failure_reason.hpp src/msg/failure_reason.cpp \
  tests/unit/core/test_fixed_mailbox_failure_reason.cpp \
  tests/unit/core/CMakeLists.txt \
  tests/integration/actor/test_fixed_mailbox_delivery.cpp
git commit -m "feat: route fixed mailbox delivery semantics"
```

---

### Task 8: Execute fixed actors through the scheduler without lost wakeups

**Files:**
- Modify: `include/hpactor/sched/actor_execution_engine.hpp`
- Modify: `src/sched/actor_execution_engine.cpp`
- Create: `src/sched/fixed_mailbox_actor_runner.cpp`
- Modify: `src/sched/CMakeLists.txt`
- Create: `tests/integration/actor/test_fixed_mailbox_scheduler.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

**Interfaces:**
- Consumes: fixed execution port and directory mailbox kind.
- Produces: `FixedMailboxActorRunner::run()` and one backend branch per
  activation.
- Consumers: lifecycle and end-to-end verification.

- [ ] **Step 1: Write deterministic scheduler RED tests**

Use `scheduler_threads = 0` and paused-worker controls. Cover:

- one publish creates one ready work item;
- 64-message requeue budget yields to a variable actor;
- user publication during gate-clear/recheck is not lost;
- system publication during gate-clear/recheck is not lost; and
- concurrent producers never cause duplicate handler execution.

Instrument the core with a test-only gate-clear hook. Drive the producer at
each boundary with latches, not sleeps.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_integration_actor
```

Expected: the existing `BehaviorActorRunner` looks only for the variable
mailbox and fixed messages remain undispatched.

- [ ] **Step 3: Extract common requeue/idle helpers**

Keep the existing `Ready -> Running` CAS and requeue constants. Add private
helpers in `actor_execution_engine.cpp`:

```cpp
ActorRunResult requeue_or_idle(EventBasedActor& actor,
                               const WorkItem& item,
                               bool has_work,
                               bool workers_paused,
                               ActorReadyGate& ready_gate) noexcept;

ActorRunResult recover_empty_race(EventBasedActor& actor,
                                  const WorkItem& item,
                                  bool has_work,
                                  ActorReadyGate& ready_gate) noexcept;
```

First prove existing behavior-runner tests remain green before adding fixed
selection.

- [ ] **Step 4: Add the fixed runner**

`FixedMailboxActorRunner::run`:

1. wins `Ready -> Running`;
2. fetches the fixed execution port from the actor/directory;
3. calls `consume_one` once;
4. uses the common requeue budget when `port.empty(context) == false`; and
5. uses clear/recheck/idle recovery when no consumable message is visible.

Before fixed user dispatch, install the envelope trace as the actor context's
current trace scope, set the current sender metadata, and emit the same
processing-latency boundary as `BehaviorActorRunner`. Clear both scopes before
the read lease releases. Use a stack/optional RAII scope, never the variable
path's per-message `make_unique<TraceScope>`; the fixed user path must retain
its zero-allocation contract. This preserves correlation without enabling the
unsupported ask/reply API.

`ActorExecutionEngine::run` branches:

```cpp
if (actor.mailbox_kind() == mailbox::MailboxKind::FixedDisruptor)
    return fixed_runner_.run(actor, item, context);
#if HPACTOR_SUPPORT_COROUTINES
if (use_coroutines)
    return coroutine_runner_.run(actor, item, context);
#else
(void)use_coroutines;
#endif
return behavior_runner_.run(actor, item, context);
```

Coroutines remain unsupported for fixed actors; reject that actor declaration
at compile time or spawn validation.

- [ ] **Step 5: Run GREEN and compatibility checks**

```bash
ninja -C build test_integration_actor test_unit_sched
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='FixedMailboxSchedulerTest.*'
./build/tests/unit/sched/test_unit_sched \
  --gtest_filter='*ActorExecution*:*ReadyGate*'
```

Expected: fixed scheduler tests and existing execution/ready-gate tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/sched/actor_execution_engine.hpp \
  src/sched/actor_execution_engine.cpp \
  src/sched/fixed_mailbox_actor_runner.cpp src/sched/CMakeLists.txt \
  tests/integration/actor/test_fixed_mailbox_scheduler.cpp \
  tests/integration/actor/CMakeLists.txt
git commit -m "feat: schedule fixed mailbox actors"
```

---

### Task 9: Implement drain, close, expiry, and metadata-only DLQ behavior

**Files:**
- Modify: `include/hpactor/actor/event_based_actor.hpp`
- Modify: `src/actor/event_based_actor.cpp`
- Modify: `include/hpactor/actor/fixed_mailbox_actor.hpp`
- Modify: `include/hpactor/mailbox/fixed_actor_mailbox.hpp`
- Modify: `include/hpactor/msg/dead_letter_record.hpp`
- Create: `tests/integration/actor/test_fixed_mailbox_lifecycle.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

**Interfaces:**
- Consumes: lifecycle port and fixed actor runner.
- Produces: complete/immediate/timeout drain semantics, close-before-destroy,
  and non-replayable fixed DLQ records.

- [ ] **Step 1: Write lifecycle RED tests**

Cover:

- begin drain rejects new user sends but accepts system control;
- a sender already inside `PublisherGuard` completes and joins the drain set;
- complete drain dispatches all preexisting valid user messages;
- immediate drain invokes no user handler and records each drop;
- timeout drain records remaining messages;
- consumer-side expiry records failure without handler invocation;
- actor destruction closes the core before a surviving reference sends; and
- last-reference destruction releases the ring's memory-region charge.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_integration_actor
```

Expected: immediate drain only drains `EventBasedActor::mailbox_` and fixed
admission remains open.

- [ ] **Step 3: Make immediate drain virtual and override it**

Change:

```cpp
virtual void drain_all_immediate();
```

`FixedMailboxActor::drain_all_immediate()` calls the lifecycle port, which:

- `accepting_user_.store(false, release)`;
- waits asynchronously for `in_flight_publishers_ == 0`;
- consumes/releases system nodes and user leases without user dispatch; and
- emits metadata-only dead-letter records.

The existing timer callback calls the virtual method through
`EventBasedActor*`, so timeout drain reaches the fixed override.

Override `FixedMailboxActor::on_exit()` to invoke the lifecycle port's
`close` callback before delegating to `EventBasedActor::on_exit()`. The
close callback rejects new publishers immediately; directory removal and actor
destruction therefore cannot race a newly accepted user claim.

- [ ] **Step 4: Extend dead-letter metadata**

Add:

```cpp
bool replayable{true};
bool fixed_message{false};
uint16_t fixed_type_index{0};
std::string fixed_type_name;
```

Fixed records set `replayable = false`, leave `payload_sample` empty, and record
`payload_size = sizeof(Message)`. Update DLQ replay to reject non-replayable
records with a typed result; do not reinterpret object bytes.

- [ ] **Step 5: Run GREEN**

```bash
ninja -C build test_integration_actor test_unit_mailbox
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='FixedMailboxLifecycleTest.*'
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='*DeadLetter*'
```

Expected: fixed lifecycle tests and existing DLQ tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/actor/event_based_actor.hpp \
  src/actor/event_based_actor.cpp \
  include/hpactor/actor/fixed_mailbox_actor.hpp \
  include/hpactor/mailbox/fixed_actor_mailbox.hpp \
  include/hpactor/msg/dead_letter_record.hpp \
  tests/integration/actor/test_fixed_mailbox_lifecycle.cpp \
  tests/integration/actor/CMakeLists.txt
git commit -m "feat: add fixed mailbox lifecycle semantics"
```

---

### Task 10: Add snapshots, metrics, CLI, configuration validation, and architecture guards

**Files:**
- Modify: `include/hpactor/cli/cli_types.hpp`
- Modify: `src/cli/commands/actor_commands.cpp`
- Modify: `include/hpactor/metrics/metrics_event.hpp`
- Modify: `src/metrics/metrics_aggregator.cpp`
- Modify: `src/actor/actor_system.cpp`
- Create: `tests/integration/actor/test_fixed_mailbox_observability.cpp`
- Modify: `tests/unit/cli/test_actor_commands.cpp`
- Modify: `tests/integration/config/test_mailbox_config.cpp`
- Modify: `tests/architecture/CMakeLists.txt`

**Interfaces:**
- Consumes: `FixedRingSnapshot` and fixed core counters.
- Produces: stable operational visibility and architecture fitness checks.

- [ ] **Step 1: Write snapshot and metric RED tests**

Assert a quiesced fixed mailbox reports:

```cpp
EXPECT_EQ(snapshot.backend, "fixed_disruptor");
EXPECT_EQ(snapshot.capacity, 8u);
EXPECT_EQ(snapshot.fixed_slot_bytes, sizeof(envelope_type));
EXPECT_EQ(snapshot.depth, 3u);
EXPECT_EQ(snapshot.fixed_claim_retries, expected_retries);
EXPECT_EQ(snapshot.fixed_gap_observations, expected_gaps);
EXPECT_EQ(snapshot.system_lane_depth, 1u);
```

Assert accepted/rejected metric events set
`aux == static_cast<uint8_t>(MailboxKind::FixedDisruptor)`.

- [ ] **Step 2: Add CLI and config RED tests**

`/actor <id> show` must display backend, capacity, slot bytes, user/system depth,
claim retries, gap observations, rejections, and pressure.

Topology validation cases:

- priority-aware true → validation error;
- overflow policy other than RejectNewest → validation error;
- nonzero overflow depth → validation error;
- explicit mailbox capacity different from actor capacity → validation error;
- global priority-level default 4 with priority-aware false → accepted and
  normalized to one user ring.

- [ ] **Step 3: Run RED**

```bash
ninja -C build test_integration_actor test_unit_cli test_integration_config
```

Expected: new snapshot fields and backend metrics are missing.

- [ ] **Step 4: Extend snapshot and metrics**

Append fields without changing existing field meaning:

```cpp
std::string backend{"variable_mpsc"};
uint32_t fixed_slot_bytes{0};
uint64_t fixed_claim_retries{0};
uint64_t fixed_gap_observations{0};
uint64_t fixed_claimed_not_published{0};
```

Add metric types after the current maximum:

```cpp
kFixedMailboxClaimRetry = 70,
kFixedMailboxProducerGap = 71,
kFixedMailboxDepth = 72,
```

Update the aggregator with named counters/gauges and a `mailbox_backend` label.
Do not alter `MetricEvent` size; encode backend in `aux` and values in
`value_hi`.

- [ ] **Step 5: Add architecture tests**

Apply the no-RTTI/no-exception set to every fixed production header/source:

```cmake
set(_fixed_mailbox_no_rtti
    "dynamic_cast" "typeid" "throw " "try {" "catch (")
```

Apply a second no-facade-capture set only to the mailbox core, ports,
MessagingRuntime adapter, and scheduler runner:

```cmake
set(_fixed_mailbox_no_facade "ActorSystem*" "ActorSystem&" "Impl*" "Impl&")
```

Do not apply the no-facade set to `FixedMailboxActor`; like every local actor,
its constructor legitimately receives `ActorSystem&` without storing a new
runtime-facade dependency in the mailbox core.

Add a separate check rejecting late dependency setters:
`set_delivery_port(`, `set_scheduler(`, and `set_metrics(` in fixed core files.
Test-only hooks under `HPACTOR_TESTING` are exempt only by exact name.

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build test_integration_actor test_unit_cli test_integration_config
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='FixedMailboxObservabilityTest.*'
./build/tests/unit/cli/test_unit_cli \
  --gtest_filter='*FixedMailbox*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='*FixedMailbox*'
ctest --test-dir build -R 'FixedMailbox|Architecture' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/cli/cli_types.hpp \
  src/cli/commands/actor_commands.cpp \
  include/hpactor/metrics/metrics_event.hpp \
  src/metrics/metrics_aggregator.cpp src/actor/actor_system.cpp \
  tests/integration/actor/test_fixed_mailbox_observability.cpp \
  tests/unit/cli/test_actor_commands.cpp \
  tests/integration/config/test_mailbox_config.cpp \
  tests/architecture/CMakeLists.txt
git commit -m "feat: expose fixed mailbox operations data"
```

---

### Task 11: Add negative compile checks and zero-allocation integration evidence

**Files:**
- Create: `tests/compile/CMakeLists.txt`
- Create: `tests/compile/fixed_mailbox_valid.cpp`
- Create: `tests/compile/fixed_mailbox_invalid_capacity.cpp`
- Create: `tests/compile/fixed_mailbox_invalid_message.cpp`
- Create: `tests/compile/fixed_mailbox_undeclared_send.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/integration/actor/test_fixed_mailbox_delivery.cpp`

**Interfaces:**
- Consumes: complete public fixed actor API.
- Produces: compile-contract evidence and allocation-free hot-path evidence.

- [ ] **Step 1: Add compile fixtures**

`fixed_mailbox_valid.cpp` must compile a `FixedMailboxActor<1024, Quote>` and
declared send. Each invalid fixture must contain exactly one violation:

```cpp
// invalid_capacity
using BadCapacity = FixedMailboxActor<3, Quote>;

// invalid_message
using BadMessage = FixedMailboxActor<8, std::string>;

// undeclared_send
void invalid_send(CounterActor::fixed_actor_ref_type ref) {
    (void)ref.try_send(Undeclared{});
}
```

`tests/compile/CMakeLists.txt` runs `try_compile` for each file with C++20 and
the project include/generated include paths. The valid fixture must succeed;
each invalid fixture must fail. A surprising success is `FATAL_ERROR`.

- [ ] **Step 2: Run compile checks**

```bash
cmake -S . -B build -GNinja -DENABLE_FIXED_DISRUPTOR_MAILBOX=ON
```

Expected: configure succeeds and reports all expected compile outcomes.

- [ ] **Step 3: Write the allocation RED test**

Install the existing memory telemetry counter after actor spawn, send and
consume 100,000 fixed user messages with the scheduler test driver, then assert:

```cpp
EXPECT_EQ(after.message_allocations - before.message_allocations, 0u);
EXPECT_EQ(after.message_frees - before.message_frees, 0u);
EXPECT_EQ(consumed.load(), accepted);
```

Send one system-control message separately and assert its allocation is not
counted as a user-path regression.

- [ ] **Step 4: Run allocation RED/GREEN**

RED is expected if any current implementation constructs a heap-backed wrapper
or allocates per dispatch:

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='FixedMailboxDeliveryTest.UserHotPathAllocatesNothing'
```

Remove every user-path allocation. Preallocate behavior tables, slots, and
diagnostic storage at spawn. Re-run the same command.

Expected GREEN: zero user-path allocations/frees and exact consumption.

- [ ] **Step 5: Commit**

```bash
git add tests/compile tests/CMakeLists.txt \
  tests/integration/actor/test_fixed_mailbox_delivery.cpp
git commit -m "test: enforce fixed mailbox compile and allocation contracts"
```

---

### Task 12: Add the comparative mailbox benchmark

**Files:**
- Create: `apps/bench_fixed_mailbox/CMakeLists.txt`
- Create: `apps/bench_fixed_mailbox/19_bench_fixed_mailbox.cpp`
- Create: `apps/bench_fixed_mailbox/README.md`
- Modify: `apps/CMakeLists.txt`

**Interfaces:**
- Consumes: fixed actor/ref API and existing variable MPSC actor API.
- Produces: machine-readable comparative evidence for the design's release gate.

- [ ] **Step 1: Define the benchmark CLI and result schema**

Support:

```text
--backend fixed|mpsc|both
--payload-bytes 32|64|128|256
--producers 1|2|4|8
--capacity 1024|65536
--messages <count>
--workload steady|burst
--runs 7
--scheduler-threads <count>
--observability on|off
--output text|json
```

JSON output fields must include backend, accepted, rejected, throughput,
enqueue p50/p95/p99, end-to-end p50/p95/p99, claim retries, gap observations,
CPU time, fairness ratio, allocations per accepted message, mailbox bytes,
compiler, flags, OS, CPU, capacity, producer count, and scheduler threads.

- [ ] **Step 2: Implement equivalent actors**

Use templated POD payloads of exact requested size:

```cpp
template <size_t Bytes>
struct BenchMessage {
    uint64_t sequence{};
    uint64_t started_ns{};
    std::array<std::byte, Bytes - 16> payload{};
};
static_assert(sizeof(BenchMessage<64>) == 64);
```

The fixed actor receives `BenchMessage<N>` directly. The MPSC actor receives the
same bytes in an inline `TypedMessage`. Both increment the same cache-line
isolated counters and record latency through preallocated histograms. Keep
priority disabled and `RejectNewest` in both paths.

- [ ] **Step 3: Build and smoke-test the matrix**

```bash
ninja -C build 19_bench_fixed_mailbox
./build/apps/bench_fixed_mailbox/19_bench_fixed_mailbox \
  --backend both --payload-bytes 64 --producers 1 \
  --capacity 1024 --messages 10000 --workload steady \
  --runs 1 --output json
```

Expected: one valid JSON record per backend, accepted + rejected equals attempts,
and fixed reports zero allocations per accepted user message.

- [ ] **Step 4: Document the seven-run release command**

`README.md` must provide:

```bash
for producers in 1 2 4 8; do
  ./build/apps/bench_fixed_mailbox/19_bench_fixed_mailbox \
    --backend both --payload-bytes 64 --producers "${producers}" \
    --capacity 65536 --messages 5000000 --workload steady \
    --runs 7 --observability off --output json
done
```

Document the exact gates: +20% throughput at 1 producer, +10% at 4 producers,
no worse p99 enqueue latency, zero per-message allocation, and less than 5%
mixed-workload fairness regression. Results are review artifacts, never CI
timing assertions.

- [ ] **Step 5: Commit**

```bash
git add apps/CMakeLists.txt apps/bench_fixed_mailbox
git commit -m "perf: add fixed mailbox comparison benchmark"
```

---

### Task 13: Final cross-cutting verification and project memory update

**Files:**
- Modify: `CLAUDE_MEMORY.md`
- Modify: `HPACTOR_PROJECT_OUTLINE.md`
- Modify: `docs/superpowers/specs/2026-06-30-fixed-disruptor-actor-mailbox-design.md`

**Interfaces:**
- Consumes: every implementation task.
- Produces: verified release evidence and accurate project documentation.

- [ ] **Step 1: Run formatting and forbidden-token checks**

```bash
git diff --check
if rg -n 'dynamic_cast|typeid|throw |try \\{|catch \\(' \
  include/hpactor/mailbox/disruptor_mpsc_ring.hpp \
  include/hpactor/mailbox/fixed_actor_mailbox.hpp \
  include/hpactor/actor/fixed_mailbox_actor.hpp \
  src/runtime/fixed_mailbox_delivery.cpp \
  src/sched/fixed_mailbox_actor_runner.cpp; then
  exit 1
fi
```

Expected: `git diff --check` is clean and `rg` returns no production matches.

- [ ] **Step 2: Run focused correctness suites**

```bash
ninja -C build \
  test_unit_mailbox test_unit_actor test_unit_core test_unit_sched \
  test_integration_actor test_unit_cli test_integration_config \
  test_disruptor_relacy
./build/tests/unit/mailbox/test_disruptor_relacy
ctest --test-dir build \
  -R 'FixedMailbox|Disruptor|ActorExecution|ReadyGate|DeadLetter|FailureReason' \
  --output-on-failure
```

Expected: every selected test passes with zero failures.

- [ ] **Step 3: Run full build and test because public actor/runtime headers changed**

```bash
cmake -S . -B build -GNinja \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_FIXED_DISRUPTOR_MAILBOX=ON \
  -DENABLE_RELACY_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure --parallel 8
```

Expected: full build succeeds and CTest reports zero new failures. Any
pre-existing platform exclusion must be identified by exact test name and
reproduced on the base commit before being classified as pre-existing.

- [ ] **Step 4: Run sanitizers where supported**

Use separate worktree-local build directories:

```bash
cmake -S . -B build-asan -GNinja \
  -DENABLE_FIXED_DISRUPTOR_MAILBOX=ON -DENABLE_ASAN=ON \
  -DENABLE_APPS=OFF -DENABLE_EXAMPLES=OFF
ninja -C build-asan test_unit_mailbox test_integration_actor
ctest --test-dir build-asan -R 'FixedMailbox|Disruptor' --output-on-failure

cmake -S . -B build-tsan -GNinja \
  -DENABLE_FIXED_DISRUPTOR_MAILBOX=ON -DENABLE_TSAN=ON \
  -DENABLE_APPS=OFF -DENABLE_EXAMPLES=OFF
ninja -C build-tsan test_unit_mailbox test_integration_actor
ctest --test-dir build-tsan -R 'FixedMailbox|Disruptor' --output-on-failure
```

Expected: zero sanitizer findings. If the documented macOS ARM sanitizer issue
blocks execution, record the configure/build/test output and do not claim a
sanitizer pass.

- [ ] **Step 5: Run the canonical performance evidence**

Run the seven-run 64-byte matrix for 1 and 4 producers plus the mixed
fixed/MPSC fairness workload. Save JSON under an ignored
`build/bench-fixed-mailbox/` directory. Compare medians with a checked-in or
documented analysis command that exits nonzero when a release gate fails.

Expected: either all five design thresholds pass, or the feature remains
experimental/default-OFF and the exact failed thresholds are recorded.

- [ ] **Step 6: Update project memory without overstating status**

Add the implemented API, concurrency contract, test counts, benchmark result,
and remaining non-goals. Mark the feature:

- `Experimental` if correctness passes but any performance gate is missing or
  fails;
- `Complete (opt-in)` only if every correctness, sanitizer-where-supported, and
  benchmark gate passes.

Update the design `Status` line to match the evidence.

- [ ] **Step 7: Commit final evidence and docs**

```bash
git add CLAUDE_MEMORY.md HPACTOR_PROJECT_OUTLINE.md \
  docs/superpowers/specs/2026-06-30-fixed-disruptor-actor-mailbox-design.md
git commit -m "docs: record fixed mailbox verification evidence"
```

- [ ] **Step 8: Verify branch cleanliness**

```bash
git status --short --branch
git log --oneline --decorate -13
```

Expected: clean worktree on the task branch with the task commits visible.

## Requirement-to-Task Traceability

| Design requirement | Implemented by |
|---|---|
| Opt-in/default-off backend | Tasks 1, 5, 6 |
| Compile-time closed fixed messages | Tasks 1, 5, 11 |
| Preallocated Disruptor ring | Tasks 2, 3 |
| Protected system MPSC lane | Task 4 |
| Shared edge-trigger/no lost wakeup | Tasks 4, 8 |
| Single-consumer read lease | Tasks 2, 4, 8 |
| Local-only typed reference | Tasks 5, 7, 11 |
| RejectNewest/full accounting | Tasks 4, 7 |
| Lifecycle/quarantine/deadline | Tasks 7, 9 |
| Trace/sender/message ID | Task 7 |
| Metadata-only non-replayable DLQ | Task 9 |
| Snapshot/metrics/CLI/config | Task 10 |
| No per-message allocation | Task 11 |
| Relacy/stress/sanitizers | Tasks 3, 13 |
| Comparative performance gates | Tasks 12, 13 |
| Existing MPSC path unchanged | Tasks 6, 8, 13 |
