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

# Python Binding Phase 1A Native Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the exception-free, RTTI-free native bridge foundation for bounded Python actor dispatch and command handoff without linking CPython or exposing a public Python actor API.

**Architecture:** A separate `hpactor_python_native` static library owns value-only envelopes, three bounded queues, POSIX notifiers, lifecycle/generation state, `PythonBridgeActor`, and `PythonGatewayActor`. It reuses HPActor's `DynamicMpscRingBuffer`, normal mailbox delivery, actor ready gate, and `ActorSpawner`; scheduler workers never call Python and native queues contain no `PyObject*`.

**Tech Stack:** C++20, HPActor actor/runtime APIs, `adt::DynamicMpscRingBuffer`, Linux `eventfd`, macOS non-blocking `socketpair`, GoogleTest, CMake/Ninja.

## Global Constraints

- Work only in `.claude/worktrees/python-binding-design` on `worktree/python-binding-design`.
- Phase 1A contains no `Python.h`, `PyObject`, CPython API, asyncio code, wheel packaging, or Python source package.
- Compile every new C++ translation unit with `-fno-exceptions` and `-fno-rtti`; do not add an exception allowlist.
- Use C++20 and the repository's LLVM-style formatting and Apache 2.0 headers.
- `ENABLE_PYTHON_BINDINGS` defaults to `OFF`; existing builds remain source-compatible.
- Supported native notifier platforms are Linux x86_64/ARM64 and macOS x86_64/ARM64. Do not add Windows fallback behavior.
- Dispatch, command, and completion capacities are powers of two from 64 through 1,048,576.
- `max_actor_bindings` is from 1 through 1,048,576; the default is 65,536.
- Per-turn drain budgets are from 1 through 4,096 and cannot exceed their queue capacity.
- Queue items own value metadata and shared immutable payload envelopes; failed enqueue retains producer ownership.
- Preserve sender, trace context, priority, deadline, message ID, ask ID, delivery flags, ACK request, actor ID, and actor generation.
- Use internal local-only tags `0xF0` through `0xF3`; reject those tags from remote-frame entry points in a subsequent wire-hardening phase, not by changing the Phase 1A wire format.
- Python actor bridge state is private; scheduler, notifier, and test callbacks must not mutate Python-side state.
- Tests must be deterministic: paused workers, explicit scheduler stepping, non-blocking descriptors, and no sleeps as proof.
- Full configure/build/test verification is required because this phase changes CMake and the public `TypedMessage` header.

## File Structure

### Shared runtime changes

- `include/hpactor/adt/mpsc_ring_buffer.hpp` — add bounded `drain_up_to()` to the existing compile-time and dynamic MPSC rings.
- `include/hpactor/msg/typed_message.hpp` — preserve delivery priority and flags after mailbox admission.
- `src/mailbox/delivery_pipeline.cpp` — stamp priority and flags before moving a message into a mailbox.
- `include/hpactor/msg/failure_reason.hpp` / `src/types/failure_reason.cpp` — append `FailureSource::LanguageBinding = 12` without renumbering existing values.

### Native binding foundation

- `bindings/python/native/CMakeLists.txt` — `hpactor_python_native` static target, PIC, includes, and links.
- `bindings/python/native/include/hpactor/python/python_type_tags.hpp` — reserved local control tags.
- `bindings/python/native/include/hpactor/python/python_runtime_config.hpp` — validated immutable capacity and budget values.
- `bindings/python/native/include/hpactor/python/python_bridge_types.hpp` — value-only dispatch, command, completion, and execution-result types.
- `bindings/python/native/include/hpactor/python/python_runtime_snapshot.hpp` — lifecycle, queue, notifier, and actor-binding snapshot values.
- `bindings/python/native/include/hpactor/python/python_runtime_queues.hpp` — bounded queue ownership and accounting.
- `bindings/python/native/include/hpactor/python/native_notifier.hpp` / `src/native_notifier.cpp` — Linux/macOS non-blocking notifier.
- `bindings/python/native/include/hpactor/python/python_ports.hpp` — fixed function-pointer/context ports.
- `bindings/python/native/include/hpactor/python/python_runtime.hpp` / `src/python_runtime.cpp` — lifecycle, queue/notifier composition, and bounded actor leases.
- `bindings/python/native/include/hpactor/python/python_bridge_actor.hpp` / `src/python_bridge_actor.cpp` — inbound actor-to-runtime transfer.
- `bindings/python/native/include/hpactor/python/python_gateway_actor.hpp` / `src/python_gateway_actor.cpp` — budgeted command execution and completion handoff.
- `bindings/python/native/include/hpactor/python/python_gateway_wake_adapter.hpp` / `src/python_gateway_wake_adapter.cpp` — stable ActorSystem wake-port context.

### Tests

- `tests/unit/python/` — contracts, queues, notifier, runtime, bridge, and gateway tests in `test_unit_python_binding`.
- `tests/integration/python/` — paused-scheduler bridge/gateway workflow in `test_integration_python_binding`.
- `tests/architecture/CMakeLists.txt` — binding architecture fitness checks.

---

### Task 1: Add bounded drain support to the shared MPSC ring

**Files:**
- Modify: `include/hpactor/adt/mpsc_ring_buffer.hpp`
- Modify: `tests/unit/adt/test_adt_mpsc_ring_buffer.cpp`

**Interfaces:**
- Consumes: Existing `MpscRingBuffer<T, Capacity>::drain(Fn)` and `DynamicMpscRingBuffer<T>::drain(Fn)`.
- Produces: `drain_up_to(size_t max_items, Fn&& callback) -> size_t` for both ring variants; existing `drain()` remains source-compatible.

- [ ] **Step 1: Write failing bounded-drain tests**

Add these tests to `tests/unit/adt/test_adt_mpsc_ring_buffer.cpp`:

```cpp
TEST(MpscRingBufferTest, CompileTimeDrainUpToPreservesRemainder) {
    adt::MpscRingBuffer<TestPayload, 8> rb;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(rb.try_push(TestPayload{i}));
    }

    std::vector<int> first;
    EXPECT_EQ(rb.drain_up_to(2, [&](const TestPayload& item) {
                  first.push_back(item.value);
              }),
              2u);
    EXPECT_EQ(first, (std::vector<int>{0, 1}));
    EXPECT_EQ(rb.size(), 3u);

    std::vector<int> second;
    EXPECT_EQ(rb.drain_up_to(8, [&](const TestPayload& item) {
                  second.push_back(item.value);
              }),
              3u);
    EXPECT_EQ(second, (std::vector<int>{2, 3, 4}));
}

TEST(MpscRingBufferTest, DynamicDrainUpToZeroConsumesNothing) {
    adt::DynamicMpscRingBuffer<TestPayload> rb(8);
    ASSERT_TRUE(rb.try_push(TestPayload{7}));
    EXPECT_EQ(rb.drain_up_to(0, [](const TestPayload&) {}), 0u);
    EXPECT_EQ(rb.size(), 1u);
}
```

- [ ] **Step 2: Configure and run the focused test to verify RED**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build test_unit_adt
./build/tests/unit/adt/test_unit_adt \
  --gtest_filter='MpscRingBufferTest.*DrainUpTo*'
```

Expected: compilation fails because neither ring exposes `drain_up_to`.

- [ ] **Step 3: Implement bounded drain in both ring variants**

In each ring class, replace the current `drain()` body with the following pair, using `Capacity` in the compile-time class and `capacity_` in the dynamic class when releasing a slot:

```cpp
template <typename Fn>
size_t drain_up_to(size_t max_items, Fn&& callback) {
    uint64_t r = read_cursor_.load(std::memory_order_relaxed);
    size_t count = 0;
    while (count < max_items) {
        const uint64_t slot = r & mask_;
        if (seq_[slot].load(std::memory_order_acquire) != (r + 1)) {
            break;
        }
        callback(buffer_[slot]);
        seq_[slot].store(r + Capacity, std::memory_order_release);
        ++r;
        ++count;
    }
    read_cursor_.store(r, std::memory_order_release);
    return count;
}

template <typename Fn> size_t drain(Fn&& callback) {
    return drain_up_to(std::numeric_limits<size_t>::max(),
                       std::forward<Fn>(callback));
}
```

For `DynamicMpscRingBuffer`, the release store is:

```cpp
seq_[slot].store(r + capacity_, std::memory_order_release);
```

Add `<limits>` and `<utility>` includes.

- [ ] **Step 4: Run the focused and existing ring tests to verify GREEN**

Run:

```bash
ninja -C build test_unit_adt
./build/tests/unit/adt/test_unit_adt \
  --gtest_filter='MpscRingBufferTest.*'
```

Expected: all `MpscRingBufferTest` cases pass.

- [ ] **Step 5: Commit the bounded drain primitive**

```bash
git add include/hpactor/adt/mpsc_ring_buffer.hpp \
  tests/unit/adt/test_adt_mpsc_ring_buffer.cpp
git commit -m "feat: add bounded MPSC ring draining"
```

### Task 2: Preserve admitted delivery metadata in `TypedMessage`

**Files:**
- Modify: `include/hpactor/msg/typed_message.hpp`
- Modify: `src/mailbox/delivery_pipeline.cpp`
- Modify: `tests/integration/msg/test_msg_integration.cpp`
- Modify: `tests/integration/msg/CMakeLists.txt`

**Interfaces:**
- Consumes: `DeliveryPipeline::try_deliver()`, `MailboxEnvelopeMeta::priority`, and `MailboxEnvelopeMeta::flags`.
- Produces: `TypedMessage::delivery_priority()`, `set_delivery_priority()`, `delivery_flags()`, and `set_delivery_flags()`; move operations preserve both values.

- [ ] **Step 1: Write failing metadata preservation tests**

Add a move-preservation test to `tests/integration/msg/test_msg_integration.cpp`:

```cpp
TEST(MessageIntegrationTest, TypedMessageDeliveryMetadataSurvivesMove) {
    TypedMessage original(TypeTag::User, StreamBuffer{1, 2, 3});
    original.set_delivery_priority(3);
    original.set_delivery_flags(0xA5A50001u);

    TypedMessage moved(std::move(original));
    EXPECT_EQ(moved.delivery_priority(), 3u);
    EXPECT_EQ(moved.delivery_flags(), 0xA5A50001u);

    TypedMessage assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned.delivery_priority(), 3u);
    EXPECT_EQ(assigned.delivery_flags(), 0xA5A50001u);
}
```

Add a real-pipeline probe actor and test in the same file:

```cpp
namespace {
struct DeliveryMetadataObservation {
    std::atomic<uint8_t> priority{0};
    std::atomic<uint32_t> flags{0};
};

class DeliveryMetadataProbe final : public EventBasedActor {
  public:
    DeliveryMetadataProbe(ActorContext* ctx, ActorSystem& sys,
                          DeliveryMetadataObservation& observation)
        : EventBasedActor(ctx, sys), observation_(observation) {}

  protected:
    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            observation_.priority.store(msg.delivery_priority(),
                                        std::memory_order_release);
            observation_.flags.store(msg.delivery_flags(),
                                     std::memory_order_release);
        });
    }

  private:
    DeliveryMetadataObservation& observation_;
};
} // namespace

TEST(MessageIntegrationTest, DeliveryPipelineStampsPriorityAndFlags) {
    Config cfg;
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);
    DeliveryMetadataObservation observation;
    auto actor = system.spawn<DeliveryMetadataProbe>(observation);

    mailbox::DeliveryOptions options;
    options.flags = 0x00000040u;
    auto result = system.try_deliver_local(
        actor.id(), TypedMessage(TypeTag::User, StreamBuffer{9}), 2,
        INT64_MAX, options);
    ASSERT_TRUE(result.accepted());
    ASSERT_TRUE(driver.drain_until([&] {
        return observation.flags.load(std::memory_order_acquire) != 0;
    }));
    EXPECT_EQ(observation.priority.load(std::memory_order_acquire), 2u);
    EXPECT_EQ(observation.flags.load(std::memory_order_acquire), 0x40u);
}
```

Add `hpactor_test_support` to `test_integration_msg` links.

- [ ] **Step 2: Run the focused test to verify RED**

Run:

```bash
ninja -C build test_integration_msg
./build/tests/integration/msg/test_integration_msg \
  --gtest_filter='MessageIntegrationTest.*DeliveryMetadata*'
```

Expected: compilation fails because the four metadata accessors do not exist.

- [ ] **Step 3: Add metadata fields and stamp them in the pipeline**

Add these members and accessors to `TypedMessage`:

```cpp
[[nodiscard]] uint8_t delivery_priority() const noexcept {
    return delivery_priority_;
}
void set_delivery_priority(uint8_t value) noexcept {
    delivery_priority_ = value;
}
[[nodiscard]] uint32_t delivery_flags() const noexcept {
    return delivery_flags_;
}
void set_delivery_flags(uint32_t value) noexcept {
    delivery_flags_ = value;
}
```

```cpp
uint8_t delivery_priority_{0};
uint32_t delivery_flags_{0};
```

Copy both fields in the move constructor and move assignment. In
`DeliveryPipeline::try_deliver()`, after `meta.flags` includes `NoDrop` and
before `mailbox->try_push`, stamp the message:

```cpp
msg.set_delivery_priority(meta.priority);
msg.set_delivery_flags(meta.flags);
```

- [ ] **Step 4: Run metadata and delivery tests to verify GREEN**

Run:

```bash
ninja -C build test_integration_msg test_integration_actor
./build/tests/integration/msg/test_integration_msg \
  --gtest_filter='MessageIntegrationTest.*DeliveryMetadata*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*DeliverySemantics*:*ActorContextTrySend*'
```

Expected: selected tests pass.

- [ ] **Step 5: Commit metadata preservation**

```bash
git add include/hpactor/msg/typed_message.hpp \
  src/mailbox/delivery_pipeline.cpp \
  tests/integration/msg/test_msg_integration.cpp \
  tests/integration/msg/CMakeLists.txt
git commit -m "feat: preserve admitted delivery metadata"
```

### Task 3: Add the native binding target, contracts, and bounded queues

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`
- Modify: `include/hpactor/msg/failure_reason.hpp`
- Modify: `src/types/failure_reason.cpp`
- Modify: `tests/unit/core/test_failure_reason.cpp`
- Create: `bindings/python/native/CMakeLists.txt`
- Create: `bindings/python/native/include/hpactor/python/python_type_tags.hpp`
- Create: `bindings/python/native/include/hpactor/python/python_runtime_config.hpp`
- Create: `bindings/python/native/include/hpactor/python/python_bridge_types.hpp`
- Create: `bindings/python/native/include/hpactor/python/python_runtime_snapshot.hpp`
- Create: `bindings/python/native/include/hpactor/python/python_runtime_queues.hpp`
- Create: `bindings/python/native/src/python_runtime_config.cpp`
- Create: `bindings/python/native/src/python_runtime_queues.cpp`
- Create: `tests/unit/python/CMakeLists.txt`
- Create: `tests/unit/python/test_python_contracts.cpp`
- Create: `tests/unit/python/test_python_runtime_queues.cpp`

**Interfaces:**
- Consumes: `adt::DynamicMpscRingBuffer`, `ActorAddress`, `TraceContext`, `StreamBuffer`, `MessageId`, and `TypeTag`.
- Produces: validated `PythonRuntimeConfig`; value-only bridge contracts; `PythonRuntimeQueues`; append-only `FailureSource::LanguageBinding = 12`.

- [ ] **Step 1: Add the disabled-by-default build and test scaffolding**

Add to top-level `CMakeLists.txt` with the other options:

```cmake
option(ENABLE_PYTHON_BINDINGS "Build the native Python binding foundation" OFF)
```

After `add_subdirectory(src)`, add:

```cmake
if(ENABLE_PYTHON_BINDINGS)
    add_subdirectory(bindings/python/native)
endif()
```

Add to `tests/unit/CMakeLists.txt`:

```cmake
if(ENABLE_PYTHON_BINDINGS)
    add_subdirectory(python)
endif()
```

Create `bindings/python/native/CMakeLists.txt`:

```cmake
add_library(hpactor_python_native STATIC
    src/python_runtime_config.cpp
    src/python_runtime_queues.cpp)
set_target_properties(hpactor_python_native PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(hpactor_python_native PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hpactor_python_native PUBLIC hpactor_lib)
target_compile_options(hpactor_python_native PRIVATE -fno-exceptions -fno-rtti)
```

Create `tests/unit/python/CMakeLists.txt`:

```cmake
add_executable(test_unit_python_binding
    test_python_contracts.cpp
    test_python_runtime_queues.cpp)
target_link_libraries(test_unit_python_binding PRIVATE
    hpactor_python_native hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_python_binding)
```

- [ ] **Step 2: Write failing contract and queue tests**

Create `test_python_contracts.cpp` with assertions for exact tag values,
distinct tags, config defaults, and each validation boundary:

```cpp
TEST(PythonContractsTest, TagsUseReservedLocalRange) {
    EXPECT_EQ(static_cast<uint32_t>(python::kPythonWakeupTag), 0xF0u);
    EXPECT_EQ(static_cast<uint32_t>(python::kPythonActorCommandTag), 0xF1u);
    EXPECT_EQ(static_cast<uint32_t>(python::kPythonActorFailedTag), 0xF2u);
    EXPECT_EQ(static_cast<uint32_t>(python::kPythonInspectTag), 0xF3u);
}

TEST(PythonContractsTest, ConfigRejectsInvalidBounds) {
    python::PythonRuntimeConfig cfg;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::None);

    cfg.dispatch_queue_capacity = 63;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::QueueCapacity);
    cfg = {};
    cfg.command_queue_capacity = 65;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::QueueCapacity);
    cfg = {};
    cfg.completion_queue_capacity = 1'048'577;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::QueueCapacity);
    cfg = {};
    cfg.max_dispatch_per_tick = 0;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::DrainBudget);
    cfg = {};
    cfg.max_commands_per_turn = cfg.command_queue_capacity + 1;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::DrainBudget);
    cfg = {};
    cfg.max_actor_bindings = 0;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::ActorBindingCapacity);
    cfg = {};
    cfg.max_actor_bindings = 1'048'577;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::ActorBindingCapacity);
    cfg = {};
    cfg.loop_lag_unready_ms = 99;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::LoopLag);
    cfg = {};
    cfg.loop_lag_unready_ms = 60'001;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::LoopLag);
    cfg = {};
    cfg.handler_shutdown_timeout_ms = 99;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::ShutdownTimeout);
    cfg = {};
    cfg.handler_shutdown_timeout_ms = 300'001;
    EXPECT_EQ(cfg.validate(), python::PythonConfigError::ShutdownTimeout);
}

TEST(PythonContractsTest, LanguageBindingFailureSourceIsAppendOnly) {
    EXPECT_EQ(static_cast<uint8_t>(FailureSource::Unknown), 11u);
    EXPECT_EQ(static_cast<uint8_t>(FailureSource::LanguageBinding), 12u);
    EXPECT_STREQ(to_string(FailureSource::LanguageBinding), "language_binding");
}
```

Create `test_python_runtime_queues.cpp`:

```cpp
TEST(PythonRuntimeQueuesTest, DispatchDrainHonorsBudgetAndOrder) {
    python::PythonRuntimeConfig cfg;
    cfg.dispatch_queue_capacity = 64;
    python::PythonRuntimeQueues queues(cfg);
    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        auto envelope = std::make_shared<python::PythonDispatchEnvelope>();
        envelope->sequence = sequence;
        ASSERT_TRUE(queues.try_push_dispatch(std::move(envelope)));
    }

    std::vector<uint64_t> observed;
    EXPECT_EQ(queues.drain_dispatch(2, [&](const auto& envelope) {
                  observed.push_back(envelope->sequence);
              }),
              2u);
    EXPECT_EQ(observed, (std::vector<uint64_t>{1, 2}));
    EXPECT_EQ(queues.snapshot().dispatch_depth, 1u);
}

TEST(PythonRuntimeQueuesTest, FullQueueRetainsProducerOwnership) {
    python::PythonRuntimeConfig cfg;
    cfg.command_queue_capacity = 64;
    python::PythonRuntimeQueues queues(cfg);
    for (uint64_t i = 0; i < 64; ++i) {
        ASSERT_TRUE(queues.try_push_command(
            std::make_shared<python::PythonCommand>()));
    }
    auto rejected = std::make_shared<python::PythonCommand>();
    EXPECT_FALSE(queues.try_push_command(rejected));
    EXPECT_EQ(rejected.use_count(), 1);
    EXPECT_EQ(queues.snapshot().command_rejected, 1u);
}
```

- [ ] **Step 3: Reconfigure and run tests to verify RED**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
  -DENABLE_PYTHON_BINDINGS=ON
ninja -C build test_unit_python_binding test_unit_core
```

Expected: compilation fails because the Python contract headers and
`FailureSource::LanguageBinding` do not exist.

- [ ] **Step 4: Implement exact value contracts and validation**

Define `PythonRuntimeConfig` with these fields and error enum:

```cpp
enum class PythonConfigError : uint8_t {
    None,
    QueueCapacity,
    DrainBudget,
    ActorBindingCapacity,
    LoopLag,
    ShutdownTimeout,
};

struct PythonRuntimeConfig final {
    size_t dispatch_queue_capacity{65'536};
    size_t command_queue_capacity{16'384};
    size_t completion_queue_capacity{16'384};
    size_t max_actor_bindings{65'536};
    size_t max_dispatch_per_tick{256};
    size_t max_commands_per_turn{256};
    uint32_t loop_lag_unready_ms{5'000};
    uint32_t handler_shutdown_timeout_ms{10'000};
    bool trace_handler_spans{true};

    [[nodiscard]] PythonConfigError validate() const noexcept;
};
```

Validation uses exact inclusive bounds from Global Constraints and the helper:

```cpp
constexpr bool valid_queue_capacity(size_t value) noexcept {
    return value >= 64 && value <= 1'048'576 &&
           (value & (value - 1)) == 0;
}
```

Define the four tags with `make_subsystem_tag(0xF0)` through `0xF3`. Use these
exact value contracts:

```cpp
enum class PythonCommandKind : uint8_t {
    Send,
    Reply,
    ReplyError,
    Ask,
    Spawn,
    Schedule,
    CancelSchedule,
    Link,
    Unlink,
    Monitor,
    Demonitor,
    Stop,
    Passivate,
    ActorFailed,
    Inspect,
};

enum class PythonCompletionKind : uint8_t {
    CommandResult,
    AskResult,
    DeliveryResult,
    SpawnResult,
    InspectResult,
};

struct PythonDispatchEnvelope final {
    ActorAddress actor;
    uint64_t generation{0};
    TypeTag type_tag{TypeTag::Invalid};
    StreamBuffer payload;
    ActorAddress sender;
    MessageId message_id{};
    uint64_t ask_message_id{0};
    TraceContext trace{};
    bool has_trace{false};
    uint8_t priority{0};
    int64_t deadline_ns{INT64_MAX};
    uint32_t flags{0};
    bool ack_requested{false};
    uint64_t sequence{0};
};

struct PythonCommand final {
    PythonCommandKind kind{PythonCommandKind::Send};
    ActorAddress origin;
    uint64_t generation{0};
    ActorAddress target;
    TypeTag type_tag{TypeTag::Invalid};
    StreamBuffer payload;
    uint64_t token{0};
    uint64_t sequence{0};
    uint8_t priority{0};
    int64_t deadline_ns{INT64_MAX};
    uint32_t flags{0};
    uint64_t delay_ns{0};
    uint32_t error_code{0};
};

struct PythonCompletion final {
    PythonCompletionKind kind{PythonCompletionKind::CommandResult};
    uint64_t token{0};
    uint64_t sequence{0};
    FailureReason failure{FailureReason::Unknown};
    ActorAddress actor;
    uint64_t generation{0};
    TypeTag type_tag{TypeTag::Invalid};
    StreamBuffer payload;
};

using PythonDispatchPtr = std::shared_ptr<const PythonDispatchEnvelope>;
using PythonCommandPtr = std::shared_ptr<const PythonCommand>;
using PythonCompletionPtr = std::shared_ptr<const PythonCompletion>;
```

Define the queue snapshot used by later runtime snapshots:

```cpp
struct PythonQueueSnapshot final {
    size_t dispatch_depth{0};
    size_t command_depth{0};
    size_t completion_depth{0};
    uint64_t dispatch_rejected{0};
    uint64_t command_rejected{0};
    uint64_t completion_rejected{0};
};
```

Define `PythonRuntimeQueues` with three `DynamicMpscRingBuffer` members,
`try_push_*(const SharedPtr&)`, `drain_*(size_t, Fn)`, and an atomic rejection
counter for each queue. The const-reference admission surface matches the ring
primitive and leaves the producer's shared pointer intact on rejection.
`snapshot()` returns queue depth and rejection totals.

Make every existing `FailureSource` numeric value explicit (`ActorRuntime = 0`
through `Unknown = 11`) and append `LanguageBinding = 12`. Add its string case.

- [ ] **Step 5: Run contract, queue, and failure-source tests to verify GREEN**

Run:

```bash
ninja -C build test_unit_python_binding test_unit_core
./build/tests/unit/python/test_unit_python_binding
./build/tests/unit/core/test_unit_core \
  --gtest_filter='FailureReasonTest.ToStringFailureSource'
```

Expected: all selected tests pass.

- [ ] **Step 6: Commit the native contracts and queue layer**

```bash
git add CMakeLists.txt tests/unit/CMakeLists.txt \
  include/hpactor/msg/failure_reason.hpp src/types/failure_reason.cpp \
  tests/unit/core/test_failure_reason.cpp \
  bindings/python/native tests/unit/python
git commit -m "feat: add Python bridge native contracts"
```

### Task 4: Add the Linux/macOS non-blocking notifier

**Files:**
- Create: `bindings/python/native/include/hpactor/python/native_notifier.hpp`
- Create: `bindings/python/native/src/native_notifier.cpp`
- Modify: `bindings/python/native/CMakeLists.txt`
- Create: `tests/unit/python/test_native_notifier.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`

**Interfaces:**
- Consumes: POSIX file descriptors.
- Produces: move-only `NativeNotifier::create()`, `read_fd()`, `signal()`, `drain()`, `valid()`, and idempotent `close()`.

- [ ] **Step 1: Write failing notifier tests**

```cpp
TEST(NativeNotifierTest, SignalAndDrainAreNonBlocking) {
    auto created = python::NativeNotifier::create();
    ASSERT_TRUE(created.ok());
    auto notifier = std::move(created.value());
    ASSERT_TRUE(notifier->valid());
    ASSERT_GE(notifier->read_fd(), 0);

    EXPECT_TRUE(notifier->signal());
    EXPECT_TRUE(notifier->signal());
    EXPECT_GE(notifier->drain(), 1u);
    EXPECT_EQ(notifier->drain(), 0u);
}

TEST(NativeNotifierTest, MoveAndClosePreserveSingleOwnership) {
    auto created = python::NativeNotifier::create();
    ASSERT_TRUE(created.ok());
    auto first = std::move(created.value());
    const int fd = first->read_fd();
    python::NativeNotifier moved(std::move(*first));
    EXPECT_FALSE(first->valid());
    EXPECT_EQ(moved.read_fd(), fd);
    moved.close();
    moved.close();
    EXPECT_FALSE(moved.valid());
    EXPECT_FALSE(moved.signal());
}
```

- [ ] **Step 2: Build the notifier test to verify RED**

Run:

```bash
ninja -C build test_unit_python_binding
```

Expected: compilation fails because `NativeNotifier` is undefined.

- [ ] **Step 3: Implement the platform notifier**

Declare:

```cpp
class NativeNotifier final {
  public:
    static result<std::unique_ptr<NativeNotifier>> create() noexcept;
    NativeNotifier(NativeNotifier&& other) noexcept;
    NativeNotifier& operator=(NativeNotifier&& other) noexcept;
    ~NativeNotifier();

    NativeNotifier(const NativeNotifier&) = delete;
    NativeNotifier& operator=(const NativeNotifier&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int read_fd() const noexcept;
    [[nodiscard]] bool signal() noexcept;
    [[nodiscard]] uint64_t drain() noexcept;
    void close() noexcept;

  private:
    NativeNotifier(int read_fd, int write_fd) noexcept;
    int read_fd_{-1};
    int write_fd_{-1};
};
```

On Linux, create one `eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)` and store the same
descriptor in both fields. On macOS, use `socketpair(AF_UNIX, SOCK_STREAM, 0,
fds)`, then set `O_NONBLOCK` and `FD_CLOEXEC` on both descriptors with `fcntl`.

`signal()` writes `uint64_t{1}` on Linux or one byte on macOS. Treat `EAGAIN`
as success because a wakeup is already pending. Retry `EINTR`; return false for
other errors. `drain()` loops until `EAGAIN`, sums eventfd counts on Linux and
bytes on macOS, and never blocks. `close()` handles the shared Linux descriptor
exactly once.

Append `src/native_notifier.cpp` to `hpactor_python_native` with
`target_sources()`, and append `test_native_notifier.cpp` to
`test_unit_python_binding`.

- [ ] **Step 4: Run notifier tests to verify GREEN**

Run:

```bash
ninja -C build test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='NativeNotifierTest.*'
```

Expected: both tests pass without timing waits.

- [ ] **Step 5: Commit notifier support**

```bash
git add bindings/python/native tests/unit/python
git commit -m "feat: add Python runtime notifier"
```

### Task 5: Compose runtime lifecycle, actor leases, and snapshots

**Files:**
- Create: `bindings/python/native/include/hpactor/python/python_ports.hpp`
- Create: `bindings/python/native/include/hpactor/python/python_runtime.hpp`
- Create: `bindings/python/native/src/python_runtime.cpp`
- Modify: `bindings/python/native/CMakeLists.txt`
- Create: `tests/unit/python/test_python_runtime.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`

**Interfaces:**
- Consumes: `PythonRuntimeConfig`, `PythonRuntimeQueues`, two `NativeNotifier` objects, and `GatewayWakePort`.
- Produces: `PythonRuntime::create`, lifecycle transitions, bounded `PythonActorLease`, queue submission/drain APIs, generation validation, and `PythonRuntimeSnapshot`.

- [ ] **Step 1: Write failing lifecycle and lease tests**

```cpp
namespace {
struct WakeProbe {
    size_t calls{0};
    static bool wake(void* context) noexcept {
        ++static_cast<WakeProbe*>(context)->calls;
        return true;
    }
};
} // namespace

TEST(PythonRuntimeTest, LifecycleIsExplicitAndStopIsIdempotent) {
    auto created = python::PythonRuntime::create({});
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());
    EXPECT_EQ(runtime->snapshot().state, python::PythonRuntimeState::Created);

    WakeProbe probe;
    ASSERT_TRUE(runtime->start({&probe, &WakeProbe::wake}).ok());
    EXPECT_EQ(runtime->snapshot().state, python::PythonRuntimeState::Running);
    EXPECT_GE(runtime->dispatch_read_fd(), 0);
    EXPECT_GE(runtime->completion_read_fd(), 0);
    runtime->begin_draining();
    EXPECT_EQ(runtime->snapshot().state, python::PythonRuntimeState::Draining);
    EXPECT_TRUE(runtime->stop().ok());
    EXPECT_TRUE(runtime->stop().ok());
    EXPECT_EQ(runtime->snapshot().state, python::PythonRuntimeState::Stopped);
    EXPECT_EQ(runtime->dispatch_read_fd(), -1);
    EXPECT_EQ(runtime->completion_read_fd(), -1);
}

TEST(PythonRuntimeTest, ActorLeasesAreBoundedAndGenerational) {
    python::PythonRuntimeConfig cfg;
    cfg.max_actor_bindings = 1;
    auto created = python::PythonRuntime::create(cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());

    auto first = runtime->reserve_actor();
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(runtime->reserve_actor().has_value());
    ASSERT_TRUE(first->bind(ActorId{42}));
    const uint64_t first_generation = first->generation();
    EXPECT_TRUE(runtime->generation_matches(ActorId{42}, first_generation));
    first.reset();

    auto replacement = runtime->reserve_actor();
    ASSERT_TRUE(replacement.has_value());
    ASSERT_TRUE(replacement->bind(ActorId{42}));
    EXPECT_GT(replacement->generation(), first_generation);
    EXPECT_FALSE(runtime->generation_matches(ActorId{42}, first_generation));
}
```

- [ ] **Step 2: Build the runtime test to verify RED**

Run:

```bash
ninja -C build test_unit_python_binding
```

Expected: compilation fails because runtime and port types do not exist.

- [ ] **Step 3: Implement fixed ports and the runtime state machine**

Define the wake port without `std::function`:

```cpp
struct GatewayWakePort final {
    void* context{nullptr};
    bool (*wake)(void*) noexcept{nullptr};
    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && wake != nullptr;
    }
};
```

Define lifecycle states exactly:

```cpp
enum class PythonRuntimeState : uint8_t {
    Created,
    Starting,
    Running,
    Draining,
    Stopping,
    Stopped,
    Failed,
};
```

Expose these exact move-only lease and runtime interfaces (the `drain_*`
templates forward to `PythonRuntimeQueues::drain_*`):

```cpp
class PythonRuntime;

class PythonActorLease final {
  public:
    PythonActorLease(PythonActorLease&& other) noexcept;
    PythonActorLease& operator=(PythonActorLease&& other) noexcept;
    ~PythonActorLease();

    PythonActorLease(const PythonActorLease&) = delete;
    PythonActorLease& operator=(const PythonActorLease&) = delete;

    [[nodiscard]] bool bind(ActorId actor_id) noexcept;
    void reset() noexcept;
    [[nodiscard]] uint64_t generation() const noexcept;
    [[nodiscard]] ActorId actor_id() const noexcept;

  private:
    friend class PythonRuntime;
    PythonActorLease(PythonRuntime* runtime, uint64_t generation) noexcept;
    PythonRuntime* runtime_{nullptr};
    ActorId actor_id_{};
    uint64_t generation_{0};
    bool bound_{false};
};

class PythonRuntime final {
  public:
    static result<std::unique_ptr<PythonRuntime>>
    create(PythonRuntimeConfig config) noexcept;
    ~PythonRuntime();

    PythonRuntime(const PythonRuntime&) = delete;
    PythonRuntime& operator=(const PythonRuntime&) = delete;

    [[nodiscard]] result<void> start(GatewayWakePort wake_port) noexcept;
    void begin_draining() noexcept;
    [[nodiscard]] result<void> stop() noexcept;
    [[nodiscard]] std::optional<PythonActorLease> reserve_actor() noexcept;
    [[nodiscard]] bool generation_matches(ActorId actor_id,
                                          uint64_t generation) const noexcept;

    [[nodiscard]] bool
    try_push_dispatch(const PythonDispatchPtr& envelope) noexcept;
    [[nodiscard]] bool try_push_command(const PythonCommandPtr& command) noexcept;
    [[nodiscard]] bool
    try_push_completion(const PythonCompletionPtr& completion) noexcept;

    [[nodiscard]] int dispatch_read_fd() const noexcept;
    [[nodiscard]] int completion_read_fd() const noexcept;
    [[nodiscard]] uint64_t drain_dispatch_notification() noexcept;
    [[nodiscard]] uint64_t drain_completion_notification() noexcept;

    template <typename Fn>
    size_t drain_dispatch(size_t max_items, Fn&& callback);
    template <typename Fn>
    size_t drain_commands(size_t max_items, Fn&& callback);
    template <typename Fn>
    size_t drain_completions(size_t max_items, Fn&& callback);

    [[nodiscard]] const PythonRuntimeConfig& config() const noexcept;
    [[nodiscard]] PythonRuntimeSnapshot snapshot() const noexcept;

  private:
    friend class PythonActorLease;
    explicit PythonRuntime(PythonRuntimeConfig config) noexcept;
    [[nodiscard]] bool bind_actor(ActorId actor_id,
                                  uint64_t generation) noexcept;
    void release_actor(ActorId actor_id, uint64_t generation,
                       bool was_bound) noexcept;
};
```

`PythonRuntimeSnapshot` contains `PythonRuntimeState state`,
`PythonQueueSnapshot queues`, `size_t actor_bindings`,
`uint64_t stale_completion_rejected`, and the descriptor fields
`int dispatch_notifier_fd` and `int completion_notifier_fd` (both `-1` while
closed).

`PythonRuntime::create()` validates config before constructing runtime queues or
notifiers. `start()` accepts exactly one valid wake port and only transitions
`Created -> Starting -> Running`; notifier creation failure transitions to
`Failed`. `begin_draining()` is `Running -> Draining`. `stop()` is idempotent,
closes admission, closes both notifiers, clears the wake port, and transitions
to `Stopped`.

Implement a move-only `PythonActorLease` reserved under a mutex. Reservation
increments a global monotonic generation and a bounded reservation count.
`bind(ActorId)` inserts the ID/generation pair once. Destruction erases only a
matching pair and releases one reservation, so an old lease cannot erase a
replacement generation.

Queue submission checks `Running`; dispatch/completion enqueue signals the
corresponding notifier; command enqueue invokes the fixed wake port. Snapshot
copies atomics and bounded-map size under the actor-binding mutex.

Before accepting a completion with a non-zero actor ID and generation,
`try_push_completion()` calls `generation_matches()`. A mismatch returns false,
increments `stale_completion_rejected`, and does not signal the completion
notifier. Add this focused test after the lease test:

```cpp
TEST(PythonRuntimeTest, RejectsCompletionForReplacedGeneration) {
    python::PythonRuntimeConfig cfg;
    cfg.max_actor_bindings = 1;
    auto created = python::PythonRuntime::create(cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());
    WakeProbe probe;
    ASSERT_TRUE(runtime->start({&probe, &WakeProbe::wake}).ok());

    auto old_lease = runtime->reserve_actor();
    ASSERT_TRUE(old_lease.has_value());
    ASSERT_TRUE(old_lease->bind(ActorId{42}));
    const uint64_t old_generation = old_lease->generation();
    old_lease->reset();

    auto replacement = runtime->reserve_actor();
    ASSERT_TRUE(replacement.has_value());
    ASSERT_TRUE(replacement->bind(ActorId{42}));
    auto stale = std::make_shared<python::PythonCompletion>();
    stale->actor.id = ActorId{42};
    stale->generation = old_generation;
    EXPECT_FALSE(runtime->try_push_completion(stale));
    EXPECT_EQ(runtime->snapshot().stale_completion_rejected, 1u);
    EXPECT_EQ(runtime->snapshot().queues.completion_depth, 0u);
    EXPECT_TRUE(runtime->stop().ok());
}
```

Append `src/python_runtime.cpp` to `hpactor_python_native` and
`test_python_runtime.cpp` to `test_unit_python_binding` with
`target_sources()`.

- [ ] **Step 4: Run runtime and queue tests to verify GREEN**

Run:

```bash
ninja -C build test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonRuntimeTest.*:PythonRuntimeQueuesTest.*'
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit runtime composition**

```bash
git add bindings/python/native tests/unit/python
git commit -m "feat: add Python runtime lifecycle"
```

### Task 6: Implement the inbound `PythonBridgeActor`

**Files:**
- Create: `bindings/python/native/include/hpactor/python/python_bridge_actor.hpp`
- Create: `bindings/python/native/src/python_bridge_actor.cpp`
- Modify: `bindings/python/native/CMakeLists.txt`
- Create: `tests/unit/python/test_python_bridge_actor.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`

**Interfaces:**
- Consumes: `PythonRuntime`, a reserved `PythonActorLease`, `EventBasedActor` pipeline gates, and admitted `TypedMessage` metadata.
- Produces: one C++ bridge identity per future Python actor and bounded inbound transfer with explicit accepted/rejected accounting.

- [ ] **Step 1: Write a failing paused-scheduler bridge test**

```cpp
namespace {
struct BridgeWakeProbe {
    size_t calls{0};
    static bool wake(void* context) noexcept {
        ++static_cast<BridgeWakeProbe*>(context)->calls;
        return true;
    }
};
} // namespace

TEST(PythonBridgeActorTest, TransfersCompleteEnvelopeOnActorTurn) {
    Config cfg;
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto created = python::PythonRuntime::create({});
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());
    BridgeWakeProbe probe;
    ASSERT_TRUE(runtime->start({&probe, &BridgeWakeProbe::wake}).ok());
    auto lease = runtime->reserve_actor();
    ASSERT_TRUE(lease.has_value());
    auto bridge = system.spawn<python::PythonBridgeActor>(
        *runtime, std::move(*lease));

    TypedMessage message(TypeTag::User, StreamBuffer{1, 2, 3});
    message.set_sender_address(
        ActorAddress{LocalEndpoint, ActorType{7}, ActorId{99}, 4});
    message.set_message_id(501);
    message.set_ask_message_id(601);
    mailbox::DeliveryOptions options;
    options.message_id = 501;
    options.flags = 0x40;
    ASSERT_TRUE(system.try_deliver_local(
        bridge.id(), std::move(message), 2, 700, options).accepted());
    ASSERT_TRUE(driver.drain_until([&] {
        return runtime->snapshot().queues.dispatch_depth == 1;
    }));

    std::shared_ptr<const python::PythonDispatchEnvelope> observed;
    ASSERT_EQ(runtime->drain_dispatch(1, [&](const auto& item) {
                  observed = item;
              }),
              1u);
    ASSERT_NE(observed, nullptr);
    EXPECT_EQ(observed->actor.id, bridge.id());
    EXPECT_EQ(observed->sender.id, ActorId{99});
    EXPECT_EQ(observed->message_id, MessageId{501});
    EXPECT_EQ(observed->ask_message_id, 601u);
    EXPECT_EQ(observed->priority, 2u);
    EXPECT_EQ(observed->flags, 0x40u);
    EXPECT_EQ(observed->deadline_ns, 700);
    EXPECT_EQ(observed->payload, (StreamBuffer{1, 2, 3}));
    EXPECT_TRUE(runtime->stop().ok());
}
```

- [ ] **Step 2: Build the bridge test to verify RED**

Run:

```bash
ninja -C build test_unit_python_binding
```

Expected: compilation fails because `PythonBridgeActor` does not exist.

- [ ] **Step 3: Implement bridge actor ownership and receive behavior**

Declare:

```cpp
class PythonBridgeActor final : public EventBasedActor {
  public:
    static constexpr std::string_view kActorTypeName{"hpactor.python.bridge"};

    PythonBridgeActor(ActorContext* context, ActorSystem& system,
                      PythonRuntime& runtime, PythonActorLease lease) noexcept;
    void receive(TypedMessage& message) override;
    void on_activate() override;
    void on_deactivate() override;
    [[nodiscard]] uint64_t generation() const noexcept;

  private:
    PythonRuntime& runtime_;
    PythonActorLease lease_;
    uint64_t next_dispatch_sequence_{1};
};
```

`on_activate()` calls `EventBasedActor::on_activate()` then binds the reserved
lease to `id()`. `on_deactivate()` resets the lease before calling the base.

For system tags below `TypeTag::User`, `receive()` delegates to
`EventBasedActor::receive()`. For user messages it executes, in order:

```cpp
if (!apply_drain_gate(message) || !apply_lifecycle_gate(message)) {
    return;
}
context()->set_current_sender(message.sender_address());
context()->set_current_ask_message_id(message.ask_message_id());
auto envelope = std::make_shared<PythonDispatchEnvelope>();
envelope->actor = address();
envelope->generation = lease_.generation();
envelope->type_tag = message.type_id();
envelope->payload = message.payload();
envelope->sender = message.sender_address();
envelope->message_id = MessageId{message.message_id()};
envelope->ask_message_id = message.ask_message_id();
envelope->priority = message.delivery_priority();
envelope->deadline_ns = message.deadline_ns();
envelope->flags = message.delivery_flags();
envelope->ack_requested = message.ack_requested();
envelope->sequence = next_dispatch_sequence_++;
if (message.has_trace_context()) {
    envelope->trace = message.trace_context();
    envelope->has_trace = true;
}
PythonDispatchPtr dispatch = std::move(envelope);
const bool accepted = runtime_.try_push_dispatch(dispatch);
```

On accepted reliable messages, send ACK status `0`; on rejected reliable
messages, send NACK status `1` with a 500 ms retry hint. Then call
`try_drain_completion()`, `check_mailbox_pressure()`, and clear the ask ID.
Phase 1C adds DLQ, metrics, logs, and handler tracing; Phase 1A must still avoid
false ACK on transfer rejection.

Append `src/python_bridge_actor.cpp` to `hpactor_python_native` and
`test_python_bridge_actor.cpp` to `test_unit_python_binding` with
`target_sources()`.

- [ ] **Step 4: Run bridge and actor delivery tests to verify GREEN**

Run:

```bash
ninja -C build test_unit_python_binding test_integration_actor
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonBridgeActorTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*DeliverySemantics*:*Drain*'
```

Expected: selected tests pass.

- [ ] **Step 5: Commit the bridge actor**

```bash
git add bindings/python/native tests/unit/python
git commit -m "feat: add Python bridge actor"
```

### Task 7: Implement budgeted gateway execution and wake adapter

**Files:**
- Create: `bindings/python/native/include/hpactor/python/python_gateway_actor.hpp`
- Create: `bindings/python/native/src/python_gateway_actor.cpp`
- Create: `bindings/python/native/include/hpactor/python/python_gateway_wake_adapter.hpp`
- Create: `bindings/python/native/src/python_gateway_wake_adapter.cpp`
- Modify: `bindings/python/native/include/hpactor/python/python_ports.hpp`
- Modify: `bindings/python/native/CMakeLists.txt`
- Create: `tests/unit/python/test_python_gateway_actor.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`

**Interfaces:**
- Consumes: command queue, `max_commands_per_turn`, `PythonCommandExecutorPort`, and local-only wake tag.
- Produces: budgeted gateway actor, completion records, FIFO command execution, and stable `GatewayWakePort` adapter.

- [ ] **Step 1: Write a failing budgeted execution test**

```cpp
namespace {
struct CommandProbe {
    std::vector<uint64_t> sequences;
    static python::PythonCommandExecution
    execute(void* context, const python::PythonCommand& command) noexcept {
        auto* self = static_cast<CommandProbe*>(context);
        self->sequences.push_back(command.sequence);
        python::PythonCompletion completion;
        completion.token = command.token;
        completion.sequence = command.sequence;
        return {true, std::move(completion)};
    }
};
} // namespace

TEST(PythonGatewayActorTest, DrainBudgetRequeuesRemainingCommands) {
    Config cfg;
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    python::PythonRuntimeConfig runtime_cfg;
    runtime_cfg.max_commands_per_turn = 2;
    auto created = python::PythonRuntime::create(runtime_cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());
    CommandProbe commands;
    auto gateway = system.spawn<python::PythonGatewayActor>(
        *runtime,
        python::PythonCommandExecutorPort{&commands, &CommandProbe::execute});
    python::PythonGatewayWakeAdapter wake(system, gateway.address());
    ASSERT_TRUE(runtime->start(wake.port()).ok());

    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        auto command = std::make_shared<python::PythonCommand>();
        command->sequence = sequence;
        command->token = sequence + 100;
        ASSERT_TRUE(runtime->try_push_command(command));
    }

    ASSERT_TRUE(driver.run_one());
    EXPECT_EQ(commands.sequences, (std::vector<uint64_t>{1, 2}));
    EXPECT_EQ(runtime->snapshot().queues.command_depth, 1u);
    ASSERT_TRUE(driver.run_one());
    EXPECT_EQ(commands.sequences, (std::vector<uint64_t>{1, 2, 3}));
    EXPECT_EQ(runtime->snapshot().queues.completion_depth, 3u);
    EXPECT_TRUE(runtime->stop().ok());
}
```

- [ ] **Step 2: Build the gateway test to verify RED**

Run:

```bash
ninja -C build test_unit_python_binding
```

Expected: compilation fails because gateway, execution port, and adapter types
do not exist.

- [ ] **Step 3: Implement fixed execution port and gateway actor**

Add to `python_ports.hpp`:

```cpp
struct PythonCommandExecution final {
    bool emit_completion{false};
    PythonCompletion completion;
};

struct PythonCommandExecutorPort final {
    void* context{nullptr};
    PythonCommandExecution (*execute)(void*, const PythonCommand&) noexcept{nullptr};
    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && execute != nullptr;
    }
};
```

Declare the actor and adapter seams exactly:

```cpp
class PythonGatewayActor final : public EventBasedActor {
  public:
    static constexpr std::string_view kActorTypeName{"hpactor.python.gateway"};

    PythonGatewayActor(ActorContext* context, ActorSystem& system,
                       PythonRuntime& runtime,
                       PythonCommandExecutorPort executor) noexcept;
    void receive(TypedMessage& message) override;

  private:
    void handle_wakeup(TypedMessage& message) noexcept;
    PythonRuntime& runtime_;
    PythonCommandExecutorPort executor_;
};

class PythonGatewayWakeAdapter final {
  public:
    PythonGatewayWakeAdapter(ActorSystem& system,
                             ActorAddress gateway) noexcept;
    [[nodiscard]] GatewayWakePort port() noexcept;

  private:
    static bool wake(void* context) noexcept;
    ActorSystem& system_;
    ActorAddress gateway_;
};
```

Because `kPythonWakeupTag` is deliberately in the protected system range,
`PythonGatewayActor::receive()` intercepts that tag before the base system-tag
switch and delegates every other tag to `EventBasedActor::receive()`. The wake
handler drains at most `max_commands_per_turn`, executes each command through
the fixed port, and pushes requested completions. When command depth remains
non-zero, it self-sends one empty `kPythonWakeupTag` message through
`context()->send(address(),
TypedMessage{kPythonWakeupTag, StreamBuffer{}})` and returns immediately.

`PythonGatewayWakeAdapter` owns stable references to `ActorSystem` and the
gateway address. Its static wake function constructs one empty wake message and
returns
`system_.deliver_with_result(gateway_.id, std::move(message)).accepted()` via
the existing delivery result API. `port()` returns
`{this, &PythonGatewayWakeAdapter::wake}`.

Append `src/python_gateway_actor.cpp` and
`src/python_gateway_wake_adapter.cpp` to `hpactor_python_native`, and append
`test_python_gateway_actor.cpp` to `test_unit_python_binding`, using
`target_sources()`.

- [ ] **Step 4: Run gateway, runtime, and scheduler tests to verify GREEN**

Run:

```bash
ninja -C build test_unit_python_binding test_unit_sched
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonGatewayActorTest.*:PythonRuntimeTest.*'
./build/tests/unit/sched/test_unit_sched \
  --gtest_filter='*ReadyGate*:*SchedulerControl*'
```

Expected: selected tests pass; the first gateway activation executes exactly
two commands and the second executes the remainder.

- [ ] **Step 5: Commit gateway execution**

```bash
git add bindings/python/native tests/unit/python
git commit -m "feat: add Python command gateway"
```

### Task 8: Add Phase 1A integration, stress, architecture, and status evidence

**Files:**
- Modify: `tests/integration/CMakeLists.txt`
- Create: `tests/integration/python/CMakeLists.txt`
- Create: `tests/integration/python/test_python_native_workflow.cpp`
- Create: `tests/unit/python/test_python_runtime_stress.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`
- Modify: `tests/architecture/CMakeLists.txt`
- Modify: `CLAUDE_MEMORY.md`
- Modify: `docs/superpowers/specs/2026-07-03-python-language-binding-design.md`

**Interfaces:**
- Consumes: all Phase 1A components.
- Produces: deterministic end-to-end native workflow, concurrent-producer accounting, architecture fitness checks, and honest project status documentation.

- [ ] **Step 1: Add integration target and failing end-to-end test**

Conditionally add `tests/integration/python` when Python bindings are enabled.
Create its CMake target:

```cmake
# tests/integration/CMakeLists.txt
if(ENABLE_PYTHON_BINDINGS)
    add_subdirectory(python)
endif()

# tests/integration/python/CMakeLists.txt
add_executable(test_integration_python_binding
    test_python_native_workflow.cpp)
target_link_libraries(test_integration_python_binding PRIVATE
    hpactor_python_native hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_python_binding PROPERTIES TIMEOUT 35)
```

Use this complete test body (with the project test-support includes and
`hpactor`/`hpactor::python` namespace aliases):

```cpp
namespace {
struct WorkflowExecutor {
    std::vector<uint64_t> commands;

    static python::PythonCommandExecution
    execute(void* context, const python::PythonCommand& command) noexcept {
        auto* self = static_cast<WorkflowExecutor*>(context);
        self->commands.push_back(command.sequence);
        python::PythonCompletion completion;
        completion.kind = python::PythonCompletionKind::CommandResult;
        completion.token = command.token;
        completion.sequence = command.sequence;
        completion.actor = command.origin;
        completion.generation = command.generation;
        return {true, std::move(completion)};
    }
};
} // namespace

TEST(PythonNativeWorkflowTest, PreservesOrderIdentityAndDrainAdmission) {
    Config cfg;
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    python::PythonRuntimeConfig runtime_cfg;
    runtime_cfg.max_commands_per_turn = 2;
    auto created = python::PythonRuntime::create(runtime_cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());

    WorkflowExecutor executor;
    auto gateway = system.spawn<python::PythonGatewayActor>(
        *runtime,
        python::PythonCommandExecutorPort{&executor,
                                           &WorkflowExecutor::execute});
    python::PythonGatewayWakeAdapter wake(system, gateway.address());
    ASSERT_TRUE(runtime->start(wake.port()).ok());

    auto lease = runtime->reserve_actor();
    ASSERT_TRUE(lease.has_value());
    auto bridge = system.spawn<python::PythonBridgeActor>(
        *runtime, std::move(*lease));
    auto* bridge_actor = static_cast<python::PythonBridgeActor*>(
        bridge.get().get());
    ASSERT_NE(bridge_actor, nullptr);

    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        TypedMessage message(TypeTag::User,
                             StreamBuffer{static_cast<uint8_t>(sequence)});
        mailbox::DeliveryOptions options;
        options.message_id = 100 + sequence;
        options.flags = static_cast<uint32_t>(sequence);
        ASSERT_TRUE(system.try_deliver_local(
            bridge.id(), std::move(message),
            static_cast<uint8_t>(sequence - 1),
            1'000 + static_cast<int64_t>(sequence), options).accepted());
    }
    ASSERT_TRUE(driver.drain_until([&] {
        return runtime->snapshot().queues.dispatch_depth == 3;
    }));

    std::vector<uint64_t> dispatch_sequences;
    ASSERT_EQ(runtime->drain_dispatch(3, [&](const auto& envelope) {
        EXPECT_EQ(envelope->actor.id, bridge.id());
        EXPECT_EQ(envelope->generation, bridge_actor->generation());
        dispatch_sequences.push_back(envelope->sequence);

        auto command = std::make_shared<python::PythonCommand>();
        command->kind = python::PythonCommandKind::Send;
        command->origin = envelope->actor;
        command->generation = envelope->generation;
        command->target = gateway.address();
        command->token = 200 + envelope->sequence;
        command->sequence = envelope->sequence;
        ASSERT_TRUE(runtime->try_push_command(command));
    }), 3u);
    EXPECT_EQ(dispatch_sequences, (std::vector<uint64_t>{1, 2, 3}));

    ASSERT_TRUE(driver.drain_until([&] {
        return runtime->snapshot().queues.completion_depth == 3;
    }));
    EXPECT_EQ(executor.commands, (std::vector<uint64_t>{1, 2, 3}));

    std::vector<uint64_t> completion_sequences;
    ASSERT_EQ(runtime->drain_completions(3, [&](const auto& completion) {
        completion_sequences.push_back(completion->sequence);
    }), 3u);
    EXPECT_EQ(completion_sequences, (std::vector<uint64_t>{1, 2, 3}));

    runtime->begin_draining();
    EXPECT_FALSE(runtime->try_push_dispatch(
        std::make_shared<python::PythonDispatchEnvelope>()));
    EXPECT_FALSE(runtime->try_push_command(
        std::make_shared<python::PythonCommand>()));
    ASSERT_TRUE(runtime->stop().ok());
    EXPECT_EQ(runtime->snapshot().state, python::PythonRuntimeState::Stopped);
}
```

- [ ] **Step 2: Add concurrent-producer accounting stress test**

Add `test_python_runtime_stress.cpp` with this no-sleep concurrent producer
test:

```cpp
namespace {
struct StressWakeProbe {
    static bool wake(void*) noexcept { return true; }
};
} // namespace

TEST(PythonRuntimeStressTest, AccountsForEveryConcurrentDispatchAttempt) {
    constexpr size_t kProducers = 4;
    constexpr size_t kAttemptsPerProducer = 2'000;
    python::PythonRuntimeConfig cfg;
    cfg.dispatch_queue_capacity = 256;
    auto created = python::PythonRuntime::create(cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());
    StressWakeProbe wake;
    ASSERT_TRUE(runtime->start({&wake, &StressWakeProbe::wake}).ok());

    std::atomic<size_t> producers_done{0};
    std::atomic<size_t> accepted{0};
    std::atomic<size_t> rejected{0};
    std::atomic<size_t> consumed{0};
    std::atomic<size_t> max_observed_depth{0};

    std::thread consumer([&] {
        while (producers_done.load(std::memory_order_acquire) != kProducers ||
               runtime->snapshot().queues.dispatch_depth != 0) {
            consumed.fetch_add(runtime->drain_dispatch(64, [](const auto&) {}),
                               std::memory_order_relaxed);
            const size_t depth = runtime->snapshot().queues.dispatch_depth;
            size_t prior = max_observed_depth.load(std::memory_order_relaxed);
            while (prior < depth && !max_observed_depth.compare_exchange_weak(
                       prior, depth, std::memory_order_relaxed)) {}
            std::this_thread::yield();
        }
    });

    std::array<std::thread, kProducers> producers;
    for (size_t producer = 0; producer < kProducers; ++producer) {
        producers[producer] = std::thread([&, producer] {
            for (size_t attempt = 0; attempt < kAttemptsPerProducer; ++attempt) {
                auto envelope =
                    std::make_shared<python::PythonDispatchEnvelope>();
                envelope->sequence = producer * kAttemptsPerProducer + attempt;
                if (runtime->try_push_dispatch(envelope)) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    consumer.join();

    EXPECT_EQ(accepted.load() + rejected.load(), 8'000u);
    EXPECT_EQ(consumed.load(), accepted.load());
    EXPECT_EQ(runtime->snapshot().queues.dispatch_rejected, rejected.load());
    EXPECT_LE(max_observed_depth.load(), 256u);
    EXPECT_TRUE(runtime->stop().ok());
}
```

Use atomics and `std::this_thread::yield()`; do not sleep.

- [ ] **Step 3: Add architecture fitness checks**

In `tests/architecture/CMakeLists.txt`, enumerate the binding target's own files
and the shared runtime directories separately, then add checks using
`assert_file_excludes.cmake`:

```cmake
file(GLOB_RECURSE _python_native_files CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/bindings/python/native/include/*.hpp
    ${CMAKE_SOURCE_DIR}/bindings/python/native/src/*.cpp)
set(_python_native_forbidden
    "Python.h" "PyObject" "dynamic_cast" "typeid" "throw "
    "try {" "catch (" "std::function")
foreach(_file IN LISTS _python_native_files)
    string(MAKE_C_IDENTIFIER "${_file}" _python_test_suffix)
    add_test(
        NAME PythonBindingArchitecture.NoPythonOrRtti.${_python_test_suffix}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${_file}
            "-DFORBIDDEN=${_python_native_forbidden}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake)
endforeach()

file(GLOB_RECURSE _python_forbidden_core_files CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/include/hpactor/adt/*.hpp
    ${CMAKE_SOURCE_DIR}/include/hpactor/actor/*.hpp
    ${CMAKE_SOURCE_DIR}/include/hpactor/sched/*.hpp
    ${CMAKE_SOURCE_DIR}/src/actor/*.cpp
    ${CMAKE_SOURCE_DIR}/src/actor/*.hpp
    ${CMAKE_SOURCE_DIR}/src/sched/*.cpp
    ${CMAKE_SOURCE_DIR}/src/sched/*.hpp
    ${CMAKE_SOURCE_DIR}/src/runtime/*.cpp
    ${CMAKE_SOURCE_DIR}/src/runtime/*.hpp)
foreach(_file IN LISTS _python_forbidden_core_files)
    string(MAKE_C_IDENTIFIER "${_file}" _python_core_test_suffix)
    add_test(
        NAME PythonBindingArchitecture.NoPythonInCore.${_python_core_test_suffix}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=${_file}
            "-DFORBIDDEN=#include <Python.h>;#include \"Python.h\""
            -P ${CMAKE_CURRENT_SOURCE_DIR}/assert_file_excludes.cmake)
endforeach()
```

Do not scan third-party sources.

- [ ] **Step 4: Run focused integration and architecture tests to verify GREEN**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
  -DENABLE_PYTHON_BINDINGS=ON
ninja -C build test_unit_python_binding test_integration_python_binding
ctest --test-dir build -R 'PythonBinding' --output-on-failure
```

Expected: all Python binding unit, integration, and architecture tests pass.

- [ ] **Step 5: Update project status without advertising a public binding**

Add a dated `Python Binding Phase 1A Native Foundation` entry to
`CLAUDE_MEMORY.md` listing the native library, bounded queues, notifier,
runtime, bridge/gateway actors, tests, and explicit statement:

```text
No public Python actor API or distributable wheel exists yet; the manual's
language-binding limitation remains accurate until Phase 1D.
```

In the design spec, change the status line to:

```markdown
**Status:** Approved design; Phase 1A native foundation implemented
```

- [ ] **Step 6: Run full build and test verification**

Because CMake and `TypedMessage` changed, run:

```bash
ninja -C build
ctest --test-dir build --output-on-failure --parallel 8
git diff --check
```

Expected: build succeeds, all configured tests pass, and `git diff --check`
prints no output.

On a supported Linux CI runner, also run:

```bash
cmake -S . -B build-tsan -GNinja \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
  -DENABLE_PYTHON_BINDINGS=ON -DENABLE_TSAN=ON
ninja -C build-tsan test_unit_python_binding test_integration_python_binding
ctest --test-dir build-tsan -R 'PythonBinding' --output-on-failure
```

Expected: targeted TSAN tests pass without race reports.

- [ ] **Step 7: Commit Phase 1A acceptance evidence**

```bash
git add tests/integration/python tests/integration/CMakeLists.txt \
  tests/unit/python tests/architecture/CMakeLists.txt \
  CLAUDE_MEMORY.md \
  docs/superpowers/specs/2026-07-03-python-language-binding-design.md
git commit -m "test: verify Python binding native foundation"
```

## Plan Completion Checklist

- [ ] Every native queue has explicit capacity, rejection accounting, and one documented consumer.
- [ ] Dispatch is MPSC; command and completion use the same proven ring with one logical producer in Phase 1A.
- [ ] `TypedMessage` preserves all metadata required by the language boundary.
- [ ] Actor lease generation prevents stale replacement access.
- [ ] Bridge rejection never emits a false reliable ACK.
- [ ] Gateway work is budgeted and requeues through normal actor delivery.
- [ ] No native binding file stores or references a Python object.
- [ ] No scheduler worker invokes CPython or blocks on a notifier.
- [ ] Shutdown closes admission and descriptors idempotently.
- [ ] Existing builds remain unchanged when `ENABLE_PYTHON_BINDINGS=OFF`.
- [ ] Focused TSAN evidence exists on supported Linux CI.
- [ ] Full build and test suite pass after the public-header/CMake changes.
