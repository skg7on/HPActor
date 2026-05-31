# EdgeOps Telemetry Platform Design

## 1. Overview

Design a production-like IoT edge telemetry application that validates HPActor
as an operational actor runtime. The application is called **EdgeOps Telemetry
Platform**. It models same-host multi-process IoT telemetry ingestion,
processing, alerting, storage, and operator inspection.

The primary goal is not to create a synthetic benchmark. The goal is to build a
credible application whose normal workflows exercise finished HPActor
functionality:

- actor lifetime and supervision
- local and remote message communication
- actor finding, registration, and location-transparent routing
- scheduler timers, worker dispatch, and slow-consumer behavior
- bounded mailboxes, backpressure, failure envelopes, and DLQ handling
- memory region accounting under actor churn and buffering
- networking and service discovery in a same-host multi-process deployment
- metrics, structured logs, distributed tracing, and CLI operations
- graceful drain and shutdown
- deterministic fault-injection drills where current hooks support it

This design starts from `CLAUDE.md`, `CLAUDE_MEMORY.md`, and the architecture
docs under `docs/architecture/`. The production reliability docs remain the
roadmap source of truth: features documented there but not implemented in the
runtime are treated as future extensions, not required behavior for this app.

## 2. Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Domain | IoT edge telemetry | Naturally stresses actor count, registration, routing, scheduling, memory pressure, networking, and operations. |
| Deployment target | Multi-process same host | Validates registrar, TCP/UDS paths, remote actor addresses, and process role boundaries without requiring a full cluster lab. |
| Primary proof | Operational maturity | Demonstrates that humans can inspect, diagnose, drain, and stop the system while it is alive. |
| Application style | Production-like demo app | Keeps the scenario realistic instead of making every path feel like a benchmark harness. |
| First version scope | Finished runtime features only | Avoids claiming durable messaging, sharding, authenticated admin APIs, or multi-node partition handling before runtime support exists. |
| Configuration | CLI flags plus optional TOML topology | Matches current examples while leaving room for topology bootstrap validation. |
| Observability | Metrics, logs, traces, DLQ, CLI | Turns failures and runtime state into operator-visible evidence. |

## 3. Goals

1. Provide one application that showcases HPActor's actor model, scheduler,
   mailbox, memory, networking, discovery, observability, CLI, and shutdown
   features together.
2. Run as a same-host multi-process topology with separate role processes.
3. Validate local and remote actor communication through realistic telemetry
   workflows.
4. Make operational state inspectable while the app is running.
5. Include built-in scenarios for happy path, device churn, malformed telemetry,
   overload, missing route, timer rollup, graceful shutdown, and deterministic
   fault injection.
6. Produce clear acceptance evidence through unit, integration, and system
   tests.
7. Keep unsupported production-roadmap features explicit.

## 4. Non-Goals

- Durable outbox/inbox, ACK/NACK retry completion, or process-restart replay.
- Cluster sharding, shard placement, rebalance, or multi-node split-brain tests.
- Authenticated admin API, mTLS authorization, or audit enforcement.
- Hosted UI or dashboard product.
- Real device protocol support such as MQTT, CoAP, Modbus, or OPC UA.
- Persisted database storage; storage is in-memory for the first version.
- AI runtime, accelerator, or MLX integration.

## 5. Runtime Topology

The application has five role processes. Each role is a mode of one executable
so the example remains easy to build and run.

```text
device-simulator process
  creates simulated devices and telemetry batches

gateway process
  accepts telemetry, owns device sessions, routes to processors

processor process
  normalizes readings, aggregates windows, evaluates alert rules

storage process
  stores recent readings and rollups in bounded in-memory buffers

ops process
  attaches operator scenarios, queries state, and drives drain/stop drills
```

Default same-host ports should avoid common system services:

- registrar: `19154`
- gateway actor transport: `17230`
- processor actor transport: `17231`
- storage actor transport: `17232`
- ops actor transport: `17233`
- optional HTTP metrics gateway: role-specific `18230` through `18233`

The first version should support an `--all-in-one` mode for fast local
debugging, but the primary validation path is multi-process same host.

## 6. Actor Model

### 6.1 Gateway Role

| Actor | Responsibility | HPActor features exercised |
|-------|----------------|----------------------------|
| `TelemetryGatewayActor` | Ingress boundary, envelope validation, routing to registry/router | local and remote send, trace propagation, structured failures |
| `DeviceRegistryActor` | Device id to actor/address map, registration lifecycle | actor finding, registration, stateful actor, CLI metadata |
| `DeviceSessionActor` | One logical device session, online/offline/reconnect state | lifecycle, timers, supervision, memory accounting |
| `FleetSupervisorActor` | Supervises device sessions and gateway workers | one-for-one style restart, failure visibility |

### 6.2 Simulator Role

| Actor | Responsibility | HPActor features exercised |
|-------|----------------|----------------------------|
| `DeviceSimulatorRootActor` | Starts and stops simulated device groups | actor spawning, graceful shutdown |
| `SimulatedDeviceActor` | Emits heartbeats and sensor readings | timers, message pressure, actor churn |
| `ScenarioDriverActor` | Runs named scenarios and prints concise summaries | command-line workflow, future automation hook |

### 6.3 Processor Role

| Actor | Responsibility | HPActor features exercised |
|-------|----------------|----------------------------|
| `TelemetryRouterActor` | Partitions readings by site, sensor class, or device id | actor lookup cache, routing, remote delivery |
| `NormalizerActor` | Validates payloads, units, timestamps, and sequence numbers | failure envelopes, DLQ for malformed input |
| `WindowAggregatorActor` | Periodic rollups by device/site/sensor | scheduler timers, mixed workload scheduling |
| `AlertRuleActor` | Threshold and rate-of-change alerts | request/reply, tracing, structured logs |
| `ProcessorSupervisorActor` | Supervises normalizer, aggregator, and alert actors | restart behavior and CLI-visible lifecycle |

### 6.4 Storage Role

| Actor | Responsibility | HPActor features exercised |
|-------|----------------|----------------------------|
| `StorageSinkActor` | Bounded in-memory store for recent readings and rollups | bounded mailbox, pressure, memory counters |
| `RollupQueryActor` | Query recent rollups and alerts | request/response, actor communication |
| `StoragePressureActor` | Deliberately slows storage in overload scenarios | backpressure, DLQ, scheduler fairness |

### 6.5 Ops Role

| Actor | Responsibility | HPActor features exercised |
|-------|----------------|----------------------------|
| `OpsScenarioActor` | Starts operational drills across roles | remote messaging, scenario orchestration |
| `OpsQueryActor` | Queries actor state, DLQ, metrics, and summaries | CLI patterns and request/response |
| `OpsDrainActor` | Coordinates graceful stop order | shutdown protocol and drain policy |

## 7. Message Contracts

The application should define explicit TypeTag assignments in an
`examples/edgeops_telemetry/messages.hpp` header. Payloads can use the existing
lightweight hand-rolled binary style from `examples/order_platform/messages.hpp`
for consistency with current examples.

Core messages:

| Message | Direction | Purpose |
|---------|-----------|---------|
| `DeviceRegister` | simulator -> gateway | Create or refresh a device session. |
| `DeviceRegistered` | gateway -> simulator | Confirm assigned session actor and sequence baseline. |
| `DeviceHeartbeat` | simulator -> gateway | Keep session active and exercise timers. |
| `TelemetryReading` | simulator -> gateway -> processor | Device reading with site, sensor, sequence, timestamp, and value. |
| `TelemetryRejected` | processor/gateway -> sender | Structured rejection projection for malformed input. |
| `NormalizedReading` | normalizer -> aggregator/storage | Validated and normalized reading. |
| `WindowRollup` | aggregator -> storage/ops | Periodic aggregate summary. |
| `AlertRaised` | alert actor -> storage/ops | Threshold or rate alert. |
| `DeviceDisconnected` | simulator/gateway -> registry | End or pause a session. |
| `QueryDevice` | ops -> registry | Inspect one device session. |
| `QueryFleetSummary` | ops -> gateway/processor/storage | Aggregate health and counters. |
| `ScenarioCommand` | ops/simulator -> role root | Start a named scenario. |
| `DrainRole` | ops -> role root | Start graceful drain for a role. |

Each telemetry reading carries:

- `device_id`
- `site_id`
- `sensor_type`
- `sequence`
- `timestamp_ns`
- `reading_value`
- `quality_flags`
- scenario marker for deterministic tests

Trace context remains an HPActor envelope concern, not a user payload field.
Message IDs and sequence numbers provide application-level correlation for
operator output.

## 8. Data Flow

### 8.1 Happy Path

```text
SimulatedDeviceActor
  -> TelemetryGatewayActor
  -> DeviceRegistryActor lookup
  -> TelemetryRouterActor
  -> NormalizerActor
  -> WindowAggregatorActor
  -> AlertRuleActor
  -> StorageSinkActor
```

Expected behavior:

1. Device registers and receives confirmation.
2. Gateway creates or refreshes `DeviceSessionActor`.
3. Gateway forwards readings to processor role by actor address.
4. Processor validates and normalizes readings.
5. Aggregator emits periodic rollups via scheduler timers.
6. Alert actor emits alerts when thresholds match.
7. Storage stores readings, rollups, and alerts in bounded in-memory buffers.
8. Ops role can query fleet/device/storage state.

### 8.2 Failure and Operations Paths

| Scenario | Behavior | Runtime capability validated |
|----------|----------|------------------------------|
| Device churn | Devices register, disconnect, reconnect, and continue sequence tracking | lifecycle, registry, actor churn, memory counters |
| Malformed telemetry | Bad units, stale timestamps, or truncated payloads are rejected | structured failures, DLQ, logs, traces |
| Overload | Storage slows down and uses a tiny bounded mailbox | backpressure, DLQ, mailbox metrics |
| Missing route | Gateway routes to absent actor id | no-route failure, failure summary, DLQ |
| Timer rollup | Aggregator emits windows on schedule | timing wheel, scheduler metrics |
| Worker restart | Processor actor fails and supervisor restarts it | supervision, lifecycle logs, metrics |
| Graceful shutdown | Simulator stops first, then gateway drains, processor drains, storage flushes summaries | drain policy, shutdown ordering |
| Fault injection | Scheduled mailbox, transport, scheduler, or allocator faults fire where hooks exist | deterministic fault visibility |

## 9. Operational Surface

The app should be useful while it is alive.

### 9.1 CLI Interaction

The app should use existing CLI commands where available and add application
state through actor metadata and inspect replies. New demo commands can be
role-local wrappers if the generic CLI command tree is not yet extensible
enough for application-specific verbs.

Operator workflows:

```text
/actor list
/actor <id> show
/system stats
/system memory
/failure reasons
/failure summary
/fault status
/fault list
/system drain
/system stop
```

Application-specific query mode should support:

```text
edgeops_telemetry --query --device device-001
edgeops_telemetry --query --fleet
edgeops_telemetry --query --alerts
edgeops_telemetry --query --storage
```

### 9.2 Metrics

The app should expose or trigger existing metric families:

- actor spawned and terminated counts
- actor lifecycle transitions
- mailbox depth and enqueue/dequeue counters
- message processing latency
- scheduler dispatch and steal counters
- memory active bytes by actor or region
- delivery failures by reason/source
- DLQ depth and pushed/lost counters
- fault injection events
- application counters for device registrations, readings, rollups, alerts,
  malformed readings, and query replies

### 9.3 Logs

Structured log events should include:

- device registration and reconnect
- malformed telemetry rejection
- route miss
- mailbox pressure decision
- alert raised
- processor restart
- lifecycle transition
- drain phase start and completion
- fault fired

Every log should carry enough correlation fields where available:

- actor id
- device id
- message id or sequence
- trace id
- scenario name
- failure reason

### 9.4 Tracing

The app should create one trace for a telemetry batch or ingress request.
Propagation should cross gateway, processor, alert, and storage actors. The
trace should make it possible to identify whether latency came from gateway
routing, normalizer processing, aggregation, alert evaluation, storage pressure,
or remote process delivery.

### 9.5 DLQ and Failure Review

The application should push inspectable DLQ records for:

- malformed telemetry
- missing actor route
- mailbox overflow
- drain policy drop
- fault-induced delivery failure

Failure summaries should use HPActor's canonical failure reason vocabulary
instead of application-only strings.

## 10. Capability Validation Matrix

| Capability area | Validation in EdgeOps |
|-----------------|-----------------------|
| Actor lifetime manager | device online/offline/reconnect, supervised processor restart, drain states |
| Scheduling | periodic rollups, mixed priority readings, slow storage consumer, scheduler metrics |
| Memory management | actor churn, storage buffering, bounded queues, memory region counters |
| Message communication | local sends, remote sends, replies, query actors, malformed payload rejection |
| Networking | same-host multi-process transport, UDS/TCP where supported |
| Actor finding and registration | device registry, registrar discovery, remote actor addresses, actor location cache |
| Mailbox/backpressure | bounded storage mailbox, overflow policies, pressure logs, DLQ routing |
| Failure semantics | structured failure envelopes, failure summary, retryable flags where available |
| Observability | metrics, logs, traces, DLQ, CLI inspection |
| CLI interaction | role startup flags, query mode, CLI inspection, drain/stop commands |
| Maturity | graceful shutdown, deterministic fault drills, clear unsupported feature boundaries |
| Performance | scenario counters, latency histograms, mailbox pressure trend, throughput summary |

## 11. Command-Line Shape

One executable should support all roles:

```text
edgeops_telemetry --gateway --actor-port 17230 --registrar-port 19154
edgeops_telemetry --processor --actor-port 17231 --registrar-port 19154
edgeops_telemetry --storage --actor-port 17232 --registrar-port 19154
edgeops_telemetry --ops --actor-port 17233 --registrar-port 19154
edgeops_telemetry --device-simulator --devices 1000 --rate 100 --scenario happy-path
```

Fast local mode:

```text
edgeops_telemetry --all-in-one --scenario happy-path
edgeops_telemetry --all-in-one --scenario overload
edgeops_telemetry --all-in-one --scenario missing-route
```

Query mode:

```text
edgeops_telemetry --query --fleet
edgeops_telemetry --query --device device-001
edgeops_telemetry --query --alerts
edgeops_telemetry --query --storage
```

Scenario names:

- `happy-path`
- `device-churn`
- `malformed-telemetry`
- `overload`
- `missing-route`
- `timer-rollup`
- `processor-restart`
- `graceful-shutdown`
- `fault-injection`

## 12. Configuration

The first version can use CLI flags for simplicity. A follow-up should add TOML
topology examples to validate the topology parser and actor factory registry.

Recommended TOML example:

```toml
[system]
enable_network = true
tcp_port = 17230

[system.metrics]
enabled = true
metrics_path = "/metrics"

[system.tracing]
enabled = true
sampling_rate = 1.0

[system.logging]
enabled = true
default_level = "info"

[system.cli]
enabled = true
default_format = "pretty"

[system.shutdown]
drain_timeout_ms = 5000

[[actor]]
id = "gateway"
behavior = "TelemetryGatewayActor"

[[actor]]
id = "registry"
behavior = "DeviceRegistryActor"

[[actor]]
id = "storage"
behavior = "StorageSinkActor"
mailbox_capacity = 256

[actor.mailbox]
policy = "dead_letter"
```

For same-host registrar discovery, leaving `[system.discovery]` absent keeps
the default UdpRegistrar path when networking is enabled. Explicit
`backend = "gossip"`, `"static"`, or `"hybrid"` should be reserved for the
multi-node or static-route follow-ups. Any application-specific TOML fields
should be added through self-registering subsystem parsers instead of expanding
a central parser.

## 13. Tests and Acceptance Evidence

### 13.1 Unit Tests

- message encode/decode round trips
- malformed payload rejection
- scenario flag parsing
- device registry insert/update/remove
- rollup math
- alert threshold and rate rules
- bounded storage buffer eviction

### 13.2 Integration Tests

- single-process happy path across gateway, processor, and storage actors
- device churn with session lifecycle state changes
- malformed telemetry produces structured failure and DLQ record
- overload pushes records to DLQ and exposes pressure state
- query actor returns device, fleet, alert, and storage summaries
- graceful drain completes with finite in-flight work

### 13.3 System Tests

- same-host multi-process happy path using registrar discovery
- remote processor or storage route lookup through actor address
- missing route from gateway to absent actor id
- overload with slow storage consumer
- drain ordering across simulator, gateway, processor, storage, and ops roles
- observability snapshot includes metrics, logs, traces, DLQ, and CLI-readable
  state

### 13.4 Acceptance Criteria

The design is complete when the implementation can demonstrate:

1. A multi-process same-host telemetry flow from simulator to storage.
2. Operator inspection of device state, actor state, mailbox pressure, DLQ, and
   failure summaries.
3. Metrics and logs changing during normal and failure scenarios.
4. Trace correlation across at least gateway, processor, and storage hops.
5. Bounded mailbox overload that avoids unbounded memory growth.
6. Graceful shutdown that drains finite work and reports dropped work when a
   configured policy drops it.
7. Focused tests for functional behavior and system-level smoke coverage for
   multi-process operation.

## 14. Backlog Boundaries and Future Extensions

The first version must not claim the following as implemented validation:

- durable reliable messaging with ACK/NACK and persisted outbox/inbox
- process restart recovery from durable state
- cluster sharding, placement, and rebalance
- multi-node partition handling or fencing
- authenticated admin API and authorization policy
- security audit enforcement
- hosted UI dashboards
- real device protocol adapters
- AI runtime or accelerator orchestration

Future extensions:

- add a multi-node gossip or hybrid-discovery deployment
- add application-specific CLI commands through a plugin-style command registry
- add durable storage adapter once durable actor state is implemented
- add authenticated admin API scenarios after security architecture lands
- add long-running soak tests for actor churn, memory pressure, and latency
- add topology-driven multi-role startup through TOML and AOT binary topology

## 15. Implementation Placement

Recommended file layout:

```text
examples/
  14_edgeops_telemetry.cpp
  edgeops_telemetry/
    messages.hpp
    scenario.hpp
    scenario.cpp
    rollup.hpp
    alert_rules.hpp

tests/unit/examples/
  test_edgeops_messages.cpp
  test_edgeops_rollup.cpp
  test_edgeops_alert_rules.cpp

tests/integration/examples/
  test_edgeops_single_process.cpp

tests/system/
  test_system_edgeops_telemetry.cpp
```

The example should avoid becoming one oversized source file. Shared message
codec, scenario parsing, rollup math, and alert rules should live in small
headers or helper sources that tests can include without launching the full app.

## 16. Review Checklist

- The design is grounded in current finished runtime features.
- Backlog-only production features are identified as future extensions.
- The first deployment target is same-host multi-process.
- Operational maturity is the primary proof.
- The app remains realistic enough to be a product-like showcase.
- Test coverage spans unit, integration, and system tiers.
