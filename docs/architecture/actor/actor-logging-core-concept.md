# Actor Logging Subsystem - Core Concept

## Overview

HPActor's logging subsystem provides a unified, structured API for developers
and framework code to record the events they decide are important. Metrics
answer "how much" and "how often"; logs answer "what happened here" at an
explicitly instrumented point. The subsystem is designed for production
observability first, with developer debug and trace logging layered on top.

The core idea is simple: developers place `HPACTOR_LOG_*` macros or call the
logging API at selected actor, mailbox, scheduler, memory, registrar, network,
or application points. Those calls emit compact log events into a lock-free ring
buffer, and one asynchronous drain formats the accepted events into configured
sinks such as stderr, JSON files, rotating files, or test memory sinks.

**Core Principle:** logging must never become part of actor progress. A disabled
log statement is a cheap level/category check. An enabled hot-path log is a
bounded ring-buffer write. Formatting, file I/O, and sink failure handling occur
outside actor execution.

## Problem Statement

HPActor already has strong operational counters through actor metrics, but
developers still need a consistent way to record richer contextual events at the
points they choose:

1. Which actor changed from `Ready` to `Running`, and on which worker?
2. Why did a registrar lookup fail for a specific endpoint?
3. Which network frame failed to decode, with which type tag and byte count?
4. Which mailbox crossed a warning threshold before latency spiked?
5. Which allocator operation detected corruption, and which actor owned it?

Ad hoc `printf`, `std::cout`, and `std::cerr` calls do not solve this cleanly.
They block, allocate unpredictably, lack categories, cannot be filtered at
runtime, and are difficult to feed into production log systems.

## Design Philosophy

### Developer-Directed Instrumentation

The logging subsystem provides mechanism, not policy. It does not infer every
actor state change, memory operation, scheduler decision, mailbox event, or
network packet by itself. Developers decide which events are meaningful and add
explicit log calls at those points.

HPActor framework code can ship with useful built-in instrumentation, but those
logs follow the same public API that applications use. This keeps the model
predictable: if a log appears, some code intentionally emitted it.

### Structured Before Textual

Human-readable text is only one formatter. The internal event model should keep
stable structure:

- severity level
- category
- actor id
- worker id
- message type tag
- trace/span ids when present
- event id
- bounded typed fields
- source location

Text output is for humans. JSON output is for log pipelines. Both should be
derived from the same `LogEvent`.

### Runtime Filtering Is Essential

HPActor has very hot paths: mailbox enqueue/dequeue, actor dispatch, memory
allocation, network packet processing, and scheduler work stealing. The logging
API must support per-category thresholds so production can run with:

```toml
[system.logging]
enabled = true
default_level = "info"

[system.logging.levels]
actor = "info"
mailbox = "warning"
memory = "warning"
network = "warning"
registrar = "info"
actor_state = "debug"
```

This lets operators turn up `network = "trace"` during an incident without
flooding actor lifecycle or memory logs.

### Logs Are Not Metrics

Metrics are aggregated signals. Logs are individual events.

| Question | Correct Tool |
|----------|--------------|
| "What is the current mailbox depth?" | Metric |
| "Which actor's mailbox crossed depth 2048?" | Log |
| "How many bytes are active per actor?" | Metric |
| "Which allocation found a canary mismatch?" | Log |
| "How many registrar misses per minute?" | Metric |
| "Which endpoint failed resolution?" | Log |

The two systems should share ideas and infrastructure where useful, but they
serve different operational jobs.

## Architecture Overview

```text
Developer-chosen instrumentation point
  mailbox, scheduler, actor, memory, registrar, network
        |
        | HPACTOR_LOG_* macro or Logger::emit()
        v
Runtime filter
  enabled, category threshold, optional sampling
        |
        | fixed-size event write
        v
LogRingBuffer
  lock-free MPSC, bounded, records dropped events
        |
        | single async consumer
        v
LogDrain
  batches events, formats outside hot paths
        |
        v
LogSink
  stderr, file, rotating file, memory sink, future CLI tail
```

### Ownership Model

```text
ActorSystem
  owns LogManager
    owns LogConfig
    owns LogRingBuffer
    owns LogDrain
      owns formatters
      owns sinks

ActorSystem::spawn()
  passes Logger pointer to actors, mailboxes, scheduler, and subsystems

Instrumentation code
  holds Logger* or uses log::global_logger()
  emits only if runtime filter accepts the event
```

The logger is a system service, not a normal user actor. A future `LogActor` can
provide control-plane operations, but the data path should not depend on actor
mailboxes because the subsystem also logs mailbox and scheduler behavior.

## Log Levels

Severity order:

```text
critical > error > warning > info > debug > trace
```

| Level | Meaning |
|-------|---------|
| `critical` | System integrity is at risk. Examples: allocator corruption, fatal event-loop failure. |
| `error` | Operation failed, but the process can continue. Examples: frame decode failure, registrar request failure. |
| `warning` | Abnormal but recoverable condition. Examples: mailbox depth high, reconnect retry, cache miss after stale route. |
| `info` | Important lifecycle events. Examples: actor spawned, topology loaded, node joined. |
| `debug` | Developer-visible state transitions. Examples: actor state transfer, scheduler dispatch details. |
| `trace` | High-volume detail. Examples: packet bytes, every enqueue/dequeue, every malloc/free. |

`trace` must be off by default and safe to enable only for specific categories.

## Log Categories

The first version should use a fixed enum to avoid string hashing or dynamic
allocation on hot paths:

```cpp
enum class LogCategory : uint16_t {
    kActor,
    kActorState,
    kMailbox,
    kScheduler,
    kMemory,
    kRegistrar,
    kDiscovery,
    kNetwork,
    kRpc,
    kConfig,
    kSupervision,
    kCli,
    kHttp,
    kUser,
};
```

Applications can use `kUser` at first. A later version can add user category
registration if the framework needs stable application-specific filtering.

## Event Model

Hot path events should be compact and copyable. The event should carry enough
data for production debugging without forcing string formatting in the producer:

```cpp
struct LogEvent {
    uint64_t timestamp_ns;
    LogLevel level;
    LogCategory category;
    ActorId actor_id;
    uint64_t trace_id;
    uint64_t span_id;
    uint32_t worker_id;
    uint32_t type_tag;
    uint32_t event_id;
    uint32_t line;
    const char* file;
    const char* message;
    LogField fields[4];
    uint8_t field_count;
};
```

The `message`, `file`, and field names should normally be string literals.
Dynamic text is allowed through a slower bounded API that truncates into an
inline buffer, but framework hot paths should prefer event ids plus typed
fields.

## Public API Shape

The preferred API is macro-based so source location and disabled-path checks are
automatic:

```cpp
HPACTOR_LOG_INFO(LogCategory::kActor, actor_id,
                 "actor spawned", hp_log_field("type_tag", type_tag));

HPACTOR_LOG_WARNING(LogCategory::kMailbox, actor_id,
                    "mailbox depth high", hp_log_field("depth", depth));

HPACTOR_LOG_TRACE(LogCategory::kNetwork, ActorId{},
                  "packet received", hp_log_field("bytes", bytes));
```

The underlying logger also exposes an explicit API for framework code and tests:

```cpp
log::global_logger().emit(level, category, actor_id, event_id, message, fields);
```

## Suggested Instrumentation Map

The table below lists useful framework and application call sites. It is a
guide for where developers may choose to add log calls, not a mandate that the
logging subsystem automatically records every item.

| Area | Default Level | Examples |
|------|---------------|----------|
| Actor lifecycle | `info` | spawn, terminate, link or monitor rejected |
| Actor state transfer | `debug` | `Idle -> Ready`, `Ready -> Running`, `Running -> IOWaiting` |
| Mailbox state | `warning`, `trace` | depth above threshold, enqueue/dequeue trace |
| Memory operation | `warning`, `error`, `trace` | alloc/free trace, canary mismatch, guard page fault |
| Registrar | `info`, `warning`, `error` | register, heartbeat timeout, resolve miss |
| Service discovery | `info`, `warning` | node join, node suspect, node dead |
| Network packet processing | `debug`, `trace`, `error` | frame send/receive, decode failure, connection close |
| Scheduler | `debug`, `trace` | worker dispatch, work steal, timer firing |
| Config/bootstrap | `info`, `error` | topology loaded, validation failure |
| Supervision | `warning`, `error` | child failure, restart, escalation |

## Sink Model

The first version should include:

- `StderrSink` for local development and container logs
- `FileSink` for simple persistent logs
- `RotatingFileSink` for long-running processes
- `MemorySink` for tests

Each sink receives already formatted bytes from `LogDrain`, so sink
implementations remain simple. Formatting should support:

- compact text
- JSON lines

Future sinks can add syslog, journald, HTTP, or OpenTelemetry export without
changing producer code.

## Failure Semantics

Logging must never crash or block the actor system.

- If the ring buffer is full, increment `logs_dropped_total`.
- If a sink fails, write one throttled diagnostic to stderr and disable that
  sink or retry with backoff.
- If formatting fails, emit a fallback text line with event id and level.
- If logging is invoked from allocator corruption or signal-sensitive paths,
  use an emergency writer that avoids normal locks and allocation.

For `critical` and optionally `error`, the drain can be nudged to flush quickly,
but producers still must not block indefinitely.

## Relationship to Existing Components

| Existing Component | Role |
|-------------------|------|
| `ActorSystem::Config` | Gains `log::LogConfig logging` |
| TOML parser | Parses `[system.logging]` and `[system.logging.levels]` |
| Metrics ring buffer | Pattern reused for `LogRingBuffer` |
| Memory telemetry | Provides allocation event vocabulary and corruption points |
| `TraceContext` | Supplies trace and span ids when available |
| CLI | Future `/logs tail`, `/logs level`, `/logs sinks` control plane |
| Metrics | Exposes dropped log events and sink error counters |

## Operational Defaults

Recommended defaults:

```toml
[system.logging]
enabled = true
default_level = "info"
format = "json"
sinks = ["stderr"]
ring_buffer_capacity = 65536
drop_policy = "drop_newest"
flush_on_level = "error"

[system.logging.levels]
mailbox = "warning"
memory = "warning"
network = "warning"
actor_state = "warning"
```

This gives useful production logs without flooding the process. Debug and trace
detail remains available by category.

## Review Note

This document is written as the high-level architecture core concept requested
for code review. The dedicated `code-review` plugin is not available in this
session, so the review artifact is saved as a normal repository document.
