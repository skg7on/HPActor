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

# Python Language Binding Design

**Status:** Approved design; Phase 1A native foundation implemented

**Date:** 2026-07-03

**Target:** CPython 3.11+ on Linux x86_64/ARM64 and macOS x86_64/ARM64

## 1. Summary

HPActor will provide a phased Python package named `hpactor`.

Phase 1 lets Python authors define in-process actors that run on the existing
HPActor C++ runtime. Python handlers execute on one dedicated asyncio event-loop
thread per process. HPActor scheduler workers never invoke Python, acquire the
GIL, or wait for Python progress. A C++ `PythonBridgeActor` moves owned message
envelopes from the normal HPActor delivery path into a bounded handoff queue. A
`PythonGatewayActor` returns bounded Python commands to actor-owned C++
execution paths.

Phase 2 adds a pure-Python external client for runtime surfaces that exist in
the current implementation: application HTTP gateway routes, health, metrics,
and CLI over UDS/TCP. It does not represent backlog-only authenticated admin or
wire-negotiation behavior as implemented.

The native extension uses the CPython limited C API, explicit status returns,
and ABI3 packaging. It does not use nanobind or pybind11 because the repository
permits exceptions in only three existing translation units and otherwise
requires exception-free and RTTI-free C++.

## 2. Current Runtime Baseline

The design is grounded in the current runtime and manual, not only the
production backlog.

Implemented foundations used by the binding include:

- `ActorSystem`, `ActorContext`, `ActorRef`, and location-transparent delivery.
- Protobuf `TypedMessage`, explicit `TypeTag`, sender, message ID, deadline,
  delivery flags, and W3C trace metadata.
- `MessagingRuntime` and the full local `DeliveryPipeline` for admission,
  quarantine, deadline, deduplication, bounded mailbox, backpressure, failure
  observability, and DLQ handoff.
- Ask/request handles, delivery receipts, scheduling, lifecycle, graceful
  shutdown, supervision, quarantine, and actor passivation foundations.
- Metrics, structured logging, tracing, health/readiness, CLI, HTTP gateway,
  topology configuration, and runtime snapshots.
- Runtime ownership extraction into actor, messaging, stream, network,
  observability, and lifecycle components.

Important current limitations remain unchanged:

- Durable outbox/inbox is not implemented.
- General transport resend is incomplete even though reliable-message tracking
  and ACK/NACK foundations exist.
- Dynamic cluster placement, rolling protocol negotiation, authenticated admin
  API, authorization, and audit remain partial or backlog work.
- The wire protocol is protobuf-only and requires manually stable `TypeTag`
  assignments.
- Windows is not supported.

The Python binding must preserve these distinctions in documentation and
runtime results.

## 3. Goals

1. Offer an idiomatic asyncio-first API for defining and using HPActor actors.
2. Preserve HPActor delivery, mailbox, lifecycle, supervision, tracing, and
   observability contracts across the language boundary.
3. Keep all Python handler execution off cooperative scheduler workers.
4. Preserve one serialized turn over each Python actor's private state.
5. Bound all cross-thread queues and make rejection observable.
6. Use generated protobuf classes with fixed, cross-language `TypeTag` values.
7. Ship ABI3 wheels for CPython 3.11+ on the supported Linux and macOS targets.
8. Add an honest external SDK after the in-process binding is proven safe.

## 4. Non-Goals

Phase 1 does not:

- Expose raw scheduler, mailbox, transport, allocator, or runtime pointers.
- Execute Python subclasses directly as `EventBasedActor` scheduler callbacks.
- Expose `BlockingActor`, `DaemonActor`, `PollingActor`, or
  `DenseComputingActor` as Python inheritance points.
- Support arbitrary pickle, JSON, or Python-object messages.
- Add actor reentrancy while an async handler is suspended.
- Add multiple interpreters or free-threaded Python execution shards.
- Claim durable delivery, dynamic cluster placement, protocol negotiation, or
  authenticated administration.
- Support Windows wheels.

Declarative topology for Python actor classes is a Phase 1 follow-up after
imperative spawn, restart, and shutdown behavior is verified.

## 5. Architectural Decisions

### 5.1 Execution domains

The process contains three relevant execution domains:

1. **HPActor scheduler workers** execute C++ actors, including
   `PythonBridgeActor` and `PythonGatewayActor`.
2. **Python actor runtime thread** owns the dedicated asyncio loop, Python actor
   instances, behaviors, handler tasks, and Python-side registries.
3. **Python application thread/loop** owns the user's application coroutine and
   public `ActorSystem` proxy. Public calls are marshalled to the actor runtime
   loop before entering the native command queue, so the native command queue
   remains single-producer.

The Python application loop may be the main thread. The actor runtime loop is
always the dedicated thread selected by this design.

### 5.2 Inbound message flow

1. A local or remote message enters the normal HPActor delivery boundary.
2. The existing delivery pipeline performs lookup, admission, lifecycle and
   quarantine checks, deadline handling, deduplication, mailbox accounting,
   failure observability, DLQ handling, tracing propagation, and scheduler
   wakeup.
3. `PythonBridgeActor::receive()` converts the admitted `TypedMessage` into an
   owned `PythonDispatchEnvelope` and attempts a bounded enqueue.
4. Successful enqueue signals a non-blocking platform notifier backed by
   `eventfd` on Linux and a non-blocking self-pipe/socket pair on macOS.
5. The asyncio loop's reader callback drains a bounded batch and routes each
   envelope to the addressed Python actor's FIFO runner.
6. The runner deserializes protobuf bytes and invokes exactly one handler for
   that actor at a time.

The existing mailbox admission result confirms admission into the HPActor
mailbox. It does not mean that Python application code completed. Failure to
transfer an already-admitted message into the Python dispatch queue is a
separate, observable language-binding failure.

### 5.3 Outbound command flow

Python context methods never call a C++ `ActorContext` from the asyncio thread.

1. A Python handler serializes its protobuf message and constructs a value-only
   `PythonCommand`.
2. The runtime loop attempts to enqueue the command into a bounded SPSC command
   queue and signals a native wakeup.
3. The wakeup schedules `PythonGatewayActor` through HPActor's normal ready
   mechanism.
4. The gateway drains at most `max_commands_per_turn` commands so it cannot
   monopolize a scheduler worker.
5. Operations that need the originating actor's `ActorContext` are forwarded
   as protected internal control messages to that actor's
   `PythonBridgeActor`. The bridge performs send, reply, child spawn, schedule,
   link, monitor, stop, or failure transition on its actor-owned turn.
6. Delivery and request completion produce value-only completion records. The
   Python loop drains those records and resolves Python futures by opaque token.

Command FIFO order is preserved for one Python runtime loop. Protected control
messages may run before user mailbox lanes, as existing HPActor system-message
semantics require, but command sequence numbers preserve order among Python
commands.

### 5.4 No native Python callbacks

The C++ runtime never calls into Python.

- Native queues contain no `PyObject*`.
- Notifiers wake asyncio file-descriptor readers; they do not call Python
  callbacks.
- Python actor instances and handler callables live only on the actor runtime
  loop.
- Public application futures live in Python registries and are resolved by
  token from Python code.
- CPython API calls occur only while Python is already executing a native
  extension entry point with the GIL held.
- Native code never stores borrowed Python references for later use.

This rule is stronger and easier to verify than permitting carefully managed
cross-thread Python callbacks.

## 6. Core Components

### 6.1 Native extension target

`bindings/python/native/` builds `_hpactor` as a separate module linked to
`hpactor_lib`.

- It uses `Py_LIMITED_API` for CPython 3.11 ABI3 compatibility.
- It compiles without exceptions and without RTTI.
- It uses explicit C API error checks and `PyErr_Set*` at Python entry points.
- CPython headers do not appear in `hpactor_lib`, public HPActor headers,
  scheduler code, mailbox code, or transport code.
- C++ failures cross the module boundary as explicit result values; Python
  wrapper code maps selected results to Python exceptions.

### 6.2 `PythonRuntime`

The native `PythonRuntime` owns only language-neutral bridge state:

- bounded dispatch queue;
- bounded command queue;
- bounded completion queue;
- notifier file descriptors;
- actor generation table;
- immutable configuration and snapshots;
- lifecycle state: `Created`, `Starting`, `Running`, `Draining`, `Stopping`,
  `Stopped`, or `Failed`.

It does not own Python actor instances or handler callables.

### 6.3 `PythonBridgeActor`

Each Python actor has one C++ bridge actor and one Python actor object.

The bridge:

- is the real HPActor registry/mailbox/supervision identity;
- preserves the actor address and generation;
- transfers incoming owned envelopes;
- receives protected Python control commands;
- performs originating-actor context operations;
- translates handler failure commands into normal supervision transitions;
- exposes bounded runtime snapshots without reading Python state directly.

The bridge never runs user Python code.

### 6.4 `PythonGatewayActor`

One gateway actor per `PythonRuntime` drains outbound commands and completion
registrations. Its drain budget is fixed by configuration. If work remains, it
requeues itself through `ActorReadyGate` rather than looping indefinitely.

### 6.5 Pure-Python package

`bindings/python/hpactor/` provides:

- `ActorSystem`, `Actor`, `ActorRef`, `ActorAddress`, and `ActorContext`;
- `Behavior` and lifecycle hooks;
- `MessageRegistry`;
- `DeliveryOptions`, `DeliveryReceipt`, and delivery result enums;
- request timeout and cancellation types;
- explicit exception types;
- asyncio marshalling and token-to-future registries;
- type annotations and `py.typed` metadata.

## 7. Python Programming Interface

### 7.1 Message registration

```python
from hpactor import MessageRegistry
from myapp.messages_pb2 import Ping, Pong

messages = MessageRegistry()
messages.register(Ping, type_tag=0x1000)
messages.register(Pong, type_tag=0x1001)
```

Registration is permitted only before the system enters `Running`.

Each entry contains:

- protobuf descriptor full name;
- fixed `TypeTag` at or above the user range;
- descriptor/schema fingerprint;
- Python generated message class.

Duplicate tags, duplicate names with different tags, invalid ranges, or
fingerprint mismatches fail startup before actors receive traffic.

### 7.2 Actor definition

```python
import hpactor
from myapp.messages_pb2 import Ping, Pong


@hpactor.actor("echo")
class Echo(hpactor.Actor):
    def behavior(self) -> hpactor.Behavior:
        return hpactor.Behavior().on_request(Ping, Pong, self.on_ping)

    async def on_ping(
        self, msg: Ping, ctx: hpactor.ActorContext
    ) -> Pong:
        return Pong(text=msg.text)

    async def on_start(self) -> None:
        pass

    async def on_stop(self) -> None:
        pass
```

`Behavior.on(MessageType, handler)` registers fire-and-forget handling.
`Behavior.on_request(RequestType, ResponseType, handler)` serializes and replies
with the returned protobuf. `Actor.become(behavior)` swaps the behavior on the
Python runtime loop and affects subsequent envelopes for that actor.

### 7.3 System use

```python
import hpactor
from myapp.actors import Echo
from myapp.messages_pb2 import Ping, Pong


async def main() -> None:
    async with hpactor.ActorSystem() as system:
        echo = await system.spawn(Echo, name="echo")

        pong = await system.ask(
            echo,
            Ping(text="hello"),
            response_type=Pong,
            timeout=5.0,
        )
        assert pong.text == "hello"
```

The async context manager starts the C++ runtime, dedicated Python loop, bridge
actors, and notifier integration. Exit invokes coordinated drain and shutdown.

### 7.4 Context operations

The first in-process release exposes:

- `ctx.send(target, message, *, options=None) -> DeliveryReceipt`
- `ctx.reply(message, *, options=None) -> DeliveryReceipt`
- `ctx.reply_error(code, detail="") -> DeliveryReceipt`
- `await ctx.ask(target, request, response_type, timeout=...)`
- `await ctx.spawn(ActorClass, *args, name=None, **kwargs)`
- `await ctx.schedule(delay, message, target=None, options=None)`
- `await ctx.cancel_schedule(handle)`
- `await ctx.link(target)` / `await ctx.unlink(target)`
- `await ctx.monitor(target)` / `await ctx.demonitor(target)`
- `await ctx.stop(target)`
- `ctx.passivate()` when the actor supports the existing passivation contract
- `ctx.become(behavior)` through the owning actor

`DeliveryReceipt` is awaitable. Ignoring it gives fire-and-forget behavior;
awaiting it observes the runtime's actual delivery tracking semantics. The
binding does not upgrade best-effort delivery into end-to-end processing
acknowledgement.

## 8. Message and Wire Compatibility

Only generated protobuf messages cross the binding boundary.

Python-to-C++ messages are serialized on the Python runtime loop before native
enqueue. C++-to-Python messages use the existing serialized payload. A local
Python-to-Python send still serializes once so local and remote semantics do not
diverge and no Python object leaks into native queues.

Cross-language messages must use explicit fixed tags. C++ peers use
`HPACTOR_MESSAGE_WITH_ID`. Runtime-allocated `HPACTOR_MESSAGE` tags are valid
only for process-local C++ use and are rejected from cross-language registry
manifests.

The binding does not change the HPActor wire frame or protobuf schema in Phase
1. Any future manifest exchange or schema negotiation belongs to protocol
negotiation work and requires a separate compatibility design.

Binding-only protected control messages use subsystem-owned constants in
`bindings/python/native/python_type_tags.hpp`:

- `kPythonWakeupTag = make_subsystem_tag(0xF0)`;
- `kPythonActorCommandTag = make_subsystem_tag(0xF1)`;
- `kPythonActorFailedTag = make_subsystem_tag(0xF2)`;
- `kPythonInspectTag = make_subsystem_tag(0xF3)`.

They are local runtime controls and are rejected if received from a remote
frame. Architecture tests reserve `0xF0` through `0xF3` and reject duplicate
subsystem tags across compiled HPActor modules.

## 9. Actor Turn and Concurrency Contract

Each Python actor owns a FIFO deque on the Python runtime loop. At most one
handler task is active for that actor.

- Awaiting does not permit another handler for the same actor to run.
- Other Python actors may run while one actor awaits.
- Ask completion resolves the suspended handler directly by token; it does not
  require the same actor to process a response envelope.
- Actor private state is touched only by that actor's handler/lifecycle task on
  the runtime loop.
- CPU-bound Python work must use process-based offload or an external worker;
  it must not block the actor runtime loop.
- Multiple Python processes are the Phase 1 scaling model.

This extends one logical actor turn across `await`. Reentrant actors and
multiple interpreter shards require a later design because they change state
and ordering semantics.

## 10. Capacity and Ownership

All bridge queues have fixed capacities configured before startup.

`PythonDispatchEnvelope` owns:

- actor ID and generation;
- type tag and protobuf bytes;
- sender address;
- message ID and ask ID;
- trace context;
- priority, deadline, delivery flags, and sequence number.

`PythonCommand` owns serialized bytes and value metadata. `PythonCompletion`
owns only result metadata, serialized bytes, and an opaque token.

No queue owns actor state, `PyObject*`, borrowed protobuf memory, or pointers to
stack data. The component that successfully enqueues a value transfers
ownership to the queue consumer. Failed enqueue leaves ownership with the
producer.

## 11. Failure Semantics

The binding adds a canonical `LanguageBinding` failure source. Existing
failure reasons are reused where their semantics fit:

- dispatch or command queue full: `ResourceExhausted`;
- actor generation not found: `NoRoute` or `ActorNotReady`;
- protobuf decode failure: `SerializationError`;
- invalid registry entry: startup/configuration error before runtime traffic;
- deadline exceeded before handler start: `Expired`;
- handler cancellation during shutdown: `ShuttingDown`;
- stale completion after restart: discarded with a stale-generation metric and
  debug log, never delivered to a replacement actor.

`FailureSource::LanguageBinding` is appended as numeric value `12` while every
existing `FailureSource` value retains its current numeric encoding, including
`Unknown = 11`. The implementation makes those values explicit before adding
the new source and updates string conversion and compatibility tests.

An unhandled Python exception becomes a value-only `PythonActorFailed` command
containing exception type name, bounded message text, and a bounded formatted
traceback. The bridge emits a structured failure envelope and lets the existing
supervision policy decide restart, stop, escalation, or quarantine.

`hpactor.ActorError(code, detail)` is an application error reply and does not
fail the actor. Timeout and cancellation exceptions are raised only in Python
after the native result has been converted to an explicit Python-side outcome.
No Python exception crosses a native runtime boundary.

Queue overflow follows configured policy. Command-queue rejection resolves the
new command's receipt with `ResourceExhausted`. Dispatch-queue rejection happens
after HPActor mailbox admission, so it cannot rewrite an already-resolved
mailbox admission receipt; instead it emits a `LanguageBinding` failure
envelope, metric, structured log, and DLQ record when configured.

## 12. Lifecycle, Restart, and Shutdown

### 12.1 Spawn

Imperative spawn creates a bridge actor through `ActorSpawner`, allocates a new
generation, then asks the Python runtime loop to construct the Python object.
The bridge is not externally ready until Python construction and `on_start()`
succeed. Failure rolls back registry publication and bridge state.

### 12.2 Restart

Supervision restart increments the generation, cancels the failed Python task,
destroys the old Python actor object on the runtime loop, and constructs a new
object from the registered Python factory and immutable spawn arguments.
Completions for the old generation are discarded.

Phase 1 does not preserve arbitrary Python object state across restart.
Applications needing recovery must reconstruct state from messages or an
external store until durable actor state is implemented.

### 12.3 Shutdown order

1. Mark Python runtime state `Draining` and reject new Python actor spawns.
2. Stop external ingress through the normal runtime coordinator.
3. Drain bridge actor mailboxes under existing actor drain policies.
4. Stop accepting new dispatch envelopes.
5. Allow active handlers to finish until `handler_shutdown_timeout_ms`.
6. Cancel remaining tasks and resolve their futures as shutting down.
7. Drain or explicitly reject remaining Python commands and completions.
8. Run `on_stop()` and destroy Python actor objects on the runtime loop.
9. Remove asyncio readers, close notifiers, stop the loop, and join the Python
   runtime thread from a non-runtime thread.
10. Destroy gateway and native queue state before CPython interpreter teardown.

No native callback, notifier event, or completion may reference Python state
after step 8.

## 13. Configuration

Configuration uses a subsystem-owned parser and opaque `TomlTableView`.

```toml
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
```

Queue capacities must be powers of two in the inclusive range 64 through
1,048,576. Per-turn drain budgets must be in the inclusive range 1 through
4,096 and cannot exceed their queue capacity. `loop_lag_unready_ms` must be
100 through 60,000. `handler_shutdown_timeout_ms` must be 100 through 300,000.
`max_actor_bindings` must be 1 through 1,048,576. Invalid values fail
configuration before thread creation.

Declarative Python actor topology is deferred. Its later design must preserve
the validated-startup contract: module/class references must be imported and
validated before actor publication, and import failure must roll back startup.

## 14. Observability and Operations

### 14.1 Metrics

The binding emits bounded, out-of-band metrics including:

- `hpactor_python_dispatch_total`
- `hpactor_python_dispatch_rejected_total`
- `hpactor_python_dispatch_queue_depth`
- `hpactor_python_command_total`
- `hpactor_python_command_rejected_total`
- `hpactor_python_command_queue_depth`
- `hpactor_python_handler_duration_seconds`
- `hpactor_python_handler_exceptions_total`
- `hpactor_python_handler_cancelled_total`
- `hpactor_python_event_loop_lag_seconds`
- `hpactor_python_stale_completions_total`

Actor type is an optional bounded label. Actor ID is not a default metric label
because it creates unbounded cardinality.

### 14.2 Logging and tracing

Structured logs preserve actor ID, actor type, generation, message ID, type tag,
trace ID, failure reason, and queue state where available. Tracebacks and error
messages are length bounded.

The existing C++ consumer span covers mailbox-to-bridge execution. A child
`python.actor.handle` span covers dispatch queue wait, protobuf decode, handler
execution, and response serialization. This makes binding delay distinct from
user handler time while continuing the incoming W3C trace.

### 14.3 CLI and health

Phase 1 adds:

- `/python status`
- `/python actors`
- `/python actor <id> inspect`

Inspection is an asynchronous request to the Python runtime loop with a bounded
timeout and bounded serialized response. CLI code never reads Python actor
memory directly.

Readiness is false when Python startup fails, runtime state is not `Running`,
loop heartbeat exceeds `loop_lag_unready_ms`, or shutdown begins. Queue pressure
alone emits pressure signals and metrics; it does not make the whole node
unready unless the runtime loop also fails its heartbeat contract.

## 15. Packaging

The Python distribution uses `pyproject.toml` with scikit-build-core and the
existing CMake project.

- Package name: `hpactor`.
- Native module: `hpactor._hpactor`.
- Python requirement: CPython 3.11 or newer.
- ABI: CPython limited API / ABI3 with a CPython 3.11 floor.
- Platforms: manylinux-compatible Linux x86_64/ARM64 and separate macOS
  x86_64/ARM64 wheels.
- Runtime Python dependency: protobuf, constrained to versions verified by the
  wheel compatibility matrix.
- Native dependencies are bundled or linked according to platform wheel rules;
  wheel repair verifies no unresolved HPActor, protobuf, OpenSSL, or C++ runtime
  dependency remains outside the platform policy.

Wheel import performs no thread creation. Runtime threads begin only when an
`ActorSystem` starts.

## 16. External Client SDK

Phase 2 adds `hpactor.client` as pure Python.

Initial clients are:

- `HealthClient` for `/healthz` and `/readyz`;
- `MetricsClient` for OpenMetrics retrieval;
- `GatewayClient` for application HTTP routes exposed by
  `HTTPGatewayActor`;
- `CliClient` for command execution over UDS or trusted-network TCP.

The client exposes capabilities discovered from configuration or explicit user
construction. Unsupported operations return typed `UnsupportedCapability`
errors. TCP CLI documentation must repeat the current lack of authentication
and recommend UDS or an authenticated proxy.

A client that participates as a native remote HPActor node is deferred until
node identity, authorization, protocol/feature negotiation, and compatibility
contracts are implemented.

## 17. Testing Strategy

### 17.1 Native unit tests

- bounded dispatch, command, and completion queues;
- value ownership on accepted and rejected enqueue;
- notifier coalescing and non-blocking behavior;
- actor generation allocation and stale completion rejection;
- gateway drain budget and requeue;
- runtime lifecycle transitions and shutdown idempotency;
- protobuf registry collision and fingerprint validation;
- explicit native error-to-Python error mapping.

### 17.2 Python unit tests

- behavior dispatch and `become()`;
- fire-and-forget and request handlers;
- delivery receipt await/discard behavior;
- ask timeout and cancellation;
- `ActorError` versus unhandled exception behavior;
- protobuf registration and deterministic serialization;
- async context manager cleanup;
- public type annotations and invalid-lifetime checks.

### 17.3 Integration tests

Deterministic integration tests cover:

- Python to C++ and C++ to Python send/reply;
- Python to Python ordering;
- local ask without response-envelope deadlock;
- scheduled delivery and cancellation;
- link, monitor, stop, restart, and quarantine;
- trace continuation and structured failure metadata;
- dispatch and command overflow with DLQ accounting;
- runtime startup rollback;
- graceful and timed-out shutdown;
- stale completion after restart.

Tests use paused workers, explicit scheduler stepping, notifier drains, and
condition-based waits. Sleeps are not proof of progress.

### 17.4 Stress and architecture tests

- Concurrent C++ producers target Python actors while one Python consumer
  drains the MPSC dispatch queue.
- Every attempted message is accounted as handled, rejected, cancelled, or
  dead-lettered.
- Queue depth never exceeds configured capacity.
- No actor executes two handlers concurrently.
- No completion resolves a replacement generation.
- Shutdown under load produces no callback or file-descriptor use after
  teardown.
- Architecture checks forbid CPython headers/API calls outside
  `bindings/python`, forbid `PyObject*` fields in native queues, forbid
  exception/RTTI flags, and forbid unbounded bridge containers.
- Sanitizer and race-oriented lanes cover the native bridge; Python tests run
  with debug builds and reference-leak checks where supported.

### 17.5 Wheel and performance tests

Each wheel target installs into a clean environment, imports `hpactor`, runs a
local echo actor, performs ask/reply, and shuts down cleanly.

Performance CI records empty-handler throughput, dispatch queue wait, handler
latency, and end-to-end p50/p95/p99. Once the first stable baseline is stored,
the same-runner gate rejects regressions greater than 20 percent. Performance
numbers are not compared across different hardware runners.

## 18. Acceptance Criteria

The in-process binding is ready when all of the following are proven:

1. Fixed Python and C++ `TypeTag` registrations interoperate locally and over
   the existing loopback network path.
2. Python actor state is never handled concurrently or accessed from C++.
3. HPActor scheduler workers never execute CPython API calls or wait for the
   GIL.
4. Cross-thread native storage contains no Python object pointers.
5. All bridge queues remain within configured bounds under stress.
6. Every overflow path produces typed failure accounting and optional DLQ
   evidence.
7. Ask, receipt, timeout, cancellation, restart, and stale-generation behavior
   are deterministic.
8. Traces, logs, metrics, CLI snapshots, health, and readiness expose binding
   state without reading Python actor memory directly.
9. Shutdown under active load joins the runtime thread and leaves no late
   completion, notifier event, actor object, or Python reference.
10. ABI3 wheels pass clean-environment smoke tests on every supported platform
    and architecture.

## 19. Delivery Phases

### Phase 1A: Native bridge foundation

Build value envelopes, bounded queues, notifiers, runtime lifecycle, gateway,
bridge actor, native snapshots, and architecture tests.

### Phase 1B: Python actor API

Add protobuf registry, actor/behavior API, imperative spawn, send/reply/ask,
delivery receipts, scheduling, lifecycle hooks, and Python tests.

### Phase 1C: Reliability and operations

Add supervision/restart, link/monitor, overflow/DLQ integration, tracing,
metrics, logs, CLI, health/readiness, stress tests, and shutdown hardening.

### Phase 1D: Packaging

Add ABI3 wheel builds, platform repair, clean-environment smoke tests,
documentation, and examples mirroring the developer manual.

### Phase 1E: Declarative topology follow-up

Design and implement validated module/class discovery and Python actor factory
registration for TOML topology without weakening startup rollback.

### Phase 2: External SDK

Add supported HTTP, health, metrics, and CLI clients. Native remote-node
participation remains gated on security and protocol negotiation.

### Planning boundary

This document is the umbrella architecture requested for both in-process and
external Python use. Implementation is deliberately split into independently
reviewable plans. The first detailed implementation plan covers Phase 1A only.
Phases 1B through 1E each receive a separate plan after their predecessor is
verified. Phase 2 is an independent deliverable and requires its own focused
specification and plan before code changes.

## 20. Alternatives Rejected

### Direct Python subclass trampoline

Calling Python handlers from `EventBasedActor::receive()` is simpler but would
place GIL acquisition, Python latency, and accidental blocking on cooperative
scheduler workers. It violates the production concurrency direction.

### Sidecar-only actors

An out-of-process sidecar gives strong isolation but is not an in-process
language binding, adds serialization/network latency to every local message,
and depends more heavily on unfinished identity and protocol-negotiation work.

### Multiple interpreter shards

Subinterpreters could increase future parallelism but complicate extension
compatibility, actor placement, object ownership, and failure isolation. Phase
1 scales Python work with multiple processes and reserves interpreter sharding
for a separate design.

### Nanobind or pybind11

Both offer productive C++ wrappers, but their normal binding model relies on
C++ exception support. The repository's exception allowlist excludes binding
translation units. A narrow CPython limited-API module preserves the stronger
project-wide contract and avoids wrapper ABI coupling.
