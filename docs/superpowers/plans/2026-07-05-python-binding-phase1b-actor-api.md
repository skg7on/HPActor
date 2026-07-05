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

# Python Binding Phase 1B Actor API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the first usable in-process Python actor API—protobuf registration, imperative spawn, serialized async handlers, send/reply/ask, delivery receipts, scheduling, lifecycle hooks, and deterministic Python tests—on top of the Phase 1A native foundation.

**Architecture:** A thin CPython limited-API module converts Python values to and from the Phase 1A value contracts but never stores borrowed Python references or calls Python from C++. One dedicated pure-Python asyncio loop owns every Python actor, behavior, handler task, and future; generated local-only protobuf controls return commands to the originating C++ bridge actor so all `ActorContext` operations remain actor-owned. A bounded global Python dispatch coordinator preserves one non-reentrant FIFO turn per actor across `await` without creating an unbounded per-actor queue.

**Tech Stack:** C++20, CPython limited C API with `Py_LIMITED_API=0x030B0000`, Python 3.11+, asyncio, Protocol Buffers generated Python classes, CMake/Ninja, GoogleTest, Python `unittest`, Linux `eventfd`, and macOS non-blocking socket pairs.

## Global Constraints

- Execute only after every task in `docs/superpowers/plans/2026-07-03-python-binding-phase1a-native-foundation.md` is implemented and its acceptance suite passes.
- Work only in `.claude/worktrees/python-binding-design` on `worktree/python-binding-design`.
- Support CPython 3.11+ on Linux x86_64/ARM64 and macOS x86_64/ARM64; do not add Windows behavior.
- Compile native binding translation units with `-fno-exceptions` and `-fno-rtti`.
- Define `Py_LIMITED_API=0x030B0000`; use raw CPython C APIs and explicit null/error checks, not pybind11 or nanobind.
- Python headers may appear only under `bindings/python/native/src/python_capi/`; they must not enter `hpactor_lib`, public HPActor headers, scheduler, mailbox, transport, or value-only native bridge files.
- Native queues and protected control messages contain values only: no `PyObject*`, borrowed Python buffers, callback closures, or pointers to stack data.
- Only generated protobuf messages cross the language boundary. Serialization always occurs on the Python runtime loop with `deterministic=True`.
- Cross-language application tags are explicit integers from `0x1000` through `0x00FFFFFF`; runtime-allocated C++ tags are rejected by the Python registry.
- Internal tags `0xF0` through `0xF3` remain local-only and are rejected at both ordinary and batch remote-frame ingress.
- Exactly one dedicated asyncio loop thread owns Python actors. Application-loop calls marshal to it; handler calls already on it execute directly.
- A Python actor executes at most one handler or lifecycle hook at a time, including while that coroutine is suspended.
- The Python-side pending dispatch deque is bounded by `dispatch_queue_capacity`; native draining never removes more envelopes than the available Python-side slots.
- Command and completion token registries are bounded by their configured queue capacities and reject new work with `ResourceExhausted` rather than growing.
- Phase 1B implements normal actor creation and failure commands but not restart state recovery, DLQ/metrics/log/health surfaces, shutdown-under-load hardening, wheel repair, or declarative topology; those remain Phase 1C–1E.
- Tests use fake native ports, explicit notifier drains, paused scheduler stepping, events/futures, and generous condition timeouts; sleeps are not proof of progress.
- The developer manual continues to state that no official packaged Python binding exists until Phase 1D.
- Because this phase adds generated protobuf, CMake, a native module, shared completion hooks, and broad Python behavior, finish with a full configured build and test run.

## File Structure

### Shared completion and wire-hardening infrastructure

- `include/hpactor/msg/completion_port.hpp` — fixed function-pointer completion ports for move-only request and delivery outcomes.
- `include/hpactor/msg/request_handle.hpp` — non-blocking one-shot completion registration for asks.
- `include/hpactor/msg/delivery_receipt.hpp` / `src/msg/delivery_receipt.cpp` — fixed-port completion registration without removing the existing `std::function` API.
- `protos/hpactor/python_binding_internal.proto` — generated, versioned, local-only command and actor-failure values.
- `src/net/inbound_frame_router.cpp` — reject binding-only tags from ordinary and batch remote ingress.

### Native actor command path and facade

- `bindings/python/native/include/hpactor/python/python_command_router.hpp` / `src/python_command_router.cpp` — convert queue commands to protected protobuf controls and deliver them to the originating bridge.
- `bindings/python/native/include/hpactor/python/python_native_system.hpp` / `src/python_native_system.cpp` — value-only ownership facade for `ActorSystem`, runtime, gateway, application bridge, and bridge spawn/stop/name operations.
- Phase 1A bridge types, runtime snapshots, bridge actor, gateway actor, ports, and CMake files — extend command/completion fields and execute protected actor-owned operations.

### Limited-API extension

- `bindings/python/native/src/python_capi/module.cpp` — `_hpactor` module initialization and exported constants.
- `bindings/python/native/src/python_capi/native_system_type.cpp` / `.hpp` — heap type wrapping one `PythonNativeSystem*` and low-level value methods.
- `bindings/python/native/src/python_capi/conversions.cpp` / `.hpp` — checked conversion of configs, addresses, commands, dispatches, and completions.

### Pure-Python package

- `bindings/python/hpactor/_address.py` — immutable `ActorAddress`, `ActorRef`, and `ScheduleHandle` values.
- `bindings/python/hpactor/_errors.py` — explicit actor, delivery, ask, lifecycle, serialization, and resource exceptions.
- `bindings/python/hpactor/_delivery.py` — options, enums, delivery result, and awaitable receipt.
- `bindings/python/hpactor/_messages.py` — pre-start protobuf registry and deterministic serialization.
- `bindings/python/hpactor/_behavior.py` — immutable validated handler tables.
- `bindings/python/hpactor/_actor.py` — actor base, decorator, lifecycle hooks, and behavior replacement.
- `bindings/python/hpactor/_context.py` — handler-scoped actor context operations.
- `bindings/python/hpactor/_runtime.py` — dedicated loop thread, bounded dispatch coordinator, actor runners, and token registry.
- `bindings/python/hpactor/_system.py` — public async context manager and application-loop marshalling.
- `bindings/python/hpactor/__init__.py` / `py.typed` — public API and typing marker.

### Tests

- `tests/unit/types/test_request_handle.cpp` and `tests/unit/msg/test_delivery_receipt.cpp` — fixed completion ports.
- `tests/unit/python/` — generated control codec, command routing, bridge execution, and native-system facade.
- `tests/unit/net/test_inbound_frame_router.cpp` — remote protected-tag rejection at the frame-router boundary.
- `bindings/python/tests/unit/` — registry, behavior, receipt, coordinator, runner, and public type tests using a fake native system.
- `bindings/python/tests/integration/` — real `_hpactor` local echo, ask, scheduling, lifecycle, and ordering tests.
- `tests/architecture/CMakeLists.txt` — C API containment, protected-tag reservation, and no-Python-object bridge checks.

---

### Task 1: Add non-blocking fixed completion ports to request and delivery handles

**Files:**
- Create: `include/hpactor/msg/completion_port.hpp`
- Modify: `include/hpactor/msg/request_handle.hpp`
- Modify: `include/hpactor/msg/delivery_receipt.hpp`
- Modify: `src/msg/delivery_receipt.cpp`
- Modify: `tests/unit/types/test_request_handle.cpp`
- Modify: `tests/unit/msg/test_delivery_receipt.cpp`

**Interfaces:**
- Consumes: existing move-only `RequestHandle<T>`, `DeliveryReceipt`, `result<T>`, and `mailbox::DeliveryResult`.
- Produces: `CompletionPort<T>`, `RequestHandle<T>::on_complete(CompletionPort<result<T>>)`, and `DeliveryReceipt::on_complete(CompletionPort<mailbox::DeliveryResult>)` with exactly-once, non-blocking notification.

- [ ] **Step 1: Write failing fixed-port tests**

Add a file-local probe to each test translation unit and cover registration both before and after resolution:

```cpp
namespace {
template <typename T> struct CompletionProbe {
    size_t calls{0};
    std::optional<T> value;

    static void complete(void* context, T value) noexcept {
        auto* self = static_cast<CompletionProbe*>(context);
        ++self->calls;
        self->value.emplace(std::move(value));
    }
};
} // namespace

TEST(RequestHandleTest, FixedPortCompletesExactlyOnce) {
    RequestHandle<StreamBuffer> handle;
    CompletionProbe<result<StreamBuffer>> probe;
    ASSERT_TRUE(handle.on_complete({&probe, &CompletionProbe<result<StreamBuffer>>::complete}));

    handle.resolve(result<StreamBuffer>::make(StreamBuffer{1, 2, 3}));
    EXPECT_EQ(probe.calls, 1u);
    ASSERT_TRUE(probe.value.has_value());
    ASSERT_TRUE(probe.value->ok());
    EXPECT_EQ(probe.value->value(), (StreamBuffer{1, 2, 3}));
    EXPECT_FALSE(handle.on_complete({&probe, &CompletionProbe<result<StreamBuffer>>::complete}));
}

TEST(DeliveryReceiptTest, FixedPortFiresForAlreadyResolvedReceipt) {
    mailbox::DeliveryResult delivered;
    delivered.status = mailbox::DeliveryStatus::Accepted;
    msg::DeliveryReceipt receipt(delivered);
    CompletionProbe<mailbox::DeliveryResult> probe;

    ASSERT_TRUE(receipt.on_complete(
        {&probe, &CompletionProbe<mailbox::DeliveryResult>::complete}));
    EXPECT_EQ(probe.calls, 1u);
    ASSERT_TRUE(probe.value.has_value());
    EXPECT_EQ(probe.value->status, mailbox::DeliveryStatus::Accepted);
}
```

- [ ] **Step 2: Run the narrow tests to verify RED**

```bash
ninja -C build test_request_handle test_unit_msg
./build/tests/unit/types/test_request_handle --gtest_filter='RequestHandleTest.FixedPort*'
./build/tests/unit/msg/test_unit_msg --gtest_filter='DeliveryReceiptTest.FixedPort*'
```

Expected: compilation fails because `CompletionPort` and the fixed-port overloads do not exist.

- [ ] **Step 3: Implement the fixed port and one-shot state transition**

Create:

```cpp
#include <memory>
#include <utility>

template <typename T> struct CompletionPort final {
    using CompleteFn = void (*)(void*, T) noexcept;
    void* context{nullptr};
    CompleteFn complete{nullptr};
    std::shared_ptr<void> keepalive;

    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && complete != nullptr;
    }

    void operator()(T value) const noexcept {
        complete(context, std::move(value));
    }
};
```

Add `CompletionPort<result<T>> fixed_completion` and
`bool completion_consumed{false}` to `RequestHandle<T>::State`. Under the
existing state mutex, `on_complete()` either installs the port or moves the
already-ready result into a local. Invoke the port only after releasing the
mutex. `resolve()` similarly moves a local port/result pair out under the
mutex, marks the result consumed, releases the mutex, notifies the condition
variable, and invokes the port. Return `false` when a callback was already
installed or the result was already consumed.

The optional `keepalive` travels with every copied or moved port until callback
return. Generic callers may leave it empty; the Python binding passes a
heap-owned completion context so callback lifetime never depends on a bridge or
stack frame.

Add the same fixed-port fields and transition to
`DeliveryReceipt::SharedState`, preserving the existing
`on_complete(std::function<void(mailbox::DeliveryResult)>)` overload. Reject installation of both callback
forms on the same receipt. Neither fixed callback may run while the state mutex
is held.

- [ ] **Step 4: Run request, receipt, ask, and tracker tests to verify GREEN**

```bash
ninja -C build test_request_handle test_unit_msg
./build/tests/unit/types/test_request_handle --gtest_filter='RequestHandleTest.*'
./build/tests/unit/msg/test_unit_msg --gtest_filter='DeliveryReceiptTest.*:OutboundDeliveryTrackerTest.*'
```

Expected: all selected tests pass and existing blocking/polling APIs remain source-compatible.

- [ ] **Step 5: Commit fixed completion ports**

```bash
git add include/hpactor/msg/completion_port.hpp \
  include/hpactor/msg/request_handle.hpp \
  include/hpactor/msg/delivery_receipt.hpp src/msg/delivery_receipt.cpp \
  tests/unit/types/test_request_handle.cpp \
  tests/unit/msg/test_delivery_receipt.cpp
git commit -m "feat: add fixed native completion ports"
```

### Task 2: Add generated local command contracts and reject protected tags from remote ingress

**Files:**
- Create: `protos/hpactor/python_binding_internal.proto`
- Modify: `cmake/dependencies.cmake`
- Modify: `bindings/python/native/include/hpactor/python/python_bridge_types.hpp`
- Modify: `bindings/python/native/include/hpactor/python/python_runtime_snapshot.hpp`
- Modify: `src/net/inbound_frame_router.cpp`
- Modify: `tests/unit/python/test_python_contracts.cpp`
- Create: `tests/unit/python/test_python_command_proto.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`
- Create: `tests/unit/net/test_inbound_frame_router.cpp`
- Create: `tests/unit/net/inbound_frame_router_test_harness.hpp`
- Modify: `tests/unit/net/CMakeLists.txt`

**Interfaces:**
- Consumes: Phase 1A `PythonCommand`, `PythonCompletion`, local tags `0xF0`–`0xF3`, `net::to_proto`, and `net::from_proto`.
- Produces: generated `hpactor::python::internal::PbPythonActorCommand`, `PbPythonActorFailed`, exact command/completion metadata, and ingress rejection for all protected binding tags.

- [ ] **Step 1: Write failing contract and ingress tests**

```cpp
TEST(PythonCommandProtoTest, RoundTripsActorOwnedCommandFields) {
    python::PythonCommand command;
    command.kind = python::PythonCommandKind::Reply;
    command.token = 41;
    command.sequence = 7;
    command.generation = 9;
    command.origin = ActorAddress{EndPoint{LocalEndpoint}, ActorType{4}, ActorId{5}, 6};
    command.target = ActorAddress{EndPoint{LocalEndpoint}, ActorType{8}, ActorId{10}, 11};
    command.reply_to = ActorAddress{EndPoint{LocalEndpoint}, ActorType{12}, ActorId{13}, 14};
    command.type_tag = static_cast<TypeTag>(0x1001);
    command.payload = StreamBuffer{1, 2, 3};
    command.ask_message_id = 99;
    command.priority = 2;
    command.deadline_ns = 700;
    command.flags = 0x40;

    auto encoded = python::encode_actor_command(command);
    ASSERT_TRUE(encoded.ok());
    auto decoded = python::decode_actor_command(encoded.value());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().kind, command.kind);
    EXPECT_EQ(decoded.value().origin, command.origin);
    EXPECT_EQ(decoded.value().reply_to, command.reply_to);
    EXPECT_EQ(decoded.value().payload, command.payload);
    EXPECT_EQ(decoded.value().ask_message_id, 99u);
}

TEST(InboundFrameRouterTest, RejectsPythonProtectedTagsFromRemoteData) {
    for (uint32_t raw_tag = 0xF0; raw_tag <= 0xF3; ++raw_tag) {
        auto frame = make_data_frame(static_cast<TypeTag>(raw_tag));
        auto result = router_.route(frame);
        EXPECT_EQ(result.code, net::FrameDispatchCode::InvalidControlPayload);
    }
    EXPECT_EQ(delivery_probe_.calls, 0u);
}

TEST(InboundFrameRouterTest, RejectsPythonProtectedTagsInsideRemoteBatch) {
    auto frame = make_batch_frame({TypeTag::User, static_cast<TypeTag>(0xF1)});
    auto result = router_.route(frame);
    EXPECT_EQ(result.code, net::FrameDispatchCode::InvalidControlPayload);
    EXPECT_EQ(delivery_probe_.calls, 0u);
}
```

The harness owns real stopped `MessagingRuntime`, `RpcChannel`, and
`StreamRuntime` instances wired to fixed test ports plus one
`InboundFrameRouter`. `make_data_frame(TypeTag)` fills valid local sender and
receiver protobuf addresses; `make_batch_frame(std::initializer_list<TypeTag>)`
fills valid common addresses and one entry per tag. Its delivery port increments
`delivery_probe_.calls`, so both tests prove rejection occurs before messaging
delivery rather than merely testing a numeric helper.

- [ ] **Step 2: Regenerate and build to verify RED**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF -DENABLE_PYTHON_BINDINGS=ON
ninja -C build test_unit_python_binding test_unit_net
```

Expected: compilation fails because the internal proto, codec, fields, and ingress rejection do not exist.

- [ ] **Step 3: Define the generated local-only protobuf contract**

Create a proto3 file in package `hpactor.python.internal` importing
`hpactor/common.proto`. Define `PythonCommandKind` values `SEND=0`, `REPLY=1`,
`REPLY_ERROR=2`, `ASK=3`, `SPAWN=4`, `SCHEDULE=5`, `CANCEL_SCHEDULE=6`,
`LINK=7`, `UNLINK=8`, `MONITOR=9`, `DEMONITOR=10`, `STOP=11`, `PASSIVATE=12`,
`ACTOR_FAILED=13`, `INSPECT=14`, and `CANCEL_ASK=15`.

Define `PbPythonActorCommand` with these exact fields:

```proto
uint32 version = 1;
PythonCommandKind kind = 2;
uint64 token = 3;
uint64 sequence = 4;
uint64 generation = 5;
hpactor.PbActorAddress origin = 6;
hpactor.PbActorAddress target = 7;
hpactor.PbActorAddress reply_to = 8;
uint32 type_tag = 9;
bytes payload = 10;
uint64 message_id = 11;
uint64 ask_message_id = 12;
uint32 priority = 13;
sint64 deadline_ns = 14;
uint32 flags = 15;
uint64 delay_ns = 16;
uint64 schedule_handle = 17;
uint32 error_code = 18;
string detail = 19;
string actor_name = 20;
uint32 delivery_mode = 21;
bool no_drop = 22;
bool emit_backpressure = 23;
```

Define `PbPythonActorFailed` with version, actor, generation, exception type,
message, traceback, and sequence fields. Comments state that both messages are
process-local controls and must never enter an HPActor wire frame.

Append `CancelAsk = 15` to `PythonCommandKind` without renumbering the Phase 1A
values; add `reply_to`, `message_id`,
`ask_message_id`, `schedule_handle`, bounded `detail`, `actor_name`, and
delivery-option values to `PythonCommand`. Append
`ScheduleResult = 5`, `ActorStopped = 6`, and `ActorFailed = 7` to
`PythonCompletionKind`; extend
`PythonCompletion` with `FailureSource source`, `uint32_t error_code`, bounded
detail bytes, `uint64_t schedule_handle`, `mailbox::DeliveryStatus`, and
`int64_t retry_after_ns`.

The codec sets version `1`, rejects other versions, payloads larger than
16 MiB, details/tracebacks larger than 16 KiB, names larger than 255 bytes,
invalid command kinds, invalid application tags, and malformed addresses.

- [ ] **Step 4: Reject protected tags before ordinary or batch delivery**

Add one helper local to `inbound_frame_router.cpp`:

```cpp
constexpr bool is_python_binding_control_tag(uint32_t tag) noexcept {
    return tag >= 0xF0u && tag <= 0xF3u;
}
```

Call it in `route_data_payload()` before `deliver_ordinary_data()` and in
`route_batch()` while validating every entry. Reject the entire batch before
delivering any entry. Return `FrameDispatchCode::InvalidControlPayload` and set
`detail_code` to the rejected raw tag. Add `${CMAKE_SOURCE_DIR}/src` to the
unit-net target's private include directories so the test exercises the real
internal router.

- [ ] **Step 5: Run codec, tag, and network tests to verify GREEN**

```bash
ninja -C build hpactor_proto test_unit_python_binding test_unit_net
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonContractsTest.*:PythonCommandProtoTest.*'
./build/tests/unit/net/test_unit_net \
  --gtest_filter='InboundFrameRouterTest.*PythonProtected*'
```

Expected: all selected tests pass; generated files build without changing the public network frame schema.

- [ ] **Step 6: Commit protected command contracts**

```bash
git add protos/hpactor/python_binding_internal.proto cmake/dependencies.cmake \
  bindings/python/native tests/unit/python \
  src/net/inbound_frame_router.cpp \
  tests/unit/net/test_inbound_frame_router.cpp \
  tests/unit/net/inbound_frame_router_test_harness.hpp \
  tests/unit/net/CMakeLists.txt
git commit -m "feat: add Python actor command contracts"
```

### Task 3: Execute Python commands on the originating bridge actor turn

**Files:**
- Create: `bindings/python/native/include/hpactor/python/python_command_router.hpp`
- Create: `bindings/python/native/src/python_command_router.cpp`
- Modify: `include/hpactor/actor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`
- Modify: `bindings/python/native/include/hpactor/python/python_bridge_actor.hpp`
- Modify: `bindings/python/native/src/python_bridge_actor.cpp`
- Modify: `bindings/python/native/include/hpactor/python/python_ports.hpp`
- Modify: `bindings/python/native/CMakeLists.txt`
- Create: `tests/unit/python/python_command_test_harness.hpp`
- Create: `tests/unit/python/test_python_command_router.cpp`
- Create: `tests/unit/python/test_python_bridge_commands.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`
- Create: `tests/unit/actor/test_actor_context_ask_raw.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`

**Interfaces:**
- Consumes: `PythonCommandExecutorPort`, command proto codec, `ActorContext`, fixed completion ports, and Phase 1A runtime queues.
- Produces: `PythonCommandRouter::port()`, protected F1 delivery, actor-generation validation, actor-owned command execution, and value-only completions.

- [ ] **Step 1: Write failing router and bridge command tests**

Use a paused scheduler and the Phase 1A runtime. Verify the gateway router
delivers one F1 system-lane message to the origin bridge and that a send command
preserves the origin as sender:

```cpp
TEST(PythonBridgeCommandsTest, SendRunsOnOriginTurnAndCompletesReceipt) {
    NativeCommandFixture fixture;
    auto target = fixture.system.spawn<EnvelopeProbeActor>(fixture.observation);
    python::PythonCommand command;
    command.kind = python::PythonCommandKind::Send;
    command.origin = fixture.bridge.address();
    command.generation = fixture.bridge_actor()->generation();
    command.target = target.address();
    command.type_tag = TypeTag::User;
    command.payload = StreamBuffer{4, 5, 6};
    command.token = 77;
    command.sequence = 1;

    ASSERT_TRUE(fixture.runtime.try_push_command(
        std::make_shared<const python::PythonCommand>(command)));
    ASSERT_TRUE(fixture.driver.drain_until([&] {
        return fixture.runtime.snapshot().queues.completion_depth == 1;
    }));
    EXPECT_EQ(fixture.observation.sender, fixture.bridge.address());
    EXPECT_EQ(fixture.observation.payload, (StreamBuffer{4, 5, 6}));
    EXPECT_EQ(fixture.take_completion()->token, 77u);
}
```

Add focused tests for reply sender/ask restoration, ask success and timeout,
schedule/cancel handle round-trip, link/unlink, monitor/demonitor, stop,
passivate, child bridge spawn, stale generation, malformed controls, and a
maximum of `command_queue_capacity` pending async receipts/asks.

Define `NativeCommandFixture` in the support header. It owns, in declaration
order, a paused `ActorSystem`, `SchedulerTestDriver`, started `PythonRuntime`,
`PythonCommandRouter`, gateway actor/wake adapter, one reserved bridge, and a
bounded observation probe. Expose `bridge`, `runtime`, `system`, `driver`,
`bridge_actor()`, and `take_completion()` exactly as used above. Its destructor
stops the runtime before destroying the wake adapter.

- [ ] **Step 2: Build command tests to verify RED**

```bash
ninja -C build test_unit_python_binding
```

Expected: compilation fails because the router and protected command execution do not exist.

- [ ] **Step 3: Implement the fixed router port**

Declare:

```cpp
class PythonCommandRouter final {
  public:
    PythonCommandRouter(ActorSystem& system, PythonRuntime& runtime) noexcept;
    [[nodiscard]] PythonCommandExecutorPort port() noexcept;

  private:
    static PythonCommandExecution execute(
        void* context, const PythonCommand& command) noexcept;
    ActorSystem& system_;
    PythonRuntime& runtime_;
};
```

`execute()` validates `origin.id != 0` and `runtime_.generation_matches()`.
For ordinary commands it encodes `PbPythonActorCommand` and delivers
`TypedMessage{kPythonActorCommandTag, bytes}` to the origin bridge. For
`ActorFailed` it encodes `PbPythonActorFailed` and delivers
`kPythonActorFailedTag`. Both messages carry the origin address as sender and
use normal local delivery. The router returns an immediate completion only
when validation, encoding, or protected-message delivery fails; successful
actor-owned work emits its completion later from the bridge.

- [ ] **Step 4: Implement bridge command execution and bounded pending state**

Before changing the bridge, add a source-compatible typed overload:

```cpp
RequestHandle<StreamBuffer>
ask_raw(const ActorAddress& target, TypeTag request_type,
        const StreamBuffer& encoded_request,
        RequestTimeout timeout = RequestTimeout::use_default());
```

The existing three-argument overload delegates with `TypeTag::Invalid`. The
new overload creates `TypedMessage(request_type, encoded_request)`, stamps the
generated ask ID, and uses the existing local ask manager. Add an actor unit
test proving the delivered request retains `0x1000` and its reply resolves the
handle.

Intercept `kPythonActorCommandTag` and `kPythonActorFailedTag` before delegating
other system tags. Decode and validate the origin/generation against the bridge
lease, then use a single ordinary-command switch with these exact operations:

- `Send`: `context()->try_send(target, TypedMessage(type_tag, payload), options)`.
- `Reply`: temporarily restore `reply_to` and `ask_message_id`, then call `context()->try_reply()` and clear both values.
- `ReplyError`: restore reply context, call `context()->reply_with_error(error(error_code, detail))`, clear context, and emit success.
- `Ask`: convert `delay_ns` to checked, ceiling-rounded milliseconds and call `context()->ask_raw(target, type_tag, payload, RequestTimeout::from_ms(timeout_ms))`; zero selects `use_default()`. Register the handle's fixed completion port.
- `Spawn`: reserve a lease, spawn a child `PythonBridgeActor` via `context()->spawn`, and return its address/generation.
- `Schedule`: call `schedule()` or `schedule_to()` and return `AlarmHandle::value()`.
- `CancelSchedule`: reconstruct `AlarmHandle{schedule_handle}` and call `cancel_schedule()`.
- `Link`/`Unlink`: call bridge `link_to()`/`unlink_from()`.
- `Monitor`/`Demonitor`: call bridge `monitor()`/`demonitor()`.
- `Stop`: call `context()->stop(target.id)`.
- `Passivate`: call `context()->passivate()`.
- `CancelAsk`: locate the actor-owned pending token and call `RequestHandle::cancel()`.
- `Inspect`: return a typed unsupported completion; Phase 1C owns inspection and operations surfaces.

The separate F2 handler decodes bounded actor-failure metadata, pushes one
`ActorFailed` completion, records `FailureSource::LanguageBinding`, and calls
`context()->stop(id())` without throwing. Phase 1C connects this value to full
supervision, structured failure envelopes, logs, metrics, and DLQ policy.
`on_deactivate()` pushes one `ActorStopped` completion with
the bridge address/generation before releasing its lease, allowing external C++
stop paths to trigger Python `on_stop()`.

Store shared pending ask/receipt contexts in a bridge-owned cancellation map
bounded by `command_queue_capacity`. Each context contains only runtime pointer,
token, sequence, actor address/generation, move-only native handle, and an
atomic completed bit. Install the port as
`{context.get(), callback, context}` so `keepalive` owns the heap context through
callback return even if the bridge stops. Callbacks convert outcomes to
`PythonCompletion` and push them to the runtime; they never touch Python.
Before each command, erase completed contexts on the actor turn. On deactivate,
cancel every incomplete handle before clearing the map. Reject a new pending
operation with `ResourceExhausted` when the bound is reached.

- [ ] **Step 5: Run bridge command and existing actor tests to verify GREEN**

```bash
ninja -C build test_unit_python_binding test_unit_actor test_integration_actor
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonCommandRouterTest.*:PythonBridgeCommandsTest.*'
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorContextAskRawTest.*:*Schedule*:*Link*:*Monitor*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*DeliverySemantics*:*LinkMonitor*:*Passivation*'
```

Expected: selected tests pass; no scheduler worker blocks while an ask or delivery receipt is pending.

- [ ] **Step 6: Commit actor-owned command execution**

```bash
git add bindings/python/native tests/unit/python \
  include/hpactor/actor/actor_context.hpp src/actor/actor_context.cpp \
  tests/unit/actor/test_actor_context_ask_raw.cpp \
  tests/unit/actor/CMakeLists.txt
git commit -m "feat: execute Python commands on bridge actors"
```

### Task 4: Add a value-only native system facade

**Files:**
- Create: `bindings/python/native/include/hpactor/python/python_native_system.hpp`
- Create: `bindings/python/native/src/python_native_system.cpp`
- Modify: `bindings/python/native/CMakeLists.txt`
- Create: `tests/unit/python/test_python_native_system.cpp`
- Modify: `tests/unit/python/CMakeLists.txt`

**Interfaces:**
- Consumes: `ActorSystem`, `PythonRuntime`, gateway actor, wake adapter, command router, bridge actor, and runtime config.
- Produces: `PythonNativeSystem::create/start/stop`, application-origin address/generation, top-level bridge spawn, name registration, queue/notifier access, and no Python dependencies.

- [ ] **Step 1: Write a failing lifecycle and spawn test**

```cpp
TEST(PythonNativeSystemTest, StartsApplicationBridgeAndSpawnsNamedActor) {
    auto created = python::PythonNativeSystem::create({}, {});
    ASSERT_TRUE(created.ok());
    auto native = std::move(created.value());
    ASSERT_TRUE(native->start().ok());
    EXPECT_NE(native->application_origin().id, ActorId{});
    EXPECT_GT(native->application_generation(), 0u);

    auto spawned = native->spawn_bridge();
    ASSERT_TRUE(spawned.ok());
    EXPECT_NE(spawned.value().address.id, ActorId{});
    EXPECT_GT(spawned.value().generation, 0u);
    ASSERT_TRUE(native->register_name("echo", spawned.value().address).ok());
    EXPECT_EQ(native->resolve_name("echo"), spawned.value().address);

    ASSERT_TRUE(native->stop_bridge(spawned.value().address).ok());
    ASSERT_TRUE(native->stop().ok());
    EXPECT_TRUE(native->stop().ok());
}
```

- [ ] **Step 2: Build the facade test to verify RED**

```bash
ninja -C build test_unit_python_binding
```

Expected: compilation fails because `PythonNativeSystem` does not exist.

- [ ] **Step 3: Implement exact facade ownership and state transitions**

Declare `PythonSpawnedActor { ActorAddress address; uint64_t generation; }` and:

```cpp
class PythonNativeSystem final {
  public:
    static result<std::unique_ptr<PythonNativeSystem>>
    create(Config system_config, PythonRuntimeConfig python_config) noexcept;
    ~PythonNativeSystem();

    [[nodiscard]] result<void> start() noexcept;
    [[nodiscard]] result<void> begin_draining() noexcept;
    [[nodiscard]] result<void> stop() noexcept;
    [[nodiscard]] result<PythonSpawnedActor>
    spawn_bridge() noexcept;
    [[nodiscard]] result<void> stop_bridge(ActorAddress actor) noexcept;
    [[nodiscard]] result<void>
    register_name(std::string_view name, ActorAddress actor) noexcept;
    [[nodiscard]] ActorAddress resolve_name(std::string_view name) const noexcept;
    [[nodiscard]] ActorAddress application_origin() const noexcept;
    [[nodiscard]] uint64_t application_generation() const noexcept;
    [[nodiscard]] bool submit(const PythonCommandPtr& command) noexcept;
    [[nodiscard]] int dispatch_read_fd() const noexcept;
    [[nodiscard]] int completion_read_fd() const noexcept;
    template <typename Fn> size_t drain_dispatch(size_t budget, Fn&& fn);
    template <typename Fn> size_t drain_completions(size_t budget, Fn&& fn);
    [[nodiscard]] PythonRuntimeSnapshot snapshot() const noexcept;
};
```

Construction owns components in destruction-safe order: `ActorSystem`,
`PythonRuntime`, `PythonCommandRouter`, gateway ref, wake adapter, application
bridge ref. `create()` constructs the existing `ActorSystem`, whose current
constructor starts its runtime services. `start()` spawns the gateway and one
hidden application bridge, then starts the Python runtime with the adapter.
Roll back those additions in reverse order on failure. `stop()` begins runtime drain, stops user bridges,
stops the application bridge and gateway, stops the actor system, and finally
closes the Python runtime. All methods return explicit `result` values.

Names are registered only after the caller reports successful Python object
construction; failed construction stops the unnamed bridge, preventing
external publication of a half-started actor.

- [ ] **Step 4: Run facade and Phase 1A lifecycle tests to verify GREEN**

```bash
ninja -C build test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonNativeSystemTest.*:PythonRuntimeTest.*:PythonGatewayActorTest.*'
```

Expected: all selected tests pass and a second stop is harmless.

- [ ] **Step 5: Commit the native system facade**

```bash
git add bindings/python/native tests/unit/python
git commit -m "feat: add Python native system facade"
```

### Task 5: Build the CPython 3.11 limited-API `_hpactor` module

**Files:**
- Create: `bindings/python/native/src/python_capi/module.cpp`
- Create: `bindings/python/native/src/python_capi/native_system_type.hpp`
- Create: `bindings/python/native/src/python_capi/native_system_type.cpp`
- Create: `bindings/python/native/src/python_capi/conversions.hpp`
- Create: `bindings/python/native/src/python_capi/conversions.cpp`
- Modify: `bindings/python/native/CMakeLists.txt`
- Create: `bindings/python/tests/unit/test_native_module.py`
- Create: `bindings/python/tests/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `tests/architecture/CMakeLists.txt`

**Interfaces:**
- Consumes: `PythonNativeSystem` and value-only bridge structs.
- Produces: importable `_hpactor.NativeSystem`, integer constants, checked tuple schemas, and no retained Python objects below the C API wrapper.

- [ ] **Step 1: Add a failing Python limited-module smoke test**

```python
import unittest
import _hpactor


class NativeModuleTest(unittest.TestCase):
    def test_constants_and_lifecycle(self) -> None:
        self.assertEqual(_hpactor.PY_LIMITED_API, 0x030B0000)
        native = _hpactor.NativeSystem({"dispatch_queue_capacity": 64})
        native.start()
        self.assertGreater(native.dispatch_fd, -1)
        self.assertGreater(native.completion_fd, -1)
        origin, generation = native.application_origin()
        self.assertEqual(len(origin), 6)
        self.assertGreater(generation, 0)
        native.stop()
        native.stop()


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Configure and run the module test to verify RED**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF -DENABLE_PYTHON_BINDINGS=ON
ninja -C build _hpactor
ctest --test-dir build -R 'PythonNativeModule' --output-on-failure
```

Expected: configure or build fails because the module target and C API sources do not exist.

- [ ] **Step 3: Add the limited-API module target**

In the binding CMake file, find Python `3.11` with `Interpreter` and
`Development`, create `_hpactor` as a `MODULE`, link `hpactor_python_native`,
`hpactor_lib`, and `Python3::Python`, set `PREFIX ""`, define
`Py_LIMITED_API=0x030B0000`, and compile with `-fno-exceptions -fno-rtti`.
Register Python tests with `cmake -E env`, placing the module output directory
and `bindings/python` on `PYTHONPATH`.

The `NativeSystem` config dictionary accepts only
`dispatch_queue_capacity`, `command_queue_capacity`,
`completion_queue_capacity`, `max_actor_bindings`,
`max_dispatch_per_tick`, `max_commands_per_turn`, `loop_lag_unready_ms`,
`handler_shutdown_timeout_ms`, and `trace_handler_spans`. Missing keys use the
Phase 1A defaults; unknown keys and invalid values raise `ValueError` before
native construction.

- [ ] **Step 4: Implement the heap type and exact low-level schemas**

Use `PyType_FromSpec`; do not access `PyTypeObject` fields directly. The
instance stores exactly one owned `PythonNativeSystem*`. Deallocation calls
`stop()`, deletes the pointer, and delegates to `tp_free`.

Expose:

```text
NativeSystem(config: dict)
start() -> None
begin_draining() -> None
stop() -> None
application_origin() -> (address_tuple, generation)
spawn_bridge() -> (address_tuple, generation)
stop_bridge(address) -> None
register_name(name, address) -> None
submit(command_tuple) -> bool
drain_dispatch(max_items) -> list[dispatch_tuple]
drain_completions(max_items) -> list[completion_tuple]
dispatch_fd: int
completion_fd: int
snapshot() -> dict[str, int]
```

Address tuples are `(family, packed_address, port, actor_type, actor_id,
incarnation)`, where family is `0`, `4`, or `6` and packed address lengths are
`0`, `4`, or `16`. Command tuples are exactly `(kind, token, sequence,
generation, origin, target, reply_to, type_tag, payload, message_id,
ask_message_id, priority, deadline_ns, flags, delay_ns, schedule_handle,
error_code, detail, actor_name, delivery_mode, no_drop, emit_backpressure)`.
Dispatch tuples are exactly `(actor, generation, type_tag, payload, sender,
message_id, ask_message_id, trace_or_none, priority, deadline_ns, flags,
ack_requested, sequence)`, where a trace is `(trace_id, span_id, flags,
tracestate)`. Completion tuples are exactly `(kind, token, sequence,
failure_reason, failure_source, actor, generation, type_tag, payload,
error_code, detail, schedule_handle, delivery_status, retry_after_ns)`.

Every conversion checks integer overflow, bytes/string bounds, tuple length,
enum range, and native result. On failure, set `TypeError`, `ValueError`,
`RuntimeError`, or `MemoryError` and return `nullptr`; never throw.

- [ ] **Step 5: Tighten architecture containment checks**

Change the Phase 1A native-file scan to exclude only
`bindings/python/native/src/python_capi/`. Add scans proving that `Python.h` and
`PyObject` occur nowhere else in production, and that C API files contain no
`throw`, `catch`, `dynamic_cast`, `typeid`, or `std::function`. Add a compile
definition assertion for `Py_LIMITED_API == 0x030B0000`.

- [ ] **Step 6: Run module and architecture tests to verify GREEN**

```bash
ninja -C build _hpactor test_unit_python_binding
ctest --test-dir build -R 'PythonNativeModule|PythonBindingArchitecture' \
  --output-on-failure
```

Expected: module imports under the configured Python interpreter and all containment checks pass.

- [ ] **Step 7: Commit the limited C extension**

```bash
git add bindings/python/native bindings/python/tests tests/CMakeLists.txt \
  tests/architecture/CMakeLists.txt
git commit -m "feat: add limited API Python extension"
```

### Task 6: Implement Python values, errors, delivery receipts, and protobuf registry

**Files:**
- Create: `bindings/python/hpactor/_address.py`
- Create: `bindings/python/hpactor/_errors.py`
- Create: `bindings/python/hpactor/_delivery.py`
- Create: `bindings/python/hpactor/_messages.py`
- Create: `bindings/python/hpactor/__init__.py`
- Create: `bindings/python/hpactor/py.typed`
- Create: `bindings/python/tests/unit/test_address_delivery.py`
- Create: `bindings/python/tests/unit/test_message_registry.py`

**Interfaces:**
- Consumes: generated protobuf Python message classes and native tuple schemas.
- Produces: immutable addresses/refs/handles, delivery enums/options/results/receipt, typed exceptions, and freezeable `MessageRegistry`.

- [ ] **Step 1: Write failing value and registry tests**

```python
import asyncio
import unittest
from google.protobuf.wrappers_pb2 import StringValue, Int64Value
from hpactor import MessageRegistry, RegistrationError


class MessageRegistryTest(unittest.TestCase):
    def test_fixed_tags_round_trip_deterministically(self) -> None:
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        registry.freeze()
        payload = registry.serialize(StringValue(value="hello"))
        self.assertEqual(registry.type_tag_for(StringValue), 0x1000)
        self.assertEqual(registry.deserialize(0x1000, payload),
                         StringValue(value="hello"))

    def test_rejects_conflicts_ranges_and_post_freeze_registration(self) -> None:
        registry = MessageRegistry()
        registry.register(StringValue, type_tag=0x1000)
        with self.assertRaises(RegistrationError):
            registry.register(Int64Value, type_tag=0x1000)
        with self.assertRaises(RegistrationError):
            registry.register(Int64Value, type_tag=0x0FFF)
        registry.freeze()
        with self.assertRaises(RegistrationError):
            registry.register(Int64Value, type_tag=0x1001)
```

Add async tests proving `DeliveryReceipt` is awaitable, returns the exact
`DeliveryResult`, can be ignored without warnings, and propagates cancellation
only to its Python future while native retry tracking continues.

- [ ] **Step 2: Run Python unit tests to verify RED**

```bash
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_address_delivery.py' -v
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_message_registry.py' -v
```

Expected: imports fail because the package modules do not exist.

- [ ] **Step 3: Implement immutable public values and typed outcomes**

Use frozen, slotted dataclasses for `ActorAddress`, `ActorRef`,
`ScheduleHandle`, `DeliveryOptions`, and `DeliveryResult`. Define exact enums
matching native numeric values for `DeliveryMode`, `DeliveryStatus`,
`FailureReason`, and `FailureSource`. Validate priorities `0..3`, non-negative
timeouts/delays, tag ranges, and address packed lengths in `__post_init__`.

Use these numeric tables: delivery modes `BestEffort=0`,
`ObservableBestEffort=1`, `AtLeastOnce=2`, `DurableAtLeastOnce=3`; delivery
statuses `Accepted=0`, `AcceptedWithPressure=1`, `NoRoute=2`, `ActorDead=3`,
`MailboxFull=4`, `Expired=5`, `Duplicate=6`, `RemoteUnavailable=7`,
`RejectedByPolicy=8`, `SerializationError=9`, `TransportError=10`,
`ShuttingDown=11`, `Cancelled=12`; failure sources `ActorRuntime=0`,
`Mailbox=1`, `Rpc=2`, `Transport=3`, `Discovery=4`, `Scheduler=5`, `Config=6`,
`Security=7`, `DurableStore=8`, `Supervision=9`, `Cluster=10`, `Unknown=11`,
`LanguageBinding=12`. Copy every `FailureReason` numeric value directly from
`include/hpactor/msg/failure_reason.hpp`, including `Unknown=255`, and assert
the table in `test_address_delivery.py`.

`DeliveryOptions` contains `delivery_mode`, `no_drop`, `emit_backpressure`,
`priority`, `deadline_ns`, `message_id`, and `flags`. `DeliveryResult` contains
`status`, `target`, `message_id`, `detail_code`, and `retry_after_ns`, plus
read-only `accepted`, `retryable`, and `failure_reason` properties matching the
C++ conversion table.

`DeliveryReceipt` stores one thread-safe
`concurrent.futures.Future[DeliveryResult]`. `__await__` shields an
`asyncio.wrap_future()` created on the currently running loop, so the same
receipt is safe on the application loop or actor runtime loop and cancellation
of one waiter does not cancel shared native tracking. It exposes `done()` and
`result()`; delivery rejection is a `DeliveryResult`, not a future exception,
so discarding the receipt emits no unobserved-exception warning. Cancelling one
wrapped asyncio waiter does not cancel native retry work or the shared
concurrent future.

Define `HPActorError`, `ActorError`, `RegistrationError`, `SerializationError`,
`ActorNotReadyError`, `ResourceExhaustedError`, `AskTimeoutError`,
`AskCancelledError`, and `SystemClosedError` with stable `.code` and `.detail`
fields where applicable.

- [ ] **Step 4: Implement and freeze the protobuf registry**

For each generated message class, require a descriptor and callable
`SerializeToString`/`FromString`. Store entries by tag, descriptor full name,
and class. Calculate the schema fingerprint as:

```python
hashlib.sha256(
    descriptor.full_name.encode("utf-8")
    + b"\0"
    + descriptor.file.serialized_pb
).digest()
```

Registration is idempotent only for the same tag, full name, class, and
fingerprint. Reject conflicts, tags outside `0x1000..0x00FFFFFF`, and all
registration after `freeze()`. Serialize with
`SerializeToString(deterministic=True)` and translate parse errors to
`SerializationError`.

- [ ] **Step 5: Run registry and value tests to verify GREEN**

```bash
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_address_delivery.py' -v
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_message_registry.py' -v
```

Expected: all tests pass with no third-party test runner dependency.

- [ ] **Step 6: Commit Python values and registry**

```bash
git add bindings/python/hpactor bindings/python/tests/unit
git commit -m "feat: add Python message registry and values"
```

### Task 7: Implement Behavior, Actor, and handler-scoped ActorContext

**Files:**
- Create: `bindings/python/hpactor/_behavior.py`
- Create: `bindings/python/hpactor/_actor.py`
- Create: `bindings/python/hpactor/_context.py`
- Modify: `bindings/python/hpactor/__init__.py`
- Create: `bindings/python/tests/unit/test_behavior_actor.py`
- Create: `bindings/python/tests/unit/test_actor_context.py`

**Interfaces:**
- Consumes: `MessageRegistry`, address/ref values, delivery values, and a private runtime command protocol.
- Produces: `Behavior.on`, `Behavior.on_request`, `Actor`, `actor(name)`, `Actor.become`, lifecycle hooks, and the Phase 1B context surface.

- [ ] **Step 1: Write failing behavior and context tests**

```python
class Echo(Actor):
    def behavior(self) -> Behavior:
        return Behavior().on_request(StringValue, StringValue, self.echo)

    async def echo(self, message: StringValue, ctx: ActorContext) -> StringValue:
        return StringValue(value=message.value)


class BehaviorActorTest(unittest.IsolatedAsyncioTestCase):
    async def test_behavior_dispatch_and_become(self) -> None:
        actor = Echo()
        initial = actor.behavior()
        entry = initial.handler_for(StringValue)
        self.assertEqual(entry.response_type, StringValue)
        replacement = Behavior().on(StringValue, actor.echo)
        actor.become(replacement)
        self.assertIs(actor.current_behavior, replacement)

    async def test_context_expires_after_handler_turn(self) -> None:
        runtime = FakeRuntime()
        ctx = ActorContext._create(runtime, runtime.self_ref, runtime.sender,
                                   ask_message_id=3, turn_id=9)
        ctx._expire()
        with self.assertRaises(ActorNotReadyError):
            ctx.send(runtime.self_ref, StringValue(value="late"))
```

- [ ] **Step 2: Run behavior/context tests to verify RED**

```bash
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_behavior_actor.py' -v
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_actor_context.py' -v
```

Expected: imports fail because actor API modules do not exist.

- [ ] **Step 3: Implement validated immutable behaviors**

`Behavior.on(message_type, handler)` and
`on_request(request_type, response_type, handler)` return `self` while building;
`freeze(registry)` validates registered protobuf types, unique request types,
async handlers, and response registrations, then stores a read-only mapping by
type tag. Mutation after freeze raises `ActorNotReadyError`. `handler_for()`
returns a frozen `_HandlerEntry` containing request class, optional response
class, and bound async callable.

- [ ] **Step 4: Implement actor lifecycle and decorator metadata**

`@actor("echo")` validates a non-empty ASCII name up to 255 bytes and stores
`__hpactor_actor_name__` on an `Actor` subclass. `Actor` defines async no-op
`on_start()`/`on_stop()`, abstract `behavior()`, a current frozen behavior, and
`become()` that delegates validation to its bound runtime before swapping.
Binding occurs once; rebinding or calling `become()` outside the owning runtime
loop raises `ActorNotReadyError`.

- [ ] **Step 5: Implement the handler-scoped context surface**

Store runtime protocol, self ref, sender ref, ask ID, and a monotonically
allocated turn ID. Every method calls `_require_live_turn()`. Implement exact
signatures from the approved design: sync `send`, `reply`, `reply_error`,
`passivate`, and `become`; async `ask`, `spawn`, `schedule`, `cancel_schedule`,
`link`, `unlink`, `monitor`, `demonitor`, and `stop`. Each method serializes via
the registry and delegates one value-only command to the runtime protocol.
`_expire()` runs in a `finally` block after handler completion and invalidates
all later context use.

- [ ] **Step 6: Run actor API unit tests to verify GREEN**

```bash
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_behavior_actor.py' -v
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_actor_context.py' -v
```

Expected: all behavior, replacement, validation, and invalid-lifetime tests pass.

- [ ] **Step 7: Commit actor-facing Python types**

```bash
git add bindings/python/hpactor bindings/python/tests/unit
git commit -m "feat: add Python actor behavior API"
```

### Task 8: Implement the bounded runtime loop, dispatch coordinator, and actor runner

**Files:**
- Create: `bindings/python/hpactor/_runtime.py`
- Modify: `bindings/python/hpactor/_actor.py`
- Modify: `bindings/python/hpactor/_context.py`
- Modify: `bindings/python/hpactor/_delivery.py`
- Create: `bindings/python/tests/unit/fake_native.py`
- Create: `bindings/python/tests/unit/test_support.py`
- Create: `bindings/python/tests/unit/test_runtime_dispatch.py`
- Create: `bindings/python/tests/unit/test_runtime_completions.py`

**Interfaces:**
- Consumes: low-level native protocol, registry, actors, behaviors, contexts, commands, dispatches, and completions.
- Produces: `_RuntimeThread`, `_ActorRuntime`, bounded `_DispatchCoordinator`, `_ActorRunner`, and bounded token-to-future resolution.

- [ ] **Step 1: Write failing serialization and non-reentrancy tests**

Create `FakeNativeSystem` with explicit dispatch/completion deques and manual
`fire_dispatch_reader()`/`fire_completion_reader()` hooks. Test two actors where
actor A waits on an event and actor B completes while A remains suspended; a
second A message must not start until the first event is released:

```python
class RuntimeDispatchTest(unittest.IsolatedAsyncioTestCase):
    async def test_one_turn_per_actor_extends_across_await(self) -> None:
        native = FakeNativeSystem(dispatch_capacity=4)
        runtime = await make_started_runtime(native)
        gate = asyncio.Event()
        observed: list[str] = []
        a = await runtime.install_test_actor(WaitingActor(gate, observed))
        b = await runtime.install_test_actor(RecordingActor(observed))

        native.push_dispatch(a, 1, StringValue(value="a1"))
        native.push_dispatch(a, 2, StringValue(value="a2"))
        native.push_dispatch(b, 1, StringValue(value="b1"))
        native.fire_dispatch_reader()
        await runtime.wait_until_idle_actor(b)
        self.assertEqual(observed, ["a1:start", "b1"])
        gate.set()
        await runtime.wait_until_idle()
        self.assertEqual(observed, ["a1:start", "b1", "a1:end", "a2:start", "a2:end"])
```

Add tests that native drain budget equals available Python deque slots, queue
depth never exceeds capacity, per-actor sequence regression fails that actor,
ask completion resolves a suspended handler directly, and stale-generation
completion is discarded.

`fake_native.py` implements every low-level method listed in Task 5, records
submitted command tuples, enforces configured dispatch/completion capacities,
and invokes registered reader callbacks synchronously on the test loop.
`test_support.py` defines `registry()` with `StringValue=0x1000` and
`Int64Value=0x1001`, `make_started_runtime()`, `WaitingActor`,
`RecordingActor`, and `FailsOnStart`; all observations use asyncio events or
lists owned by the test loop.

- [ ] **Step 2: Run runtime unit tests to verify RED**

```bash
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_runtime_*.py' -v
```

Expected: imports fail because `_runtime.py` and the fake protocol do not exist.

- [ ] **Step 3: Implement the dedicated loop thread and marshalling boundary**

`_RuntimeThread.start()` creates a thread, creates and sets one event loop in
that thread, constructs `_ActorRuntime` there, and signals a `threading.Event`
only after notifier readers are registered. `submit(coro)` uses
`asyncio.run_coroutine_threadsafe` from application threads and directly awaits
when already on the runtime loop. `stop()` is requested from a non-runtime
thread, removes readers, drains/cancels tasks, stops the loop, joins the thread,
and clears references.

- [ ] **Step 4: Implement bounded dispatch and serialized actor runners**

Maintain one global `collections.deque` with maximum
`dispatch_queue_capacity`, an `active_actor_keys` set, and actor runners keyed
by `(actor_id, generation)`. The reader computes free slots and calls native
`drain_dispatch(min(max_dispatch_per_tick, free_slots))`. It appends in native
FIFO order, then scans the bounded deque once, scheduling the first envelope
for each inactive actor while retaining same-actor order. Completion of a
runner turn clears the active key and reschedules the coordinator with
`loop.call_soon`.

`_ActorRunner` checks generation, sequence monotonicity, deadline before start,
registry tag, and handler presence. It creates a live `ActorContext`, awaits
exactly one handler, validates/serializes request responses, sends reply or
application `ActorError`, converts any other exception to a bounded actor-failed
command, and expires context in `finally`. The actor remains active across all
awaits.

- [ ] **Step 5: Implement bounded completion tokens**

Use one `_TokenRegistry` bounded by `completion_queue_capacity`. Tokens start at
1, never reuse a live value, and map to a thread-safe
`concurrent.futures.Future`, expected completion kind,
expected actor generation, and optional response type. The completion reader
drains at most `max_dispatch_per_tick`, validates token/kind/generation, maps
native failures to typed exceptions, deserializes ask payloads, and resolves
each future exactly once. Cancellation submits `CancelAsk` for ask tokens and
removes only after native cancellation completion.

`ActorStopped` and `ActorFailed` completions bypass the token table and route by
actor ID/generation. The runner's idempotent `stop_once()` serializes
`on_stop()` after any active turn, removes the actor from the coordinator, and
discards later stale events. `ActorFailed` first records the bounded exception
metadata for the system-level failure future, then follows the same stop path.

- [ ] **Step 6: Run runtime unit tests to verify GREEN**

```bash
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_runtime_*.py' -v
```

Expected: all boundedness, ordering, non-reentrancy, direct ask completion, and stale-generation tests pass without sleeps.

- [ ] **Step 7: Commit the Python runtime loop**

```bash
git add bindings/python/hpactor bindings/python/tests/unit
git commit -m "feat: add serialized Python actor runtime"
```

### Task 9: Add the public ActorSystem context manager and imperative spawn

**Files:**
- Create: `bindings/python/hpactor/_system.py`
- Modify: `bindings/python/hpactor/_runtime.py`
- Modify: `bindings/python/hpactor/__init__.py`
- Create: `bindings/python/tests/unit/test_actor_system.py`
- Create: `bindings/python/tests/unit/test_public_api.py`

**Interfaces:**
- Consumes: `_hpactor.NativeSystem`, `_RuntimeThread`, registry, actor classes, and native spawn/stop/name methods.
- Produces: `ActorSystem`, `async with` lifecycle, `spawn`, `send`, and `ask` from application loops.

- [ ] **Step 1: Write failing public lifecycle and rollback tests**

```python
@actor("echo")
class Echo(Actor):
    started = 0
    stopped = 0

    async def on_start(self) -> None:
        type(self).started += 1

    async def on_stop(self) -> None:
        type(self).stopped += 1

    def behavior(self) -> Behavior:
        return Behavior().on_request(StringValue, StringValue, self.echo)

    async def echo(self, msg: StringValue, ctx: ActorContext) -> StringValue:
        return msg


class ActorSystemTest(unittest.IsolatedAsyncioTestCase):
    async def test_context_manager_spawns_and_stops_actor(self) -> None:
        native = FakeNativeSystem()
        async with ActorSystem(messages=registry(), _native=native) as system:
            ref = await system.spawn(Echo, name="echo")
            self.assertEqual(ref.name, "echo")
            self.assertEqual(Echo.started, 1)
        self.assertEqual(Echo.stopped, 1)
        self.assertEqual(native.stop_calls, 1)

    async def test_failed_start_rolls_back_bridge_and_name(self) -> None:
        native = FakeNativeSystem()
        async with ActorSystem(messages=registry(), _native=native) as system:
            with self.assertRaises(RuntimeError):
                await system.spawn(FailsOnStart, name="bad")
            self.assertEqual(native.stopped_bridge_count, 1)
            self.assertIsNone(native.resolve_name("bad"))
```

- [ ] **Step 2: Run public system tests to verify RED**

```bash
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_actor_system.py' -v
```

Expected: import fails because `_system.py` does not exist.

- [ ] **Step 3: Implement explicit system lifecycle and loop marshalling**

`ActorSystem` accepts `MessageRegistry`, optional validated Python config, and
an internal native factory for tests. `__aenter__` freezes the registry,
constructs native state, starts the dedicated loop, and returns only after the
native system and notifier readers are running. A second enter or use after
exit raises `SystemClosedError`.

Keep `_hpactor` import lazy inside the default native factory so registry,
behavior, and typing modules remain importable for source-only unit tests.

Top-level `spawn()` marshals to the runtime loop and calls native
`spawn_bridge()`. Context child spawn submits a `Spawn` command from the current
parent bridge. Both paths receive address/generation, instantiate the Python
class with exact `*args/**kwargs`, bind and install its runner, await `on_start`,
then register the name and return `ActorRef`.
On construction, behavior, or start failure, stop the unnamed bridge, remove
the runner, run `on_stop` only if start completed, and re-raise.

`__aexit__` is idempotent: reject new calls, invoke each installed runner's
`stop_once()` in reverse spawn order, stop bridges, stop the native system,
remove readers, stop/join the runtime loop, and resolve outstanding futures as
`SystemClosedError`.

- [ ] **Step 4: Expose top-level send and ask through the application bridge**

`ActorSystem.send()` returns `DeliveryReceipt`; `ActorSystem.ask()` allocates a
token, submits an `Ask` command with the hidden application bridge as origin,
awaits the typed result, and submits `CancelAsk` when its Python task is
cancelled. Both methods marshal to the runtime loop and use deterministic
registry serialization.

For synchronous `send()`, allocate the concurrent completion promise in the
calling thread, post command/token registration with
`loop.call_soon_threadsafe`, and return the receipt immediately; never call
`Future.result()` on an application event-loop thread. Actor-context sends call
the same registration function directly because they already run on the actor
loop.

Set `hpactor.__all__` exactly to `ActorSystem`, `Actor`, `ActorContext`,
`ActorAddress`, `ActorRef`, `ScheduleHandle`, `Behavior`, `MessageRegistry`,
`DeliveryMode`, `DeliveryStatus`, `DeliveryOptions`, `DeliveryResult`,
`DeliveryReceipt`, `FailureReason`, `FailureSource`, `actor`, `HPActorError`,
`ActorError`, `RegistrationError`, `SerializationError`,
`ActorNotReadyError`, `ResourceExhaustedError`, `AskTimeoutError`,
`AskCancelledError`, and `SystemClosedError`.

- [ ] **Step 5: Run system and public export tests to verify GREEN**

```bash
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_actor_system.py' -v
PYTHONPATH=bindings/python python3 -m unittest discover \
  -s bindings/python/tests/unit -p 'test_public_api.py' -v
```

Expected: lifecycle, rollback, cross-loop marshalling, invalid-state, and public export tests pass.

- [ ] **Step 6: Commit the public actor system**

```bash
git add bindings/python/hpactor bindings/python/tests/unit
git commit -m "feat: add Python ActorSystem lifecycle"
```

### Task 10: Complete messaging, scheduling, lifecycle, and real native integration

**Files:**
- Create: `bindings/python/tests/integration/test_echo_workflow.py`
- Create: `bindings/python/tests/integration/test_actor_ordering.py`
- Create: `bindings/python/tests/integration/test_context_operations.py`
- Create: `bindings/python/tests/integration/test_lifecycle.py`
- Modify: `bindings/python/tests/CMakeLists.txt`
- Modify: `tests/architecture/CMakeLists.txt`
- Modify: `CLAUDE_MEMORY.md`
- Modify: `docs/superpowers/specs/2026-07-03-python-language-binding-design.md`

**Interfaces:**
- Consumes: the complete Phase 1B native and Python surfaces.
- Produces: deterministic end-to-end evidence for public Phase 1B behavior and honest project status.

- [ ] **Step 1: Add the real local echo and ask workflow**

```python
@actor("echo")
class Echo(Actor):
    def behavior(self) -> Behavior:
        return (Behavior()
                .on(Int64Value, self.on_fire)
                .on_request(StringValue, StringValue, self.on_ask))

    async def on_fire(self, msg: Int64Value, ctx: ActorContext) -> None:
        self.last_fire = msg.value

    async def on_ask(self, msg: StringValue,
                     ctx: ActorContext) -> StringValue:
        return StringValue(value=msg.value)


class EchoWorkflowTest(unittest.IsolatedAsyncioTestCase):
    async def test_spawn_send_and_ask(self) -> None:
        messages = MessageRegistry()
        messages.register(StringValue, type_tag=0x1000)
        messages.register(Int64Value, type_tag=0x1001)
        async with ActorSystem(messages=messages) as system:
            echo = await system.spawn(Echo, name="echo")
            receipt = system.send(echo, Int64Value(value=7))
            result = await asyncio.wait_for(receipt, timeout=5.0)
            self.assertTrue(result.accepted)
            reply = await asyncio.wait_for(
                system.ask(echo, StringValue(value="ask"),
                           response_type=StringValue, timeout=5.0),
                timeout=6.0,
            )
            self.assertEqual(reply.value, "ask")
```

Use events and `asyncio.wait_for` only as a deadlock guard. Add Python-to-Python
tests proving deterministic serialization and same-actor FIFO ordering across
an awaited gate while a second actor progresses.

- [ ] **Step 2: Add real context-operation tests**

Exercise reply error versus unhandled exception, ask timeout/cancellation,
child spawn, schedule/cancel, link/unlink, monitor/demonitor, stop, passivate,
and `become()`. Assert returned schedule handles, delivery results, and typed
exceptions. Use actors that report observations through protobuf messages or
test futures; do not inspect C++ actor state from Python.

- [ ] **Step 3: Add lifecycle and cleanup tests**

Prove `on_start` before first handler, `on_stop` exactly once, failed start
rollback, async-context cleanup, no use after close, no stale-generation future
resolution, and no Python object retained by the native module after system
exit. Use `weakref.ref` and `gc.collect()` only after deterministic shutdown.

- [ ] **Step 4: Register and run focused Phase 1B tests**

```bash
ninja -C build _hpactor test_unit_python_binding
ctest --test-dir build -R 'PythonBinding(Unit|Integration|Architecture)' \
  --output-on-failure
```

Expected: every Phase 1B C++ and Python test passes with no timeout or skipped supported-platform case.

- [ ] **Step 5: Update status without claiming packaging or operations work**

Add a dated `Python Binding Phase 1B Actor API` entry to `CLAUDE_MEMORY.md`
listing the local limited-API module, protobuf registry, actor/behavior/context
API, dedicated loop, messaging, lifecycle, and exact test evidence. Include:

```text
The Phase 1B API is a build-tree development surface. ABI3 wheel production,
repair, installation docs, and supported distribution begin in Phase 1D; the
developer manual's “no official bindings” limitation remains accurate.
```

Change the design status to:

```markdown
**Status:** Approved design; Phases 1A and 1B implemented
```

- [ ] **Step 6: Run full build and test verification**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF -DENABLE_PYTHON_BINDINGS=ON
ninja -C build
ctest --test-dir build --output-on-failure --parallel 8
git diff --check
```

Expected: full build succeeds, all configured C++/Python/architecture tests pass, and `git diff --check` prints no output.

- [ ] **Step 7: Run debug-Python reference and Linux sanitizer evidence**

With a CPython debug interpreter, run:

```bash
PYTHONMALLOC=debug PYTHONASYNCIODEBUG=1 \
  ctest --test-dir build -R 'PythonBinding' --output-on-failure
```

On supported Linux CI, run:

```bash
cmake -S . -B build-tsan -GNinja \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
  -DENABLE_PYTHON_BINDINGS=ON -DENABLE_TSAN=ON
ninja -C build-tsan _hpactor test_unit_python_binding
ctest --test-dir build-tsan -R 'PythonBinding' --output-on-failure
```

Expected: debug allocator/asyncio reports no leaked references or late callbacks, and TSAN reports no native race.

- [ ] **Step 8: Commit Phase 1B acceptance evidence**

```bash
git add bindings/python/tests tests/architecture/CMakeLists.txt \
  CLAUDE_MEMORY.md \
  docs/superpowers/specs/2026-07-03-python-language-binding-design.md
git commit -m "test: verify Python actor API"
```

## Plan Completion Checklist

- [ ] Phase 1A is implemented and passing before Phase 1B begins.
- [ ] Pending native asks and delivery receipts notify through fixed value-only ports without blocking scheduler workers.
- [ ] Protected tags `0xF0`–`0xF3` are rejected from ordinary and batch remote ingress.
- [ ] Every command requiring actor context executes on the originating bridge actor turn.
- [ ] No native bridge queue, callback, or facade stores a Python object.
- [ ] `Python.h` and `PyObject` are confined to `bindings/python/native/src/python_capi/`.
- [ ] `_hpactor` compiles with `Py_LIMITED_API=0x030B0000`, no exceptions, and no RTTI.
- [ ] Message registration is pre-start only, fixed-tag, fingerprinted, deterministic, and conflict checked.
- [ ] The dedicated asyncio thread exclusively owns Python actors, handlers, and futures.
- [ ] Total Python pending dispatch storage is bounded and native drains honor available slots.
- [ ] A Python actor never executes two handler/lifecycle turns concurrently, including across `await`.
- [ ] Ask completion resolves the suspended handler by token without requiring a response envelope turn.
- [ ] Send/reply/ask, receipts, child spawn, schedule/cancel, link/monitor, stop, passivate, and `become()` have deterministic tests.
- [ ] `ActorError` replies without actor failure; unhandled exceptions become bounded value-only failure commands.
- [ ] Start failure rolls back the unnamed bridge; stop runs `on_stop()` exactly once and rejects later use.
- [ ] Manual documentation still makes no packaged-binding claim before Phase 1D.
- [ ] Full C++/Python/architecture verification and supported Linux TSAN evidence pass.
