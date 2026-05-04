# Actor Metrics — Core Concept and Architecture Design

## 1. Executive Summary

This document specifies an actor-level metrics subsystem for the HPActor C++20 framework. Metrics on mailbox queue sizes, message processing latency, actor lifecycle events, scheduler activity, and memory allocation are natively exposed to telemetry pipelines (Prometheus, Grafana) via an HTTP `/metrics` endpoint serving OpenMetrics format.

**Key Design Decisions:**
- **Out-of-band instrumentation**: The mailbox, scheduler, and lifecycle hooks emit fixed-size events to a lock-free ring buffer. A dedicated `MetricsActor` drains, aggregates, and formats them. Zero per-message overhead on the actor hot path — the actor itself writes nothing.
- **Reuse proven patterns**: The ring buffer design is the same CAS-based MPSC architecture as the existing `TelemetryRingBuffer` (`mem/telemetry_ring_buffer.hpp`). No new synchronization primitives, no new dependencies.
- **OpenMetrics wire format**: Prometheus's successor format (`application/openmetrics-text; version=1.0.0`) with structured `# HELP`/`# TYPE` metadata, exponential histogram bucketing, and `_total` counter suffixes. Served by the existing `HTTPGatewayActor` route registry.
- **Pull model**: Prometheus scrapes `/metrics` on its scrape interval. The `MetricsActor` drains the ring buffer and snapshots aggregators on each request, then replies synchronously.

---

## 2. Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                   Instrumentation Points                      │
│ (mailbox enqueue, scheduler dispatch, lifecycle hooks, etc.)  │
│          │ atomic CAS write (no allocations)                  │
│          ▼                                                    │
│  ┌─────────────────┐    drain       ┌────────────────────┐   │
│  │ MetricsRingBuffer│─────────────▶│  MetricsAggregator  │   │
│  │ (lock-free, MPSC,│  (callback)  │ (Counter/Gauge/     │   │
│  │  65536 events)   │              │  Histogram per      │   │
│  └─────────────────┘               │  label set)         │   │
│                                     └────────┬───────────┘   │
│                                              │ snapshot       │
│                                              ▼                │
│                                     ┌────────────────────┐   │
│                                     │ OpenMetricsFormatter│   │
│                                     │ (serializes to text)│   │
│                                     └────────┬───────────┘   │
│                                              │                │
│  ┌──────────────────────┐  GET /metrics      │                │
│  │   HTTPGatewayActor   │◀───────────────────┘                │
│  │  route("/metrics")   │                                     │
│  └──────────┬───────────┘                                     │
│             │ send MetricsRequest                              │
│             ▼                                                  │
│  ┌──────────────────────┐                                     │
│  │    MetricsActor        │  (EventBasedActor)                 │
│  │  drains ring buffer,   │                                    │
│  │  builds OpenMetrics    │                                    │
│  └──────────────────────┘                                     │
└──────────────────────────────────────────────────────────────┘
```

### Ownership Chain

```
ActorSystem
  ├── owns MetricsRingBuffer (shared_ptr, passed to components)
  ├── owns MetricsActor (spawned as system actor, before topology)
  │     ├── owns MetricRegistry (counters, gauges, histograms)
  │     ├── owns Aggregator (event → metric aggregation callback)
  │     └── owns OpenMetricsFormatter (snapshot → text/plain)
  └── ActorContext (per-actor)
        └── holds MetricsRingBuffer* (set during spawn, null if disabled)
```

### Design Constraints

- **No exceptions, no RTTI** (`-fno-exceptions -fno-rtti`). All error handling via `result<T>`.
- **No malloc on hot path**. Ring buffer slots are pre-allocated. Event writes are CAS + store.
- **C++20**. Uses `std::atomic_ref` where appropriate, `std::chrono::steady_clock` for timestamps.
- **No new dependencies**. OpenMetrics is simple line-based text. The ring buffer adapts existing code.
- **Graceful degradation**. If the ring buffer overflows between scrapes, `events_lost` is incremented and exposed as a metric. No crashes, no blocking.

---

## 3. Event Schema

### MetricEvent — 32-byte fixed-size event

```cpp
struct alignas(32) MetricEvent {
    uint64_t    timestamp_ns;    // CLOCK_MONOTONIC, written by producer
    ActorId     actor_id;        // 4 bytes
    uint8_t     event_type;      // MetricEventType enum
    uint8_t     reserved[3];     // padding
    uint32_t    value_hi;        // high 32 bits of metric value
    // Total: 8 + 4 + 1 + 3 + 4 = 20, + 12 implicit padding = 32 bytes
};
```

32 bytes so that two events fit in one cache line (64 bytes), minimizing false sharing.

### MetricEventType

| Event | Emitted When | Value Field |
|-------|-------------|-------------|
| `kMailboxEnqueue (0)` | Message enqueued to mailbox | 1 (increments depth) |
| `kMailboxDequeue (1)` | Message dequeued from mailbox | 1 (decrements depth) |
| `kMessageProcessed (2)` | Actor finished handling one message | latency_ns |
| `kActorSpawned (3)` | Actor created by ActorSystem | 1 |
| `kActorTerminated (4)` | Actor destroyed / on_exit() called | exit_reason_code |
| `kSchedulerDispatch (5)` | Worker picked up actor from pool | worker_id |
| `kSchedulerSteal (6)` | Work stolen from another worker | source_worker_id |
| `kSupervisorRestart (7)` | Supervisor restarted a child | child_actor_id |
| `kMemoryAlloc (8)` | Memory allocated via slab allocator | size_bytes |
| `kMemoryFree (9)` | Memory freed via slab allocator | size_bytes |

### Derived Metrics

Events are aggregated into these OpenMetrics families:

| Metric | Type | Source Events | Labels |
|--------|------|--------------|--------|
| `hpactor_mailbox_depth` | Gauge | Enqueue (+1), Dequeue (−1) | `actor_id`, `actor_type` |
| `hpactor_mailbox_messages_total` | Counter | Enqueue | `actor_id`, `actor_type` |
| `hpactor_message_processing_seconds` | Histogram | MessageProcessed | `actor_id`, `actor_type` |
| `hpactor_actor_lifecycle_total` | Counter | Spawned, Terminated | `event_type` |
| `hpactor_actors_active` | Gauge | Spawned (+1), Terminated (−1) | (none) |
| `hpactor_scheduler_dispatches_total` | Counter | Dispatch | `worker_id` |
| `hpactor_scheduler_steals_total` | Counter | Steal | `source_worker` |
| `hpactor_supervisor_restarts_total` | Counter | SupervisorRestart | `child_type` |
| `hpactor_memory_active_bytes` | Gauge | Alloc (+size), Free (−size) | `actor_id` |
| `hpactor_metrics_events_lost_total` | Counter | Ring buffer overflow | (none) |

---

## 4. Integration Points

### 4a. Mailbox (`MPSCActorMailbox`)

`MPSCActorMailbox::enqueue()` writes a `kMailboxEnqueue` event after the message lands in the ring buffer. `dequeue()` writes `kMailboxDequeue` after a successful pop. Both access the ring buffer via a pointer set during mailbox construction (null if metrics disabled).

Hot-path cost: one atomic CAS + store on the existing enqueue/dequeue paths. No allocation, no function call overhead (inline branch on null pointer).

### 4b. Message Processing (`EventBasedActor`)

`EventBasedActor::receive()` captures `std::chrono::steady_clock::now()` before invoking the behavior handler, computes the delta after the handler returns, and writes a `kMessageProcessed` event with the latency in nanoseconds.

Hot-path cost: two `clock_gettime` calls and one event write per handled message. Acceptable for both operational monitoring and latency debugging.

### 4c. Lifecycle (`ActorSystem` / `EventBasedActor`)

`ActorSystem::spawn()` writes `kActorSpawned` after actor construction and mailbox creation. `EventBasedActor::on_exit()` writes `kActorTerminated` before sending DownMsg.

Cold path — no perf concern.

### 4d. Scheduler (`HybridScheduler`)

`HybridScheduler::dispatch_actor()` writes `kSchedulerDispatch` with the worker_id. `try_steal()` writes `kSchedulerSteal` with the source worker_id after a successful steal.

Hot-path cost: one atomic store in dispatch/steal. Existing `WorkerStats` atomics can feed directly into the ring buffer — no additional tracking needed.

### 4e. Supervision (`SupervisorActor` / `SelfSupervisingActor`)

`restart_child()` writes `kSupervisorRestart` after incrementing the restart count. The existing `RestartCountMap` remains internal; the event is the observable side-effect.

Cold path — only fires on failure/restart.

### 4f. Memory (`SlabCache` / `SegmentProvider`)

The existing `TelemetryRingBuffer` already tracks allocation events. The memory allocator writes a co-event to the `MetricsRingBuffer` alongside the existing `TelemetryRingBuffer` write — or the two ring buffers can be consolidated if the `TelemetryRingBuffer` is refactored into a shared `MpscRingBuffer<AllocationEvent>` that both the memory debug system and the metrics system drain.

---

## 5. MetricsActor

`MetricsActor` is an `EventBasedActor` spawned as a system actor during `ActorSystem` initialization, before topology actors. It registers a request-response handler for `MetricsRequest`.

### Request Flow

1. HTTPGatewayActor receives `GET /metrics`.
2. Route dispatches a `MetricsRequest` (containing a reply handle) to MetricsActor.
3. MetricsActor drains the ring buffer: for each event, calls `Aggregator::on_event(event)` which routes to the correct `MetricFamily` + label set and updates atomics.
4. MetricsActor snapshots all `MetricFamily` values into a copy (reading atomics without locks).
5. `OpenMetricsFormatter` serializes the snapshot to `text/plain; version=1.0.0`.
6. MetricsActor replies with the formatted body.
7. HTTPGatewayActor sends the HTTP response.

### Ring Buffer Drain

The ring buffer uses the same `drain(callback)` pattern as `TelemetryRingBuffer`: iterate committed slots from last drained position, invoke callback per event, advance cursor. If the buffer wrapped around between scrapes (65536+ events lost), the `events_lost` counter is incremented and exposed as `hpactor_metrics_events_lost_total`.

---

## 6. OpenMetrics Wire Format

The `/metrics` endpoint serves:

```
# HELP hpactor_mailbox_depth Current mailbox queue depth.
# TYPE hpactor_mailbox_depth gauge
hpactor_mailbox_depth{actor_id="42",actor_type="EchoActor"} 3
hpactor_mailbox_depth{actor_id="43",actor_type="Worker"} 17
# HELP hpactor_mailbox_messages_total Total messages enqueued.
# TYPE hpactor_mailbox_messages_total counter
hpactor_mailbox_messages_total{actor_id="42",actor_type="EchoActor"} 1503
hpactor_mailbox_messages_total{actor_id="43",actor_type="Worker"} 8721
# HELP hpactor_message_processing_seconds Message processing latency.
# TYPE hpactor_message_processing_seconds histogram
hpactor_message_processing_seconds_bucket{actor_id="42",le="0.001"} 50
hpactor_message_processing_seconds_bucket{actor_id="42",le="0.002"} 102
hpactor_message_processing_seconds_bucket{actor_id="42",le="0.004"} 245
hpactor_message_processing_seconds_bucket{actor_id="42",le="0.008"} 480
hpactor_message_processing_seconds_bucket{actor_id="42",le="0.016"} 710
hpactor_message_processing_seconds_bucket{actor_id="42",le="0.032"} 890
hpactor_message_processing_seconds_bucket{actor_id="42",le="0.064"} 950
hpactor_message_processing_seconds_bucket{actor_id="42",le="0.128"} 980
hpactor_message_processing_seconds_bucket{actor_id="42",le="0.256"} 995
hpactor_message_processing_seconds_bucket{actor_id="42",le="0.512"} 998
hpactor_message_processing_seconds_bucket{actor_id="42",le="1.024"} 1000
hpactor_message_processing_seconds_bucket{actor_id="42",le="2.048"} 1000
hpactor_message_processing_seconds_bucket{actor_id="42",le="4.096"} 1000
hpactor_message_processing_seconds_bucket{actor_id="42",le="8.192"} 1000
hpactor_message_processing_seconds_bucket{actor_id="42",le="16.384"} 1000
hpactor_message_processing_seconds_bucket{actor_id="42",le="+Inf"} 1000
hpactor_message_processing_seconds_sum{actor_id="42"} 0.153
hpactor_message_processing_seconds_count{actor_id="42"} 1000
# HELP hpactor_metrics_events_lost_total Events lost due to ring buffer overflow.
# TYPE hpactor_metrics_events_lost_total counter
hpactor_metrics_events_lost_total 0
# EOF
```

Histogram bucketing uses 16 exponential buckets from 1ms to 16.384s (powers of 2 in seconds), plus `+Inf`. This covers sub-millisecond actor processing (common) to multi-second stalls (rare).

---

## 7. Configuration

Metrics are enabled via `ActorSystem::Config`:

```cpp
struct MetricsConfig {
    bool        enabled = true;             // Global on/off
    uint32_t    ring_buffer_capacity = 65536; // Power-of-two event capacity
    std::string metrics_path = "/metrics";  // HTTP endpoint path
    bool        per_actor_labels = true;    // Include actor_id labels (cardinality)
    bool        scheduler_metrics = true;   // Include scheduler events
    bool        memory_metrics = true;      // Include memory events
};
```

For TOML-based topologies, metrics configuration lives in the `[system]` table:

```toml
[system]
scheduler_threads = 4
http_port = 8080

[system.metrics]
enabled = true
ring_buffer_capacity = 131072
metrics_path = "/metrics"
```

If `enabled = false`, the `MetricsRingBuffer` pointer is null at all integration points, and the per-call overhead is a single null-pointer branch (predictable, near-zero cost).

---

## 8. Open Questions

1. **TelemetryRingBuffer consolidation**: Should the existing memory allocation `TelemetryRingBuffer` be migrated to share the generic `MpscRingBuffer<T>`, or should memory events be co-written to both buffers? Consolidation simplifies the codebase but touches the memory debug subsystem. The spec resolves this for the initial implementation: co-write (Option A), with consolidation deferred.

2. **Histogram bucket tuning**: The 16-bucket exponential scheme from 1ms to 16.384s is a reasonable default, but some workloads may need finer granularity. Should bucket boundaries be configurable via `MetricsConfig`? Defer until requested — the default covers the vast majority of use cases.
