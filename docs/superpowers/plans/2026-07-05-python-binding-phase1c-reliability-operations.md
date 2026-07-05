# Python Binding Phase 1C Reliability and Operations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (- [ ]) syntax for tracking.

**Goal:** Make the Phase 1B Python actor API production-operable by adding native supervision and restart, link/monitor events, bounded overflow and DLQ accounting, metrics, structured logs, tracing, CLI inspection, health/readiness, deterministic stress coverage, and shutdown hardening.

**Architecture:** Phase 1C extends the value-only Phase 1A bridge and the Phase 1B actor API; it does not introduce a second runtime or let HPActor scheduler threads call Python. Reliability transitions remain actor-owned, while fixed function-pointer ports connect the bridge to bounded observability, DLQ, inspection, health, and shutdown adapters. The dedicated asyncio thread reconstructs Python actor objects during restart and services bounded inspection requests, with generation fencing on every result.

**Tech Stack:** C++20, HPActor lifecycle/supervision/DLQ/metrics/logging/tracing/CLI/health APIs, CPython 3.11 limited API, asyncio, generated Protocol Buffers, CMake/Ninja, GoogleTest, Python unittest, Linux eventfd, and macOS non-blocking socket pairs.

## Global Constraints

- Phase 1A and Phase 1B must be implemented and passing before Phase 1C begins.
- HPActor scheduler, network, telemetry, CLI, and health threads never call CPython or wait for the GIL.
- Python actors, handlers, lifecycle hooks, inspection hooks, and Python object destruction run only on the dedicated asyncio thread.
- Cross-thread bridge records are owned values with no PyObject pointers, borrowed Python buffers, stack pointers, or unbounded containers.
- Dispatch, command, completion, inspection, telemetry, and pending-operation storage stay within configured bounds.
- Existing FailureReason, FailureSource, DeadLetterReason, protocol enum, and TypeTag numeric values are not renumbered.
- FailureSource::LanguageBinding remains numeric value 12.
- Local-only Python control tags remain exactly 0xF0 through 0xF3 and remain rejected from remote ordinary and batch ingress.
- Error messages and exception type names are capped at 1 KiB; formatted tracebacks and inspection payloads are capped at 16 KiB.
- Actor ID is never a default metric label. Optional actor-type labels are bounded to registered Python actor types.
- Readiness becomes false for startup failure, non-Running runtime state, heartbeat lag above loop_lag_unready_ms, or shutdown; queue pressure alone does not make the node unready.
- Restart never restores arbitrary Python object memory. A replacement instance is created from the frozen class factory and immutable spawn arguments.
- Tests use paused workers, explicit scheduler steps, notifier drains, events, and condition-based waits. Sleeps are deadlock guards only.
- No dynamic_cast, typeid, exceptions, exception translation, public toml++ headers, or unbounded bridge queues.
- The developer manual continues to state that no official packaged Python binding exists until Phase 1D.
- Because this phase changes lifecycle, config, generated protobuf, observability, CLI, health, and shutdown integration, finish with a full configured build/test run plus supported Linux TSAN evidence.

## File Structure

### Configuration and value contracts

- Create: include/hpactor/config/python_binding_config.hpp
- Modify: include/hpactor/config/topology_model.hpp
- Create: src/config/parsers/python_binding_config_parser.cpp
- Modify: bindings/python/native/include/hpactor/python/python_bridge_types.hpp
- Modify: bindings/python/native/include/hpactor/python/python_runtime_snapshot.hpp
- Modify: bindings/python/native/include/hpactor/python/python_runtime.hpp
- Modify: bindings/python/native/src/python_runtime.cpp
- Modify: protos/hpactor/python_binding_internal.proto
- Modify: bindings/python/native/src/python_command_codec.cpp
- Modify: bindings/python/native/src/python_capi/conversions.cpp

### Reliability and lifecycle

- Create: bindings/python/native/include/hpactor/python/python_reliability.hpp
- Create: bindings/python/native/src/python_reliability.cpp
- Modify: bindings/python/native/include/hpactor/python/python_ports.hpp
- Modify: bindings/python/native/include/hpactor/python/python_bridge_actor.hpp
- Modify: bindings/python/native/src/python_bridge_actor.cpp
- Modify: bindings/python/native/include/hpactor/python/python_command_router.hpp
- Modify: bindings/python/native/src/python_command_router.cpp
- Modify: bindings/python/native/include/hpactor/python/python_native_system.hpp
- Modify: bindings/python/native/src/python_native_system.cpp
- Modify: bindings/python/hpactor/_actor.py
- Modify: bindings/python/hpactor/_runtime.py
- Modify: bindings/python/hpactor/_errors.py
- Modify: bindings/python/hpactor/__init__.py

### Observability, inspection, health, and shutdown

- Create: bindings/python/native/include/hpactor/python/python_observability.hpp
- Create: bindings/python/native/src/python_observability.cpp
- Create: bindings/python/native/include/hpactor/python/python_inspection.hpp
- Create: bindings/python/native/src/python_inspection.cpp
- Create: bindings/python/native/include/hpactor/python/python_health_check.hpp
- Create: bindings/python/native/src/python_health_check.cpp
- Create: bindings/python/native/include/hpactor/python/python_shutdown_adapter.hpp
- Create: bindings/python/native/src/python_shutdown_adapter.cpp
- Create: bindings/python/native/src/python_cli_commands.cpp
- Modify: bindings/python/native/src/python_capi/native_system_type.cpp
- Modify: bindings/python/hpactor/_runtime.py
- Modify: bindings/python/hpactor/_system.py

### Tests and status

- Modify: tests/unit/python/CMakeLists.txt
- Create: tests/unit/python/test_python_binding_config.cpp
- Create: tests/unit/python/test_python_reliability.cpp
- Create: tests/unit/python/test_python_observability.cpp
- Create: tests/unit/python/test_python_inspection.cpp
- Create: tests/unit/python/test_python_health_shutdown.cpp
- Create: bindings/python/tests/unit/test_reliability.py
- Create: bindings/python/tests/unit/test_inspection.py
- Create: bindings/python/tests/integration/test_supervision.py
- Create: bindings/python/tests/integration/test_link_monitor.py
- Create: bindings/python/tests/integration/test_overflow_dlq.py
- Create: bindings/python/tests/integration/test_observability.py
- Create: bindings/python/tests/integration/test_cli_health.py
- Create: bindings/python/tests/integration/test_shutdown_under_load.py
- Create: tests/integration/python/test_python_runtime_stress.cpp
- Modify: bindings/python/tests/CMakeLists.txt
- Modify: tests/integration/python/CMakeLists.txt
- Modify: tests/architecture/CMakeLists.txt
- Modify: CLAUDE_MEMORY.md
- Modify: docs/superpowers/specs/2026-07-03-python-language-binding-design.md

---

### Task 1: Add validated Phase 1C configuration and bounded lifecycle contracts

**Files:**
- Create: include/hpactor/config/python_binding_config.hpp
- Modify: include/hpactor/config/topology_model.hpp
- Create: src/config/parsers/python_binding_config_parser.cpp
- Modify: bindings/python/native/include/hpactor/python/python_bridge_types.hpp
- Modify: bindings/python/native/include/hpactor/python/python_runtime_snapshot.hpp
- Modify: bindings/python/native/include/hpactor/python/python_runtime.hpp
- Modify: bindings/python/native/src/python_runtime.cpp
- Modify: protos/hpactor/python_binding_internal.proto
- Modify: bindings/python/native/src/python_command_codec.cpp
- Create: tests/unit/python/test_python_binding_config.cpp
- Modify: tests/unit/python/test_python_contracts.cpp
- Modify: CMakeLists.txt
- Modify: bindings/python/native/CMakeLists.txt

**Interfaces:**
- Consumes: Phase 1A PythonRuntimeConfig and snapshots, Phase 1B internal protobuf controls, SystemDef, TomlTableView, and self-registering TOML parsers.
- Produces: config::PythonBindingConfig, PythonDispatchKind, bounded PythonFailureMetadata, PythonActorSnapshot, inspect/restart completion kinds, heartbeat accounting, and append-only generated values.

- [ ] **Step 1: Write failing parser and append-only contract tests**

Add tests that parse the exact approved system.python table and reject every boundary violation:

~~~cpp
TEST(PythonBindingConfigTest, ParsesApprovedValues) {
    auto parsed = parse_system(R"(
[system]
scheduler_threads = 0
[system.python]
enabled = true
dispatch_queue_capacity = 65536
command_queue_capacity = 16384
completion_queue_capacity = 16384
max_actor_bindings = 65536
max_dispatch_per_tick = 256
max_commands_per_turn = 256
loop_lag_unready_ms = 5000
handler_shutdown_timeout_ms = 10000
trace_handler_spans = true
)");
    ASSERT_TRUE(parsed.ok());
    const auto& cfg = parsed.value().system.python;
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.dispatch_queue_capacity, 65536u);
    EXPECT_EQ(cfg.loop_lag_unready_ms, 5000u);
    EXPECT_TRUE(cfg.trace_handler_spans);
}

TEST(PythonBindingConfigTest, RejectsNonPowerOfTwoCapacity) {
    EXPECT_FALSE(parse_python_config("dispatch_queue_capacity = 65").ok());
}

TEST(PythonBindingConfigTest, RejectsBudgetAboveCapacity) {
    EXPECT_FALSE(parse_python_config(
        "dispatch_queue_capacity = 64\nmax_dispatch_per_tick = 65").ok());
}
~~~

Extend contract tests to assert the existing command/completion numeric table and these appended values:

~~~cpp
EXPECT_EQ(static_cast<uint8_t>(python::PythonDispatchKind::Message), 0u);
EXPECT_EQ(static_cast<uint8_t>(python::PythonDispatchKind::LinkedExit), 1u);
EXPECT_EQ(static_cast<uint8_t>(python::PythonDispatchKind::MonitorDown), 2u);
EXPECT_EQ(static_cast<uint8_t>(python::PythonDispatchKind::Restart), 3u);
EXPECT_EQ(static_cast<uint8_t>(python::PythonCompletionKind::InspectResult), 8u);
EXPECT_EQ(static_cast<uint8_t>(python::PythonCompletionKind::RestartReady), 9u);
~~~

- [ ] **Step 2: Configure and build the focused tests to verify RED**

~~~bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF -DENABLE_PYTHON_BINDINGS=ON
ninja -C build test_unit_python_binding test_unit_config
~~~

Expected: compilation fails because PythonBindingConfig, the parser, and Phase 1C contract values do not exist.

- [ ] **Step 3: Add the core config value and subsystem-owned parser**

Define this dependency-free value in config/python_binding_config.hpp:

~~~cpp
struct PythonBindingConfig final {
    bool enabled{false};
    uint32_t dispatch_queue_capacity{65536};
    uint32_t command_queue_capacity{16384};
    uint32_t completion_queue_capacity{16384};
    uint32_t max_actor_bindings{65536};
    uint32_t max_dispatch_per_tick{256};
    uint32_t max_commands_per_turn{256};
    uint32_t loop_lag_unready_ms{5000};
    uint32_t handler_shutdown_timeout_ms{10000};
    bool trace_handler_spans{true};

    [[nodiscard]] result<void> validate() const noexcept;
};
~~~

Add PythonBindingConfig python{} to SystemDef. Implement validate() with powers-of-two capacities in 64..1048576, budgets in 1..4096 and not above their queue, actor bindings in 1..1048576, loop lag in 100..60000, and shutdown timeout in 100..300000.

Implement PythonBindingConfigParser as ITomlSystemConfigParser with kName = "system.python" and kOrder = 105. It reads only system.table("python"), populates out.python, calls validate(), and returns errors::invalid_argument with the exact invalid key in the detail. Register it with one file-scope TomlSystemParserRegistration.

- [ ] **Step 4: Extend the generated and native value-only contracts**

Append these native values without changing Phase 1A/1B numbers:

~~~cpp
enum class PythonDispatchKind : uint8_t {
    Message = 0,
    LinkedExit = 1,
    MonitorDown = 2,
    Restart = 3,
};

struct PythonFailureMetadata final {
    FailureReason reason{FailureReason::Unknown};
    FailureSource source{FailureSource::LanguageBinding};
    uint32_t error_code{0};
    std::string exception_type;
    std::string detail;
    std::string traceback;
};

struct PythonActorSnapshot final {
    ActorAddress actor{};
    uint64_t generation{0};
    uint64_t last_sequence{0};
    uint64_t handled{0};
    uint64_t failures{0};
    uint64_t restarts{0};
    uint64_t cancellations{0};
    uint32_t pending_turns{0};
    bool active_turn{false};
    bool quarantined{false};
    std::string actor_type;
};
~~~

Add PythonDispatchKind kind to PythonDispatchEnvelope and a bounded failure field used only for linked-exit, monitor-down, and restart records. Append InspectResult = 8 and RestartReady = 9 to PythonCompletionKind. Extend PythonRuntimeSnapshot with dispatch_rejected, command_rejected, handler_exceptions, handler_cancelled, stale_completions, last_heartbeat_ns, loop_lag_ns, and readiness.

Add version-1 PbPythonInspectRequest and PbPythonInspectResult messages. The result contains actor address, generation, actor type, lifecycle state, counters, and bytes detail_json. Reject detail_json above 16 KiB, actor type above 255 bytes, failure strings above their global bounds, unknown enum values, and all non-version-1 payloads.

- [ ] **Step 5: Implement heartbeat and monotonic counter updates**

Add these noexcept methods to PythonRuntime:

~~~cpp
void record_heartbeat(uint64_t now_ns) noexcept;
void record_dispatch_rejected() noexcept;
void record_command_rejected() noexcept;
void record_handler_exception() noexcept;
void record_handler_cancelled() noexcept;
void record_stale_completion() noexcept;
~~~

Use relaxed atomics. snapshot() computes loop_lag_ns from a monotonic clock and sets readiness only when state is Running and lag is within config.loop_lag_unready_ms. Never use wall-clock time for lag or deadlines.

- [ ] **Step 6: Run focused config and contract tests to verify GREEN**

~~~bash
ninja -C build test_unit_python_binding test_unit_config
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonBindingConfigTest.*:PythonContractsTest.*:PythonCommandProtoTest.*'
ctest --test-dir build -R 'PythonBindingConfig|PythonBindingContracts' \
  --output-on-failure
~~~

Expected: all parser bounds, append-only numeric values, codec limits, and heartbeat snapshot tests pass.

- [ ] **Step 7: Commit the Phase 1C contracts**

~~~bash
git add include/hpactor/config/python_binding_config.hpp \
  include/hpactor/config/topology_model.hpp \
  src/config/parsers/python_binding_config_parser.cpp \
  bindings/python/native protos/hpactor/python_binding_internal.proto \
  tests/unit/python CMakeLists.txt
git commit -m "feat: add Python reliability contracts"
~~~

### Task 2: Drive Python failures through lifecycle supervision and generational restart

**Files:**
- Create: bindings/python/native/include/hpactor/python/python_reliability.hpp
- Create: bindings/python/native/src/python_reliability.cpp
- Modify: bindings/python/native/include/hpactor/python/python_ports.hpp
- Modify: bindings/python/native/include/hpactor/python/python_bridge_actor.hpp
- Modify: bindings/python/native/src/python_bridge_actor.cpp
- Modify: bindings/python/native/include/hpactor/python/python_native_system.hpp
- Modify: bindings/python/native/src/python_native_system.cpp
- Modify: bindings/python/hpactor/_actor.py
- Modify: bindings/python/hpactor/_runtime.py
- Create: tests/unit/python/test_python_reliability.cpp
- Create: bindings/python/tests/unit/test_reliability.py

**Interfaces:**
- Consumes: PbPythonActorFailed, LifecycleActor, SupervisionPolicy, PythonActorLease, FailureEnvelope, generation fencing, and Phase 1B actor factories.
- Produces: PythonReliabilityController, explicit restart/stop/quarantine decisions, bridge lifecycle hooks, replacement generations, Python SupervisionPolicy, and deterministic actor reconstruction.

- [ ] **Step 1: Write failing native decision and generation tests**

~~~cpp
TEST(PythonReliabilityTest, RestartAllocatesReplacementGeneration) {
    ReliabilityFixture fixture;
    auto actor = fixture.spawn_python_bridge();
    const auto old_generation = actor.generation;

    fixture.fail(actor, "ValueError", "boom", "trace");
    fixture.step_until_reliability_decision();

    auto snapshot = fixture.native.snapshot_actor(actor.address);
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_GT(snapshot->generation, old_generation);
    EXPECT_EQ(snapshot->restarts, 1u);
    EXPECT_EQ(fixture.restart_dispatches(), 1u);
}

TEST(PythonReliabilityTest, QuarantineStopsRestartAfterBudget) {
    ReliabilityFixture fixture({.max_restarts = 1,
                                .quarantine_on_exhaustion = true});
    auto actor = fixture.spawn_python_bridge();
    fixture.fail_and_restart(actor);
    fixture.fail(actor, "RuntimeError", "again", "trace");
    fixture.step_until_reliability_decision();
    EXPECT_TRUE(fixture.native.snapshot_actor(actor.address)->quarantined);
}
~~~

Add stale-generation tests proving that RestartReady for the old generation and completions from the failed actor are discarded and counted.

- [ ] **Step 2: Run the reliability tests to verify RED**

~~~bash
ninja -C build test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonReliabilityTest.*'
~~~

Expected: compilation fails because the controller, bridge lifecycle integration, and replacement-generation API do not exist.

- [ ] **Step 3: Define fixed reliability ports and policy values**

In python_reliability.hpp define:

~~~cpp
enum class PythonFailureDirective : uint8_t {
    Restart = 0,
    Stop = 1,
    Escalate = 2,
    Quarantine = 3,
};

struct PythonSupervisionConfig final {
    uint32_t max_restarts{10};
    uint32_t restart_window_ms{5000};
    bool quarantine_on_exhaustion{false};
};

struct PythonReliabilityPort final {
    void* context{nullptr};
    void (*on_failure)(void*, const ActorAddress&, uint64_t,
                       const PythonFailureMetadata&) noexcept{nullptr};
    void (*on_restart_ready)(void*, const ActorAddress&, uint64_t) noexcept{
        nullptr};
};
~~~

PythonReliabilityController owns a bounded table keyed by ActorId with generation, policy, window start, restart count, immutable factory token, and current state. Its methods are noexcept and return explicit result values. It reuses SupervisionDirective numeric semantics but does not call Python.

- [ ] **Step 4: Make the bridge lifecycle-aware and emit structured failures**

Change PythonBridgeActor to inherit EventBasedActor and LifecycleActor, and add:

~~~cpp
LifecycleActor* as_lifecycle() noexcept override { return this; }
void on_drain() override;
void on_stop() override;
void on_deactivate() override;
void on_fail(error err) override;
void on_restart() override;
void on_quarantined(QuarantineReason reason) override;
~~~

On valid F2 input, construct a bounded PythonFailureMetadata, call make_failure_envelope with FailureSource::LanguageBinding, set_failure_reason(error(error_code, detail)), transition to kFailed, and invoke the reliability port. Do not call Python and do not immediately release the lease.

Restart replaces the lease through PythonRuntime::replace_actor_generation(actor, old_generation), cancels pending native asks/receipts, clears actor-owned pending maps, increments the runtime restart counter, and enqueues exactly one Restart dispatch with the new generation. Stop and quarantine reject future user dispatches and resolve pending commands with ActorDead or ActorNotReady.

- [ ] **Step 5: Reconstruct the Python actor and complete the restart handshake**

Add this frozen Python value:

~~~python
@dataclass(frozen=True, slots=True)
class SupervisionPolicy:
    max_restarts: int = 10
    restart_window_ms: int = 5000
    quarantine_on_exhaustion: bool = False
~~~

ActorSystem.spawn and ActorContext.spawn accept supervision: SupervisionPolicy | None. Freeze the class, constructor arguments, keyword arguments, actor type name, and behavior factory in the runner record.

Freeze spawn data with one recursive _freeze_spawn_value helper that accepts
None, bool, int, finite float, str, bytes, tuples of accepted values,
string-keyed mappings converted to sorted tuples, and registered protobuf
messages converted to (type_tag, deterministic_bytes). Reject mutable or
unsupported objects before the bridge is published. Thaw a fresh argument
graph for every construction and restart. Export SupervisionPolicy from
hpactor.__init__ and include it in __all__.

When _ActorRunner receives Restart, it:

1. Cancels the active handler and resolves its outstanding futures as ActorNotReadyError.
2. Awaits old_instance.on_stop() once.
3. Drops the old instance on the runtime loop.
4. Creates a new instance from the frozen factory record.
5. Binds behavior and awaits on_start().
6. Submits RestartReady with the replacement generation.

If reconstruction fails, send a new ActorFailed for the replacement generation so the same native policy chooses another restart, stop, escalation, or quarantine. Never reuse arbitrary attributes from the previous Python object.

- [ ] **Step 6: Run native and Python reliability tests to verify GREEN**

~~~bash
ninja -C build _hpactor test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonReliabilityTest.*:PythonBridgeActorTest.*Restart*'
PYTHONPATH=bindings/python python3 -m unittest \
  bindings.python.tests.unit.test_reliability -v
~~~

Expected: restart, stop, quarantine, reconstruction failure, cancellation, and stale-generation tests pass without timing assumptions.

- [ ] **Step 7: Commit supervision and restart**

~~~bash
git add bindings/python/native bindings/python/hpactor \
  tests/unit/python bindings/python/tests/unit/test_reliability.py
git commit -m "feat: supervise Python actor failures"
~~~

### Task 3: Deliver link and monitor events through serialized Python actor turns

**Files:**
- Modify: bindings/python/native/include/hpactor/python/python_bridge_actor.hpp
- Modify: bindings/python/native/src/python_bridge_actor.cpp
- Modify: bindings/python/native/include/hpactor/python/python_bridge_types.hpp
- Modify: bindings/python/native/src/python_capi/conversions.cpp
- Modify: bindings/python/hpactor/_actor.py
- Modify: bindings/python/hpactor/_runtime.py
- Modify: bindings/python/hpactor/_errors.py
- Modify: bindings/python/hpactor/__init__.py
- Modify: bindings/python/tests/unit/test_reliability.py
- Create: bindings/python/tests/integration/test_link_monitor.py

**Interfaces:**
- Consumes: existing link_to/unlink_from/monitor/demonitor operations, ExitMessage, DownMessage, PythonDispatchKind, and the one-turn-per-actor runner.
- Produces: immutable ExitEvent and DownEvent values plus Actor.on_exit and Actor.on_down serialized lifecycle hooks.

- [ ] **Step 1: Write failing Python hook and generation tests**

~~~python
class Watcher(Actor):
    def __init__(self, observed: list[object]) -> None:
        self.observed = observed

    def behavior(self) -> Behavior:
        return Behavior()

    async def on_exit(self, event: ExitEvent) -> None:
        self.observed.append(event)

    async def on_down(self, event: DownEvent) -> None:
        self.observed.append(event)


class LinkMonitorTest(unittest.IsolatedAsyncioTestCase):
    async def test_down_event_runs_after_active_turn(self) -> None:
        runtime, gate, observed = await linked_runtime()
        watcher = await runtime.install_test_actor(Watcher(observed))
        await runtime.start_blocked_turn(watcher, gate)
        runtime.native.push_down(watcher, failed_actor(), reason=17)
        runtime.native.fire_dispatch_reader()
        self.assertEqual(observed, [])
        gate.set()
        await runtime.wait_until_idle()
        self.assertIsInstance(observed[0], DownEvent)
~~~

Add tests for unlink/demonitor suppression, duplicate system-event sequence rejection, and stale target generations.

- [ ] **Step 2: Run the focused Python tests to verify RED**

~~~bash
PYTHONPATH=bindings/python python3 -m unittest \
  bindings.python.tests.unit.test_reliability -v
~~~

Expected: ExitEvent, DownEvent, and lifecycle hooks are missing.

- [ ] **Step 3: Convert native system messages to bounded dispatch values**

Intercept the existing exit and down system tags in PythonBridgeActor before ordinary behavior dispatch. Parse with generated HPActor messages, preserve the observed actor address, reason code, local generation, and sequence, then enqueue LinkedExit or MonitorDown through PythonRuntime::try_push_dispatch.

If the dispatch queue is full, call record_dispatch_rejected(), preserve the
rejected system-event metadata in the bounded failure record, and do not run a
Python hook on the scheduler. Malformed system payloads emit SerializationError
with LanguageBinding source.

- [ ] **Step 4: Add immutable public events and serialized hooks**

~~~python
@dataclass(frozen=True, slots=True)
class ExitEvent:
    actor: ActorRef
    reason: FailureReason
    error_code: int
    detail: str


@dataclass(frozen=True, slots=True)
class DownEvent:
    actor: ActorRef
    reason: FailureReason
    error_code: int
    detail: str
~~~

Actor supplies async no-op on_exit and on_down hooks. _ActorRunner routes system events through the same active-actor gate as message handlers; it never invokes two hooks or a hook and handler concurrently. Hook exceptions become ActorFailed exactly like handler exceptions.

Export ExitEvent and DownEvent from hpactor.__init__ and add them to __all__.

- [ ] **Step 5: Run link/monitor unit and real integration tests**

~~~bash
ninja -C build _hpactor test_unit_python_binding
PYTHONPATH=bindings/python python3 -m unittest \
  bindings.python.tests.unit.test_reliability -v
ctest --test-dir build -R 'PythonBindingIntegration.LinkMonitor' \
  --output-on-failure
~~~

Expected: all link, unlink, monitor, demonitor, ordering, hook-failure, and stale-generation cases pass.

- [ ] **Step 6: Commit link and monitor events**

~~~bash
git add bindings/python/native bindings/python/hpactor \
  bindings/python/tests/unit/test_reliability.py \
  bindings/python/tests/integration/test_link_monitor.py
git commit -m "feat: deliver Python link and monitor events"
~~~

### Task 4: Account for overflow and integrate Python bridge failures with the DLQ

**Files:**
- Modify: include/hpactor/msg/dead_letter_record.hpp
- Modify: bindings/python/native/include/hpactor/python/python_ports.hpp
- Modify: bindings/python/native/include/hpactor/python/python_reliability.hpp
- Modify: bindings/python/native/src/python_reliability.cpp
- Modify: bindings/python/native/src/python_bridge_actor.cpp
- Modify: bindings/python/native/src/python_gateway_actor.cpp
- Modify: bindings/python/native/src/python_native_system.cpp
- Modify: bindings/python/hpactor/_runtime.py
- Modify: tests/unit/python/test_python_reliability.cpp
- Create: bindings/python/tests/integration/test_overflow_dlq.py

**Interfaces:**
- Consumes: DeadLetterQueue, DeadLetterRecord, admitted TypedMessage metadata, queue snapshots, FailureEnvelope, and Phase 1A enqueue ownership rules.
- Produces: append-only PythonDispatchQueueFull and PythonCommandQueueFull reasons, PythonReliabilitySink, exact attempt accounting, typed ResourceExhausted completions, and optional DLQ evidence.

- [ ] **Step 1: Write failing ownership and accounting tests**

~~~cpp
TEST(PythonReliabilityTest, DispatchOverflowCreatesOneDlqRecord) {
    ReliabilityFixture fixture({.dispatch_queue_capacity = 64});
    auto actor = fixture.spawn_python_bridge();
    fixture.fill_dispatch_queue();

    auto message = fixture.make_message(actor, TypeTag{0x1000}, "payload");
    fixture.deliver_admitted(std::move(message));

    const auto runtime = fixture.native.snapshot();
    const auto dlq = fixture.system.dead_letter_snapshot();
    EXPECT_EQ(runtime.dispatch_rejected, 1u);
    ASSERT_EQ(dlq.records.size(), 1u);
    EXPECT_EQ(dlq.records[0].reason,
              DeadLetterReason::PythonDispatchQueueFull);
    EXPECT_EQ(dlq.records[0].type_tag, TypeTag{0x1000});
}

TEST(PythonReliabilityTest, CommandOverflowCompletesRejectedToken) {
    ReliabilityFixture fixture({.command_queue_capacity = 64});
    fixture.fill_command_queue();
    auto completion = fixture.submit_send();
    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->source, FailureSource::LanguageBinding);
    EXPECT_EQ(completion->error_code, errors::resource_exhausted);
}
~~~

Also assert that a failed enqueue retains producer ownership and that every attempt is exactly one of handled, rejected, cancelled, stale, or dead-lettered.

- [ ] **Step 2: Run native reliability tests to verify RED**

~~~bash
ninja -C build test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonReliabilityTest.*Overflow*'
~~~

Expected: append-only DLQ reasons and the reliability sink are missing.

- [ ] **Step 3: Append binding-specific dead-letter reasons**

Append without renumbering:

~~~cpp
PythonDispatchQueueFull = 20,
PythonCommandQueueFull = 21,
PythonHandlerFailed = 22,
PythonShutdownCancelled = 23,
~~~

Update to_string, failure_reason, failure_source, compatibility tests, and CLI formatting. Map all four to FailureSource::LanguageBinding. Dispatch/command full maps to ResourceExhausted, handler failure to ActorDead, and shutdown cancellation to ShuttingDown.

- [ ] **Step 4: Implement the fixed reliability sink**

~~~cpp
struct PythonReliabilitySink final {
    void* context{nullptr};
    void (*record_failure)(void*, const FailureEnvelope&) noexcept{nullptr};
    bool (*dead_letter)(void*, DeadLetterRecord&&) noexcept{nullptr};
};
~~~

PythonNativeSystem builds the sink from ActorSystem failure and DLQ APIs. PythonBridgeActor constructs DeadLetterRecord from the admitted message, including sender, target, type tag, message ID, flags, priority, deadline, trace IDs, payload size/sample, queue depth/capacity, and monotonic timestamp.

Dispatch rejection occurs after mailbox admission, so never mutate the already-resolved delivery receipt. Command rejection creates a new PythonCompletion for that command token with ResourceExhausted and retry_after_ns = 0.

- [ ] **Step 5: Make Python-side accounting total and non-overlapping**

_ActorRuntime maintains monotonic counters for handled, rejected, cancelled, failed, and stale. Every terminal path calls exactly one private _finish_attempt(category) helper. Assertions in debug tests reject double completion. Queue pressure schedules another drain only when free slots exist; it never spins a reader callback while the Python deque is full.

- [ ] **Step 6: Run native and end-to-end overflow tests**

~~~bash
ninja -C build _hpactor test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonReliabilityTest.*'
ctest --test-dir build -R 'PythonBindingIntegration.OverflowDlq' \
  --output-on-failure
~~~

Expected: dispatch and command overflow, DLQ enabled/disabled behavior, payload truncation, ownership, and exact accounting pass.

- [ ] **Step 7: Commit overflow and DLQ integration**

~~~bash
git add include/hpactor/msg/dead_letter_record.hpp \
  bindings/python/native bindings/python/hpactor \
  tests/unit/python bindings/python/tests/integration/test_overflow_dlq.py
git commit -m "feat: account for Python bridge overflow"
~~~

### Task 5: Emit bounded metrics, structured logs, and handler child spans

**Files:**
- Create: bindings/python/native/include/hpactor/python/python_observability.hpp
- Create: bindings/python/native/src/python_observability.cpp
- Modify: bindings/python/native/include/hpactor/python/python_ports.hpp
- Modify: bindings/python/native/include/hpactor/python/python_native_system.hpp
- Modify: bindings/python/native/src/python_native_system.cpp
- Modify: bindings/python/native/src/python_capi/native_system_type.cpp
- Modify: bindings/python/hpactor/_runtime.py
- Create: tests/unit/python/test_python_observability.cpp
- Create: bindings/python/tests/integration/test_observability.py

**Interfaces:**
- Consumes: MetricRegistry, Logger, TraceManager, incoming TraceContext, PythonRuntimeSnapshot, bounded actor type names, and native C API value conversion.
- Produces: PythonObservability, the approved metric families, bounded log events, begin_handler_span/finish_handler_span token APIs, and python.actor.handle child spans.

- [ ] **Step 1: Write failing metric, label, log, and span tests**

~~~cpp
TEST(PythonObservabilityTest, RegistersApprovedMetricFamilies) {
    MetricRegistry registry;
    PythonObservability obs({.metrics = &registry});
    obs.record_dispatch("echo", false);
    obs.record_handler("echo", std::chrono::microseconds{250}, false);
    const auto names = metric_names(registry.snapshot());
    EXPECT_THAT(names, Contains("hpactor_python_dispatch_total"));
    EXPECT_THAT(names, Contains("hpactor_python_handler_duration_seconds"));
    EXPECT_THAT(names, Not(Contains("actor_id")));
}

TEST(PythonObservabilityTest, BoundsFailureLogFields) {
    CapturingLogger logger;
    PythonObservability obs({.logger = logger.port()});
    obs.log_handler_failure(failure_with_detail(64 * 1024));
    EXPECT_LE(logger.last_field("detail").size(), 1024u);
    EXPECT_LE(logger.last_field("traceback").size(), 16384u);
}
~~~

Add a memory-exporter test asserting python.actor.handle is a child of the incoming consumer context and has separate queue_wait_ns and handler_duration_ns attributes.

- [ ] **Step 2: Run observability tests to verify RED**

~~~bash
ninja -C build test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonObservabilityTest.*'
~~~

Expected: PythonObservability and its metric/span APIs do not exist.

- [ ] **Step 3: Register the exact bounded metric families**

PythonObservability registers and caches references for:

- hpactor_python_dispatch_total
- hpactor_python_dispatch_rejected_total
- hpactor_python_dispatch_queue_depth
- hpactor_python_command_total
- hpactor_python_command_rejected_total
- hpactor_python_command_queue_depth
- hpactor_python_handler_duration_seconds
- hpactor_python_handler_exceptions_total
- hpactor_python_handler_cancelled_total
- hpactor_python_event_loop_lag_seconds
- hpactor_python_stale_completions_total

Use empty LabelSet by default. If actor-type metrics are enabled, resolve the type through a pre-start frozen set capped by max_actor_bindings; unknown types collapse to actor_type="other". Never use actor ID, message ID, exception text, or type tag as metric labels.

Queue depth and loop lag gauges are refreshed from PythonRuntimeSnapshot. Handler duration is a histogram. All counters are monotonic.

- [ ] **Step 4: Emit structured bounded logs**

Define PythonLogEvent as an owned value with actor, actor_type, generation, message_id, type_tag, trace IDs, failure reason, queue depth/capacity, exception_type, detail, and traceback. PythonObservability maps it to existing LogEvent/LogField values under the actor category.

Use debug for stale completions, warning for queue rejection and shutdown cancellation, and error for unhandled handler failures. Enforce string bounds before calling Logger::emit. Logging failure or ring overflow never changes actor behavior.

- [ ] **Step 5: Add bounded native span tokens**

Expose low-level methods:

~~~cpp
[[nodiscard]] uint64_t
begin_handler_span(const PythonHandlerSpanStart& start) noexcept;
void finish_handler_span(uint64_t token,
                         PythonHandlerSpanStatus status) noexcept;
~~~

The start value contains parent TraceContext, actor type, generation, message ID, type tag, dispatch enqueue ns, and handler start ns. PythonNativeSystem stores SpanHandle values in a mutex-protected map bounded by completion_queue_capacity. Full-map start returns token 0, increments a private span_tokens_rejected counter, and emits one rate-limited debug log; it does not mutate TraceManager internals.

The child span is named python.actor.handle and records dispatch.queue_wait_ns, protobuf.decode_ns, handler.duration_ns, response.serialize_ns, actor.type, actor.generation, and message.type_tag. Actor ID and payload are not attributes.

The C API converts only primitive tuples and integer tokens. _ActorRunner starts immediately before decode and finishes in finally with Ok, Error, Cancelled, Expired, or Stale.

- [ ] **Step 6: Run focused native and real observability tests**

~~~bash
ninja -C build _hpactor test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonObservabilityTest.*'
ctest --test-dir build -R 'PythonBindingIntegration.Observability' \
  --output-on-failure
~~~

Expected: exact metric names/types, bounded labels, log fields, parent/child traces, cancellation status, and disabled-observability behavior pass.

- [ ] **Step 7: Commit observability**

~~~bash
git add bindings/python/native bindings/python/hpactor \
  tests/unit/python/test_python_observability.cpp \
  bindings/python/tests/integration/test_observability.py
git commit -m "feat: observe Python actor runtime"
~~~

### Task 6: Add bounded asynchronous inspection and Python CLI commands

**Files:**
- Create: bindings/python/native/include/hpactor/python/python_inspection.hpp
- Create: bindings/python/native/src/python_inspection.cpp
- Create: bindings/python/native/src/python_cli_commands.cpp
- Modify: bindings/python/native/include/hpactor/python/python_native_system.hpp
- Modify: bindings/python/native/src/python_native_system.cpp
- Modify: bindings/python/native/src/python_bridge_actor.cpp
- Modify: bindings/python/native/src/python_capi/native_system_type.cpp
- Modify: bindings/python/hpactor/_actor.py
- Modify: bindings/python/hpactor/_runtime.py
- Create: tests/unit/python/test_python_inspection.cpp
- Create: bindings/python/tests/unit/test_inspection.py
- Create: bindings/python/tests/integration/test_cli_health.py
- Modify: bindings/python/native/CMakeLists.txt

**Interfaces:**
- Consumes: kPythonInspectTag, PythonActorSnapshot, CommandRegistry, ICommand, PythonRuntime dispatch/completion queues, and a dedicated CLI daemon execution context.
- Produces: Actor.inspect, PythonInspectionService, timed request/result correlation, /python status, /python actors, and /python actor ID inspect.

- [ ] **Step 1: Write failing bounded inspection tests**

~~~python
class Inspectable(Actor):
    def __init__(self) -> None:
        self.count = 7

    def behavior(self) -> Behavior:
        return Behavior()

    async def inspect(self) -> dict[str, object]:
        return {"count": self.count, "mode": "ready"}


class InspectionTest(unittest.IsolatedAsyncioTestCase):
    async def test_inspection_runs_after_active_turn_and_is_bounded(self) -> None:
        runtime, gate, actor = await blocked_inspectable()
        pending = asyncio.create_task(runtime.inspect_actor(actor, timeout=1.0))
        await asyncio.sleep(0)
        self.assertFalse(pending.done())
        gate.set()
        result = await pending
        self.assertEqual(result.detail, {"count": 7, "mode": "ready"})
        self.assertLessEqual(len(result.serialized), 16 * 1024)
~~~

Native tests cover unknown actor, stale generation, request table full, timeout, duplicate result, invalid JSON value, and shutdown cancellation.

- [ ] **Step 2: Run inspection tests to verify RED**

~~~bash
ninja -C build test_unit_python_binding
PYTHONPATH=bindings/python python3 -m unittest \
  bindings.python.tests.unit.test_inspection -v
~~~

Expected: inspection service, actor hook, and completion path are absent.

- [ ] **Step 3: Implement bounded native request correlation**

PythonInspectionService owns at most completion_queue_capacity pending entries:

~~~cpp
class PythonInspectionService final {
  public:
    result<PythonInspectResult>
    inspect(ActorAddress actor, uint64_t generation,
            std::chrono::milliseconds timeout) noexcept;
    void complete(PythonInspectResult result) noexcept;
    void cancel_all(error reason) noexcept;
};
~~~

inspect() allocates a nonzero token, submits a protected F3 request through the bridge, and waits only on the dedicated CLI/management thread. It never waits on scheduler, network, telemetry, or Python runtime threads. Timeout erases the token and returns errors::timeout. Late results are counted stale.

- [ ] **Step 4: Run inspection on the serialized actor turn**

Actor defines async inspect() returning Mapping[str, JSONScalar | list | mapping] with default {}. _ActorRunner services inspection through the same active-turn gate as handlers and lifecycle hooks.

Serialize deterministically with json.dumps(sort_keys=True, separators=(",", ":"), ensure_ascii=False). Reject non-string keys, NaN/infinity, custom objects, cycles, nesting above 16, and output above 16 KiB. Include the base PythonActorSnapshot even when the custom detail is empty.

- [ ] **Step 5: Register three self-contained CLI commands**

Implement ICommand classes with static CommandRegistration values:

- /python status reads PythonRuntimeSnapshot only.
- /python actors reads a bounded copy of PythonActorSnapshot values sorted by actor ID.
- /python actor ID inspect calls PythonInspectionService with a 2-second default timeout and renders the bounded result.

Return a clear "Python binding disabled" result when ENABLE_PYTHON_BINDINGS is off or no native system is registered. CLI code never imports Python, reads Python object memory, or retains a reference to a Python runner.

- [ ] **Step 6: Run unit and CLI integration tests**

~~~bash
ninja -C build _hpactor test_unit_python_binding test_unit_cli
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonInspectionTest.*'
PYTHONPATH=bindings/python python3 -m unittest \
  bindings.python.tests.unit.test_inspection -v
ctest --test-dir build -R 'PythonBindingIntegration.Cli' \
  --output-on-failure
~~~

Expected: status, actor listing, inspection success/timeout/overflow, disabled state, bounds, ordering, and no-direct-memory-access tests pass.

- [ ] **Step 7: Commit inspection and CLI**

~~~bash
git add bindings/python/native bindings/python/hpactor \
  tests/unit/python/test_python_inspection.cpp \
  bindings/python/tests/unit/test_inspection.py \
  bindings/python/tests/integration/test_cli_health.py
git commit -m "feat: inspect Python actors through CLI"
~~~

### Task 7: Integrate readiness and harden coordinated shutdown

**Files:**
- Create: bindings/python/native/include/hpactor/python/python_health_check.hpp
- Create: bindings/python/native/src/python_health_check.cpp
- Create: bindings/python/native/include/hpactor/python/python_shutdown_adapter.hpp
- Create: bindings/python/native/src/python_shutdown_adapter.cpp
- Modify: bindings/python/native/include/hpactor/python/python_native_system.hpp
- Modify: bindings/python/native/src/python_native_system.cpp
- Modify: bindings/python/native/include/hpactor/python/python_runtime.hpp
- Modify: bindings/python/native/src/python_runtime.cpp
- Modify: bindings/python/hpactor/_runtime.py
- Modify: bindings/python/hpactor/_system.py
- Create: tests/unit/python/test_python_health_shutdown.cpp
- Modify: bindings/python/tests/integration/test_cli_health.py
- Create: bindings/python/tests/integration/test_shutdown_under_load.py

**Interfaces:**
- Consumes: IHealthCheck, HealthCheckEngine, PythonRuntimeSnapshot, ShutdownCoordinator user phases, handler_shutdown_timeout_ms, and idempotent Phase 1B stop.
- Produces: PythonRuntimeHealthCheck, readiness reasons, a Python drain/flush shutdown phase, timed handler cancellation, and callback-quiescent teardown.

- [ ] **Step 1: Write failing readiness matrix and shutdown-order tests**

~~~cpp
TEST(PythonHealthTest, ReadinessUsesStateAndHeartbeatNotPressure) {
    HealthFixture fixture;
    fixture.runtime.start(fixture.wake_port());
    fixture.runtime.record_heartbeat(fixture.now_ns());
    fixture.fill_dispatch_queue();
    EXPECT_EQ(fixture.check().status, HealthStatus::Healthy);

    fixture.advance_ms(fixture.config.loop_lag_unready_ms + 1);
    EXPECT_EQ(fixture.check().status, HealthStatus::Unhealthy);
    EXPECT_EQ(fixture.check().reason, "python event loop heartbeat stale");
}

TEST(PythonShutdownTest, QuiescesPythonBeforeNativeDestruction) {
    ShutdownFixture fixture;
    fixture.start_handler_that_never_finishes();
    fixture.native.stop();
    EXPECT_THAT(fixture.events, ElementsAre(
        "draining", "ingress-stopped", "actors-drained",
        "handler-cancelled", "on-stop", "readers-removed",
        "loop-stopped", "thread-joined", "queues-destroyed"));
    EXPECT_EQ(fixture.late_callbacks(), 0u);
}
~~~

- [ ] **Step 2: Run health/shutdown tests to verify RED**

~~~bash
ninja -C build test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonHealthTest.*:PythonShutdownTest.*'
~~~

Expected: health check and coordinated shutdown adapter do not exist.

- [ ] **Step 3: Implement snapshot-only health and readiness**

PythonRuntimeHealthCheck implements IHealthCheck:

~~~cpp
class PythonRuntimeHealthCheck final : public process::IHealthCheck {
  public:
    explicit PythonRuntimeHealthCheck(const PythonRuntime* runtime) noexcept;
    std::string_view name() const noexcept override;
    bool is_critical() const noexcept override;
    process::HealthCheckResult
    check(const process::CheckContext& ctx) override;
};
~~~

Treat a null runtime pointer as a disabled binding and return Healthy. For an
enabled binding, return Healthy only when Running with a fresh heartbeat.
Return Unhealthy for startup failure, Created/Starting after startup deadline,
Draining/Stopped while the system still claims readiness, or stale heartbeat.
Include state, lag ms, and queue depths in bounded reason details. Queue
pressure with a fresh heartbeat returns Healthy and remains visible through
metrics.

- [ ] **Step 4: Register an explicit shutdown phase and exact teardown order**

PythonShutdownAdapter performs:

1. begin_draining and reject new spawns.
2. let ActorSystem stop external ingress.
3. let normal actor drain policies drain bridge mailboxes.
4. close dispatch admission.
5. request Python runner drain with handler_shutdown_timeout_ms.
6. cancel remaining handler/inspection/ask/receipt tasks.
7. drain or reject command/completion records.
8. run on_stop once and destroy Python actor objects on the loop.
9. remove readers, close notifiers, stop the loop, and join from a non-runtime thread.
10. destroy gateway, queues, and native state.

Register python-runtime-flush after ShutdownPhase::DrainingActors with the configured handler timeout. PythonNativeSystem::stop calls begin_draining before ActorSystem shutdown starts. If stop is called on the Python runtime thread, return errors::operation_not_permitted and marshal final stop to the owning application thread; never self-join.

- [ ] **Step 5: Make Python cancellation outcomes explicit**

_RuntimeThread.drain_and_stop(timeout) waits for active turns until the monotonic deadline, then cancels them. Ask futures resolve AskCancelledError with ShuttingDown source; delivery receipts receive DeliveryStatus.ShuttingDown; inspection futures receive SystemClosedError. Each cancelled handler increments handler_cancelled exactly once and its span finishes Cancelled.

After Python objects are destroyed, set a native python_objects_quiesced flag. Every completion, notifier, inspection, and C API entry checks this flag and rejects late work without dereferencing Python state.

- [ ] **Step 6: Run health and shutdown tests**

~~~bash
ninja -C build _hpactor test_unit_python_binding
./build/tests/unit/python/test_unit_python_binding \
  --gtest_filter='PythonHealthTest.*:PythonShutdownTest.*'
ctest --test-dir build -R 'PythonBindingIntegration.(CliHealth|ShutdownUnderLoad)' \
  --output-on-failure
~~~

Expected: readiness matrix, pressure behavior, graceful drain, timed cancellation, repeated stop, self-stop rejection, and late-callback fencing pass.

- [ ] **Step 7: Commit health and shutdown hardening**

~~~bash
git add bindings/python/native bindings/python/hpactor \
  tests/unit/python/test_python_health_shutdown.cpp \
  bindings/python/tests/integration/test_cli_health.py \
  bindings/python/tests/integration/test_shutdown_under_load.py
git commit -m "feat: harden Python runtime shutdown"
~~~

### Task 8: Prove supervision, observability, and operations end to end

**Files:**
- Create: bindings/python/tests/integration/test_supervision.py
- Modify: bindings/python/tests/integration/test_link_monitor.py
- Modify: bindings/python/tests/integration/test_overflow_dlq.py
- Modify: bindings/python/tests/integration/test_observability.py
- Modify: bindings/python/tests/integration/test_cli_health.py
- Modify: bindings/python/tests/integration/test_shutdown_under_load.py
- Modify: bindings/python/tests/CMakeLists.txt
- Modify: tests/integration/python/CMakeLists.txt

**Interfaces:**
- Consumes: the complete Phase 1C native and Python surfaces.
- Produces: deterministic real-module evidence for restart, stop, quarantine, link/monitor, DLQ, telemetry, CLI, health, and shutdown.

- [ ] **Step 1: Add a real restart workflow**

~~~python
@actor("restart-once")
class RestartOnce(Actor):
    starts = 0

    async def on_start(self) -> None:
        type(self).starts += 1

    def behavior(self) -> Behavior:
        return Behavior().on_request(
            StringValue, StringValue, self.handle)

    async def handle(self, msg: StringValue,
                     ctx: ActorContext) -> StringValue:
        if type(self).starts == 1:
            raise RuntimeError("first incarnation fails")
        return StringValue(value=f"generation-{type(self).starts}")


class SupervisionWorkflowTest(unittest.IsolatedAsyncioTestCase):
    async def test_restart_reconstructs_actor_and_fences_old_completion(self):
        async with configured_system() as system:
            ref = await system.spawn(
                RestartOnce,
                supervision=SupervisionPolicy(max_restarts=2))
            with self.assertRaises(ActorNotReadyError):
                await system.ask(ref, StringValue(value="fail"),
                                 response_type=StringValue, timeout=5.0)
            await system.wait_until_actor_ready(ref, timeout=5.0)
            reply = await system.ask(ref, StringValue(value="ok"),
                                     response_type=StringValue, timeout=5.0)
            self.assertEqual(reply.value, "generation-2")
~~~

Add stop, escalation, quarantine, reconstruction-failure, on_stop-once, and inspect-after-restart cases. Use events and public APIs, not native actor memory.

- [ ] **Step 2: Add cross-surface failure correlation**

Cause one Python handler failure with a fixed incoming trace. Assert:

- one LanguageBinding FailureEnvelope;
- one PythonHandlerFailed DLQ record when configured;
- one handler exception metric;
- one bounded structured error log;
- one python.actor.handle span with Error status and the incoming parent;
- one restart or terminal supervision decision;
- CLI and actor snapshot counters agree.

Use stable IDs injected by the test harness. Do not compare timestamps for exact equality.

- [ ] **Step 3: Add readiness and shutdown workflow tests**

Pause heartbeat publication while keeping queues empty and assert /readyz becomes non-ready while /healthz remains live. Restore heartbeat and assert readiness recovers. Then start one cooperative handler and one cancellation-resistant handler, begin shutdown, and prove the first drains, the second cancels at the configured timeout, all on_stop hooks run once, and the runtime thread joins.

- [ ] **Step 4: Register and run focused Phase 1C integration tests**

~~~bash
ninja -C build _hpactor
ctest --test-dir build -R 'PythonBindingIntegration.(Supervision|LinkMonitor|OverflowDlq|Observability|CliHealth|ShutdownUnderLoad)' \
  --output-on-failure
~~~

Expected: every Phase 1C integration case passes without skipped supported-platform behavior or timing-order assumptions.

- [ ] **Step 5: Commit end-to-end workflows**

~~~bash
git add bindings/python/tests/integration \
  bindings/python/tests/CMakeLists.txt tests/integration/python/CMakeLists.txt
git commit -m "test: verify Python reliability workflows"
~~~

### Task 9: Add native stress, architecture, leak, and sanitizer evidence

**Files:**
- Create: tests/integration/python/test_python_runtime_stress.cpp
- Modify: tests/integration/python/CMakeLists.txt
- Modify: tests/architecture/CMakeLists.txt
- Modify: bindings/python/tests/CMakeLists.txt

**Interfaces:**
- Consumes: Phase 1C queues, counters, generation fencing, shutdown adapter, CPython limited module, and existing architecture-test conventions.
- Produces: concurrent attempt accounting, no-concurrent-handler evidence, callback-quiescence evidence, reference-leak coverage, and enforceable architecture boundaries.

- [ ] **Step 1: Add concurrent native producer stress**

Create 8 producer threads, each attempting 10000 immutable dispatch records against 64 Python actor keys while one consumer drains with budget 256. Use a barrier for start and atomics for accepted/rejected/handled/cancelled/dead-lettered counts.

Assert:

~~~cpp
EXPECT_EQ(attempted, accepted + rejected);
EXPECT_EQ(accepted, handled + cancelled + dead_lettered);
EXPECT_LE(max_dispatch_depth, config.dispatch_queue_capacity);
EXPECT_LE(max_command_depth, config.command_queue_capacity);
EXPECT_EQ(concurrent_handler_violations, 0u);
EXPECT_EQ(replacement_generation_resolutions, 0u);
~~~

Repeat while forcing restart every 1000 handled messages and while beginning shutdown with producers still active.

- [ ] **Step 2: Add architecture fitness checks**

Extend architecture CTest scripts to fail when:

- Python.h or CPython API names appear outside bindings/python/native/src/python_capi.
- PyObject pointers appear in python bridge types, ports, queues, runtime, reliability, observability, inspection, health, or shutdown files.
- dynamic_cast, typeid, throw, catch, -frtti, or -fexceptions appears in binding targets.
- std::function appears in binding ports or cross-thread callbacks.
- an unbounded deque/vector/map is used for dispatch, command, completion, span-token, inspection-token, or pending-operation storage without an adjacent capacity check.
- CLI or health code imports Python or reads actor object fields.
- scheduler or network sources include Python binding C API headers.
- local tags 0xF0 through 0xF3 are accepted by remote frame routing.

- [ ] **Step 3: Add debug-Python reference and late-callback tests**

Run the integration suite with PYTHONMALLOC=debug and PYTHONASYNCIODEBUG=1. Keep weakref references to actor instances, behavior bound methods, contexts, wrapped futures, and protobuf values. After deterministic shutdown and gc.collect(), assert every weak reference is dead.

Instrument native notifier callbacks, completion callbacks, inspection completion, and span finishing. Assert zero callback executes after python_objects_quiesced becomes true.

- [ ] **Step 4: Run focused stress and architecture evidence**

~~~bash
ninja -C build test_integration_python_binding _hpactor
ctest --test-dir build -R 'PythonBinding(Stress|Architecture)' \
  --output-on-failure
PYTHONMALLOC=debug PYTHONASYNCIODEBUG=1 \
  ctest --test-dir build -R 'PythonBindingIntegration' \
  --output-on-failure
~~~

Expected: all attempts are accounted, capacity maxima remain bounded, no generation/concurrency invariant fails, architecture checks pass, and debug Python reports no retained actor graph or late callback.

- [ ] **Step 5: Run supported Linux TSAN evidence**

~~~bash
cmake -S . -B build-tsan -GNinja \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
  -DENABLE_PYTHON_BINDINGS=ON -DENABLE_TSAN=ON
ninja -C build-tsan test_unit_python_binding test_integration_python_binding
ctest --test-dir build-tsan -R 'PythonBinding' --output-on-failure
~~~

Expected: no native data race, lock-order failure, late notifier access, or unsupported skipped Phase 1C test.

- [ ] **Step 6: Commit stress and architecture coverage**

~~~bash
git add tests/integration/python tests/architecture \
  bindings/python/tests/CMakeLists.txt
git commit -m "test: stress Python reliability bridge"
~~~

### Task 10: Record Phase 1C acceptance evidence and run full verification

**Files:**
- Modify: CLAUDE_MEMORY.md
- Modify: docs/superpowers/specs/2026-07-03-python-language-binding-design.md

**Interfaces:**
- Consumes: complete Phase 1C implementation and test evidence.
- Produces: honest project status, exact verification counts, and a clean boundary before Phase 1D packaging.

- [ ] **Step 1: Audit every Phase 1C requirement**

Create a local checklist mapping design sections 11, 12, 13, 14, 17.3, 17.4, and acceptance criteria 6 through 9 to the implementing task and exact test name. Resolve every missing mapping before changing status.

- [ ] **Step 2: Update project memory without claiming packaging**

Add a dated Python Binding Phase 1C Reliability and Operations entry to CLAUDE_MEMORY.md listing:

- supervision directives and replacement-generation restart;
- serialized link/monitor hooks;
- dispatch/command overflow and DLQ evidence;
- exact metric families, structured logs, and python.actor.handle tracing;
- CLI status/actors/inspect;
- heartbeat readiness and shutdown sequence;
- exact unit/integration/stress/architecture/debug-Python/TSAN counts.

Include this limitation verbatim:

~~~text
Phase 1C is a build-tree development surface. ABI3 wheel production, platform
repair, clean-environment installation, release documentation, and supported
distribution begin in Phase 1D; the developer manual's "no official bindings"
limitation remains accurate.
~~~

Change the design status to:

~~~markdown
**Status:** Approved design; Phases 1A, 1B, and 1C implemented
~~~

Only make that change after every required verification command passes.

- [ ] **Step 3: Run the full configured build and test suite**

~~~bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF -DENABLE_PYTHON_BINDINGS=ON
ninja -C build
ctest --test-dir build --output-on-failure --parallel 8
git diff --check
~~~

Expected: the full build succeeds, every configured C++/Python/architecture test passes, and git diff --check prints no output.

- [ ] **Step 4: Re-run Phase 1C acceptance lanes after the full suite**

~~~bash
ctest --test-dir build -R 'PythonBinding' --output-on-failure
PYTHONMALLOC=debug PYTHONASYNCIODEBUG=1 \
  ctest --test-dir build -R 'PythonBindingIntegration' \
  --output-on-failure
~~~

Expected: Phase 1C remains green in isolation and debug Python reports no retained references or late callbacks.

- [ ] **Step 5: Inspect the final diff and commit acceptance evidence**

~~~bash
git status --short
git diff --check
git diff --stat
git add CLAUDE_MEMORY.md \
  docs/superpowers/specs/2026-07-03-python-language-binding-design.md
git commit -m "docs: record Python reliability operations status"
~~~

Expected: only intended Phase 1C implementation, tests, build wiring, and status files are present.

## Plan Completion Checklist

- [ ] Phase 1A and Phase 1B are implemented and passing before Phase 1C begins.
- [ ] system.python config is parsed by a self-registering parser using TomlTableView and every approved bound is enforced before thread creation.
- [ ] Existing TypeTag, failure, DLQ, protobuf, command, and completion numeric values remain unchanged.
- [ ] Unhandled Python exceptions become bounded LanguageBinding failures and an explicit supervision directive.
- [ ] Restart allocates a replacement generation, reconstructs the Python object from frozen factory data, and never restores arbitrary object memory.
- [ ] Old-generation dispatches, completions, inspection results, and restart acknowledgements never affect the replacement actor.
- [ ] Link and monitor events run through the same non-reentrant serialized actor turn as handlers.
- [ ] Every dispatch/command attempt has exactly one terminal accounting category.
- [ ] Dispatch overflow preserves the already-resolved mailbox receipt and optionally creates one bounded DLQ record.
- [ ] Command overflow resolves the new command token with ResourceExhausted.
- [ ] All eleven approved metric families exist with bounded cardinality and no actor-ID label.
- [ ] Structured failure logs preserve bounded correlation fields and never change actor behavior when logging is unavailable.
- [ ] python.actor.handle spans continue incoming W3C context and separate queue wait from handler time.
- [ ] /python status and /python actors read bounded native snapshots only.
- [ ] /python actor ID inspect uses a timed bounded request and never reads Python memory from CLI code.
- [ ] Readiness reflects startup, runtime state, heartbeat lag, and shutdown; queue pressure alone does not make the node unready.
- [ ] Shutdown follows the approved ten-step order and joins the Python thread from a non-runtime thread.
- [ ] No native callback, notifier, completion, span, or inspection result references Python after object quiescence.
- [ ] Native stress accounts for every attempt, respects capacity, preserves one handler per actor, and fences replacement generations.
- [ ] Architecture checks enforce C API containment, value-only queues, fixed ports, remote-tag rejection, and no RTTI/exceptions.
- [ ] Debug-Python leak checks and supported Linux TSAN evidence pass.
- [ ] Full configured build/test evidence passes before Phase 1C is marked implemented.
- [ ] No ABI3 wheel, installation, or supported-distribution claim is made before Phase 1D.

## Execution Handoff

Plan complete. Execute only after the Phase 1A and Phase 1B acceptance checklists pass. Use superpowers:subagent-driven-development for one fresh implementer and review gate per task, or superpowers:executing-plans for inline batches with review checkpoints.
