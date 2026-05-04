# Actor-Level Metrics Design Specification

**Date:** 2026-05-04
**Status:** Draft
**Author:** HPActor Team

---

## Overview

Add a native actor-level metrics subsystem that exposes mailbox queue sizes, message processing latency, actor lifecycle events, scheduler activity, and memory allocation stats to Prometheus and Grafana via an OpenMetrics HTTP endpoint. The subsystem uses out-of-band instrumentation (lock-free ring buffer) to avoid adding overhead to actor hot paths.

## Goals

- **Mailbox metrics**: queue depth (gauge), total messages enqueued (counter) per actor
- **Latency metrics**: message processing time histogram per actor
- **Lifecycle metrics**: actor spawn/terminate counters, active actor count gauge
- **Scheduler metrics**: dispatches, work steals per worker thread
- **Supervision metrics**: restart counts per supervisor
- **Memory metrics**: per-actor active bytes from existing `MemoryTracker`
- **HTTP `/metrics` endpoint**: OpenMetrics text format served via existing HTTPGatewayActor
- **Zero hot-path overhead for user actors**: instrumentation writes come from mailbox, scheduler, and lifecycle hooks — not from actor code
- **Configurable**: enable/disable at compile and runtime, tune ring buffer capacity
- **No new external dependencies**: OpenMetrics is simple line-based text
- **No exceptions, no RTTI**: consistent with HPActor design constraints

## Non-Goals

- OpenTelemetry SDK integration (OTLP exporter, trace context propagation) — future phase
- Push model (Prometheus Pushgateway) — pull model only for initial implementation
- Custom metric registration by user actors (programmatic Counter/Gauge creation) — future phase
- gRPC metrics protocol — OpenMetrics text format only
- Per-message tracing (distributed trace IDs in message headers) — future phase
- Grafana dashboard definitions — out of scope for the C++ runtime; dashboard JSON belongs in a separate ops repo

---

## Architecture

### Component Diagram

```
ActorSystem
├── MetricsRingBuffer (lock-free MPSC, 32-byte events)
│   └── drained by MetricsActor on each /metrics scrape
├── MetricsActor (EventBasedActor, system actor)
│   ├── Aggregator (event dispatch → Counter/Gauge/Histogram update)
│   ├── MetricRegistry (owns all MetricFamily instances)
│   └── OpenMetricsFormatter (snapshot → text/plain)
├── ActorContext (per-actor, holds MetricsRingBuffer*)
├── MPSCActorMailbox (writes kMailboxEnqueue/kMailboxDequeue events)
├── EventBasedActor::receive() (writes kMessageProcessed event)
├── HybridScheduler (writes kSchedulerDispatch/kSchedulerSteal events)
├── SupervisorActor (writes kSupervisorRestart event)
└── SlabCache (writes kMemoryAlloc/kMemoryFree events)
```

### Data Flow

```
[Hot Path]                          [Scrape Path]
-----------                         -------------
mailbox.enqueue()                   HTTP GET /metrics
  │                                    │
  ├─ CAS slot in ring buffer           ├─ MetricsActor receives MetricsRequest
  ├─ store 32-byte MetricEvent         ├─ drain ring buffer (drain callback)
  └─ return (no allocation)            │   └─ Aggregator::on_event(event)
                                       │       └─ MetricFamily[label_set] += value
                                       ├─ snapshot all MetricFamilies
                                       ├─ OpenMetricsFormatter::format(snapshot)
                                       └─ reply(formatted_text)
```

### Directory Layout

```
include/hpactor/metrics/
    metrics_config.hpp          // MetricsConfig struct
    metrics_event.hpp            // MetricEvent, MetricEventType
    metrics_ring_buffer.hpp      // MpscRingBuffer<T> template
    metrics_registry.hpp         // Counter, Gauge, Histogram, MetricFamily, MetricRegistry
    metrics_aggregator.hpp       // Aggregator (event → registry dispatch)
    metrics_formatter.hpp        // OpenMetricsFormatter
    metrics_actor.hpp            // MetricsActor

src/metrics/
    metrics_actor.cpp            // MetricsActor implementation
    metrics_aggregator.cpp       // Aggregator implementation
    metrics_formatter.cpp        // OpenMetricsFormatter implementation
    metrics_registry.cpp         // MetricRegistry implementation
```

---

## Detailed Design

### 1. MetricsRingBuffer — Generic Lock-Free MPSC Ring Buffer

**Refactor** the existing `mem/telemetry_ring_buffer.hpp` into a generic `MpscRingBuffer<T>` template in `include/hpactor/metrics/metrics_ring_buffer.hpp`. The existing `TelemetryRingBuffer` becomes a type alias or thin adapter.

The existing ring buffer uses a single-phase CAS: `reserve()` atomically claims a slot via CAS on the write cursor and returns a reference — the caller writes directly into that slot, and the write is implicitly committed because the write cursor has already advanced. The consumer reads all slots up to the write cursor. This design is preserved and parameterized on `T`.

```cpp
template <typename T>
class MpscRingBuffer {
public:
    static constexpr size_t kDefaultCapacity = 65536; // must be power of 2

    explicit MpscRingBuffer(size_t capacity = kDefaultCapacity);

    // Producer: atomically claim a slot. Returns nullptr if buffer is full.
    // Caller writes directly to the returned reference. The slot is committed
    // by the CAS that reserved it — no separate commit() call.
    T* reserve();

    // Consumer: drain all slots up to the write cursor.
    // Callback invoked per slot; return true to continue.
    template <typename Fn>
    size_t drain(Fn&& callback);

    // Number of events lost due to buffer full since last drain.
    uint64_t events_lost() const;

    // Current number of committed (un-drained) slots.
    size_t size() const;

private:
    // ... CAS write_cursor, consumer read_cursor, power-of-two bitmask ...
};
```

**Design notes:**
- Capacity must be power of 2 for fast bitmask modulo (`index & (capacity - 1)`).
- `reserve()` CAS-increments the write cursor. If `write_cursor - read_cursor >= capacity`, buffer is full — return nullptr and increment `events_lost_`.
- No per-slot metadata. The write cursor itself demarcates committed slots — all slots between `read_cursor` and `write_cursor` are valid for the consumer. This is the exact design the existing `TelemetryRingBuffer` uses today.
- `drain()` iterates from `read_cursor` to `write_cursor`, invoking the callback per slot. After drain, `read_cursor` is advanced to `write_cursor`.
- No memory allocation in `reserve()`. The buffer is a contiguous `std::vector<T>` pre-allocated at construction.

### 2. MetricEvent Schema

```cpp
// include/hpactor/metrics/metrics_event.hpp

namespace hpactor::metrics {

enum class MetricEventType : uint8_t {
    kMailboxEnqueue     = 0,  // value_hi = 1 (increments depth)
    kMailboxDequeue     = 1,  // value_hi = 1 (decrements depth)
    kMessageProcessed   = 2,  // value_hi = latency_ns
    kActorSpawned       = 3,  // value_hi = 1
    kActorTerminated    = 4,  // value_hi = exit_reason_code
    kSchedulerDispatch  = 5,  // value_hi = worker_id
    kSchedulerSteal     = 6,  // value_hi = source_worker_id
    kSupervisorRestart  = 7,  // value_hi = child_actor_id
    kMemoryAlloc        = 8,  // value_hi = size_bytes
    kMemoryFree         = 9,  // value_hi = size_bytes
};

struct alignas(32) MetricEvent {
    uint64_t         timestamp_ns;   // CLOCK_MONOTONIC
    ActorId          actor_id;       // 4 bytes
    MetricEventType  event_type;     // 1 byte
    uint8_t          reserved[3];    // alignment padding
    uint32_t         value_hi;       // metric-specific value
};
// static_assert(sizeof(MetricEvent) == 32);

} // namespace hpactor::metrics
```

32 bytes so two events fit in one 64-byte cache line. Producers write `timestamp_ns` via `clock_gettime(CLOCK_MONOTONIC, ...)` before storing. Consumers read in drain order.

**`actor_type` label**: The `MetricEvent` does not carry an actor type field — adding it would bloat every event on the hot path. Instead, the `actor_type` label is resolved during drain (scrape path) via a `ActorSystem*` reference held by the `Aggregator`. On first encounter of an `actor_id`, the aggregator queries `ActorSystem` for the actor's type name (a new `virtual const char* type_name()` on `AbstractActor`, defaulting to the behavior name string from the factory registry). The result is cached in an `unordered_map<ActorId, std::string>`. If the actor has already terminated, a cached value (from a prior spawn event) is used; if never cached, `"unknown"` is emitted. See Section 4 for details.

### 3. Metric Types

```cpp
// include/hpactor/metrics/metrics_registry.hpp

namespace hpactor::metrics {

enum class MetricType { kCounter, kGauge, kHistogram };

struct LabelSet {
    std::string metric_name;
    std::unordered_map<std::string, std::string> labels;
    // equality/hash based on sorted label map serialization
    bool operator==(const LabelSet&) const;
};

struct LabelSetHash {
    size_t operator()(const LabelSet&) const;
};

// Metric values — atomic for lock-free producer updates, snapshot for consumer reads
struct alignas(64) CounterValue {
    std::atomic<uint64_t> total{0};
};

struct alignas(64) GaugeValue {
    std::atomic<int64_t> value{0};
};

struct alignas(64) HistogramValue {
    std::atomic<uint64_t> count{0};
    // double atomics aren't standard; we store sum_ns as uint64_t and convert on read
    std::atomic<uint64_t> sum_ns{0};
    std::atomic<uint64_t> buckets[16];  // exponential, 1ms base
};

struct MetricFamily {
    std::string name;       // "hpactor_mailbox_depth"
    std::string help;       // "Current mailbox queue depth."
    MetricType  type;       // kGauge / kCounter / kHistogram
    std::unordered_map<LabelSet, std::variant<CounterValue, GaugeValue, HistogramValue>,
                       LabelSetHash> values;
};

class MetricRegistry {
public:
    // Register a metric family (idempotent — returns existing if already registered)
    MetricFamily& register_family(std::string name, std::string help, MetricType type);

    // Get or create a labeled value within a family
    template <typename V>
    V& get_or_create(MetricFamily& family, const LabelSet& labels);

    // Snapshot all families into plain (non-atomic) copies for formatting
    struct Snapshot {
        struct FamilySnapshot {
            std::string name, help;
            MetricType type;
            std::vector<std::pair<LabelSet, uint64_t>> counters;  // total
            std::vector<std::pair<LabelSet, int64_t>> gauges;     // value
            struct HistogramSnapshot {
                LabelSet labels;
                uint64_t count;
                double   sum_seconds;
                uint64_t buckets[16];
            };
            std::vector<HistogramSnapshot> histograms;
        };
        std::vector<FamilySnapshot> families;
    };
    Snapshot snapshot();

private:
    std::vector<std::unique_ptr<MetricFamily>> families_;
    std::unordered_map<std::string, MetricFamily*> family_index_;
};

} // namespace hpactor::metrics
```

**Design notes:**
- `HistogramValue::buckets[]` uses exponential bounds: 1ms, 2ms, 4ms, 8ms, 16ms, 32ms, 64ms, 128ms, 256ms, 512ms, 1.024s, 2.048s, 4.096s, 8.192s, 16.384s, +Inf (bucket[15]).
- `histogram_insert(value_ns)` does: find bucket index via `63 - __builtin_clzll(value_ns / 1000000)` (leading zero count on milliseconds), clamp to [0, 15]. Then `fetch_add` on that bucket, `fetch_add(1)` on count, `fetch_add(value_ns)` on sum_ns.
- `GaugeValue::value` is modified via `fetch_add(1)` or `fetch_sub(1)` for depth, `fetch_add(size_bytes)` for memory.
- Cache-line alignment (64 bytes) on each value struct prevents false sharing between producers updating different actors' metrics.
- No locks. All updates are atomic `fetch_add`. Snapshots read with `load(memory_order_relaxed)` — acceptable for operational metrics.

### 4. Aggregator

```cpp
// include/hpactor/metrics/metrics_aggregator.hpp

namespace hpactor::metrics {

class Aggregator {
public:
    Aggregator(MetricRegistry& registry, ActorSystem& system);

    // Called for each event drained from the ring buffer.
    // Dispatches to the correct MetricFamily + label set and updates atomics.
    void on_event(const MetricEvent& event);

    // Called before drain — builds any derived gauges (e.g., actor_active from
    // spawn/terminate counts).
    void begin_drain();

    // Called after drain — finalizes derived metrics.
    void end_drain();

private:
    MetricRegistry& registry_;
    ActorSystem&    system_;

    // Pre-resolved MetricFamily references for hot-path dispatch
    MetricFamily* mailbox_depth_family_;
    MetricFamily* mailbox_messages_family_;
    MetricFamily* processing_latency_family_;
    MetricFamily* lifecycle_family_;
    MetricFamily* scheduler_dispatch_family_;
    MetricFamily* scheduler_steal_family_;
    MetricFamily* supervisor_restart_family_;
    MetricFamily* memory_bytes_family_;

    // Derived: active actors gauge
    int64_t active_actors_{0};

    // Actor type name cache: actor_id → type_name string.
    // Populated lazily during drain via ActorSystem::get_actor().
    // Eviction not needed — bounded by max actor count.
    mutable std::unordered_map<ActorId, std::string> actor_type_cache_;

    // Helper: build LabelSet for a given actor_id. Resolves actor_type
    // from cache (or queries ActorSystem on cache miss).
    LabelSet make_actor_labels(ActorId id);
};

} // namespace hpactor::metrics
```

**Actor type resolution** in `make_actor_labels()`:

```cpp
LabelSet Aggregator::make_actor_labels(ActorId id) {
    std::string type_name;

    auto it = actor_type_cache_.find(id);
    if (it != actor_type_cache_.end()) {
        type_name = it->second;
    } else {
        // Query ActorSystem for the actor. If still alive, read type_name().
        // Cache the result (even "unknown" for terminated actors).
        auto actor = system_.get_actor(id);
        type_name = actor ? actor->type_name() : "unknown";
        actor_type_cache_[id] = type_name;
    }

    LabelSet ls;
    ls.labels["actor_id"] = std::to_string(id.value());
    ls.labels["actor_type"] = type_name;
    return ls;
}
```

`AbstractActor::type_name()` is a new virtual method. Default implementation returns the behavior name string from the factory registry (stored during spawn). Zero overhead — not on the hot path. No RTTI required.

**`on_event()` dispatch logic:**

```cpp
void Aggregator::on_event(const MetricEvent& e) {
    switch (e.event_type) {
    case MetricEventType::kMailboxEnqueue: {
        auto& g = registry_.get_or_create<GaugeValue>(
            *mailbox_depth_family_, make_actor_labels(e.actor_id));
        g.value.fetch_add(1, std::memory_order_relaxed);
        auto& c = registry_.get_or_create<CounterValue>(
            *mailbox_messages_family_, make_actor_labels(e.actor_id));
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kMailboxDequeue: {
        auto& g = registry_.get_or_create<GaugeValue>(
            *mailbox_depth_family_, make_actor_labels(e.actor_id));
        g.value.fetch_sub(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kMessageProcessed: {
        auto& h = registry_.get_or_create<HistogramValue>(
            *processing_latency_family_, make_actor_labels(e.actor_id));
        histogram_insert(h, e.value_hi);  // value_hi = latency_ns
        break;
    }
    case MetricEventType::kActorSpawned:
        active_actors_++;
        // fall through
    case MetricEventType::kActorTerminated: {
        if (e.event_type == MetricEventType::kActorTerminated)
            active_actors_--;
        auto& c = registry_.get_or_create<CounterValue>(
            *lifecycle_family_, make_actor_labels(e.actor_id));
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kSchedulerDispatch: {
        LabelSet ls{"hpactor_scheduler_dispatches_total", {{"worker_id", std::to_string(e.value_hi)}}};
        auto& c = registry_.get_or_create<CounterValue>(*scheduler_dispatch_family_, ls);
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kSchedulerSteal: {
        LabelSet ls{"hpactor_scheduler_steals_total", {{"source_worker", std::to_string(e.value_hi)}}};
        auto& c = registry_.get_or_create<CounterValue>(*scheduler_steal_family_, ls);
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kSupervisorRestart: {
        auto& c = registry_.get_or_create<CounterValue>(
            *supervisor_restart_family_, make_actor_labels(e.actor_id));
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kMemoryAlloc: {
        auto& g = registry_.get_or_create<GaugeValue>(
            *memory_bytes_family_, make_actor_labels(e.actor_id));
        g.value.fetch_add(static_cast<int64_t>(e.value_hi), std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kMemoryFree: {
        auto& g = registry_.get_or_create<GaugeValue>(
            *memory_bytes_family_, make_actor_labels(e.actor_id));
        g.value.fetch_sub(static_cast<int64_t>(e.value_hi), std::memory_order_relaxed);
        break;
    }
    }
}
```

### 5. OpenMetricsFormatter

```cpp
// include/hpactor/metrics/metrics_formatter.hpp

namespace hpactor::metrics {

class OpenMetricsFormatter {
public:
    // Serialize a registry snapshot to OpenMetrics text format.
    // Returns the formatted text as a string suitable for HTTP response body.
    std::string format(const MetricRegistry::Snapshot& snapshot);

private:
    void format_counter(std::string& out,
                        const std::string& name, const std::string& help,
                        const std::vector<std::pair<LabelSet, uint64_t>>& values);
    void format_gauge(std::string& out,
                      const std::string& name, const std::string& help,
                      const std::vector<std::pair<LabelSet, int64_t>>& values);
    void format_histogram(std::string& out,
                          const std::string& name, const std::string& help,
                          const std::vector<MetricRegistry::Snapshot::FamilySnapshot::HistogramSnapshot>& values);

    // Pre-computed bucket boundaries in seconds (for le= labels)
    static constexpr double kBucketBounds[15] = {
        0.001, 0.002, 0.004, 0.008, 0.016, 0.032, 0.064,
        0.128, 0.256, 0.512, 1.024, 2.048, 4.096, 8.192, 16.384
    };
};

} // namespace hpactor::metrics
```

**Output format** follows the OpenMetrics specification:

- `# HELP <name> <help text>` — metric description
- `# TYPE <name> <counter|gauge|histogram>` — metric type
- Counter lines: `<name>_total{<labels>} <value>`
- Gauge lines: `<name>{<labels>} <value>`
- Histogram: `<name>_bucket{le="<bound>"} <count>` per bucket, then `<name>_sum` and `<name>_count`
- `# EOF` — end-of-file marker

Label escaping: double quotes around label values, escape `\`, `"`, and newline.

### 6. MetricsActor

```cpp
// include/hpactor/metrics/metrics_actor.hpp

namespace hpactor::metrics {

class MetricsActor : public EventBasedActor {
public:
    MetricsActor(ActorSystem& system,
                 std::shared_ptr<MpscRingBuffer<MetricEvent>> ring_buffer);

    void register_handlers() override;

private:
    std::shared_ptr<MpscRingBuffer<MetricEvent>> ring_buffer_;
    MetricRegistry                                registry_;
    Aggregator                                    aggregator_;
    OpenMetricsFormatter                          formatter_;
    uint64_t                                      events_lost_{0};
};

} // namespace hpactor::metrics
```

**Implementation** (`src/metrics/metrics_actor.cpp`):

```cpp
void MetricsActor::register_handlers() {
    on_request<MetricsRequest, MetricsResponse>(
        [this](const MetricsRequest& req) -> result<MetricsResponse> {
            // Drain ring buffer
            aggregator_.begin_drain();
            size_t drained = ring_buffer_->drain(
                [this](const MetricEvent& e) {
                    aggregator_.on_event(e);
                    return true;  // continue draining
                });
            aggregator_.end_drain();

            // Track lost events
            events_lost_ += ring_buffer_->events_lost();

            // Snapshot and format
            auto snapshot = registry_.snapshot();
            MetricsResponse resp;
            resp.set_body(formatter_.format(snapshot));

            // Append events_lost metric
            // "# HELP hpactor_metrics_events_lost_total Events lost due to ring buffer overflow."
            // ...
            return resp;
        });
}
```

**Wire-up in ActorSystem:**

```cpp
// In ActorSystem constructor or init():
if (config_.metrics.enabled) {
    metrics_ring_buffer_ = std::make_shared<MpscRingBuffer<MetricEvent>>(
        config_.metrics.ring_buffer_capacity);
    auto metrics_actor = spawn<MetricsActor>(*this, metrics_ring_buffer_);

    if (http_gateway_actor_) {
        // RouteRegistry::MessageBuilder signature:
        //   std::function<std::pair<ActorAddress, TypedMessage>(const HttpRequest&)>
        http_gateway_actor_->route(
            HttpMethod::GET, config_.metrics.metrics_path,
            [metrics_actor](const HttpRequest& req) {
                // Build a MetricsRequest protobuf message targeting the MetricsActor.
                // The HTTPGatewayActor's PendingReply mechanism correlates the
                // MetricsActor's reply back to the HTTP connection.
                MetricsRequest metrics_req;
                TypedMessage msg;
                msg.set_type_tag(TypeTag::METRICS_REQUEST);
                msg.set_payload(metrics_req.SerializeAsString());
                return std::make_pair(metrics_actor->address(), std::move(msg));
            });
    }
}
```

The request/response types are protobuf messages (required by the `on_request<ReqT, ResT>` dispatch mechanism):

```protobuf
// Added to the existing hpactor_types.proto schema:
message MetricsRequest {
    // Empty — the presence of this message triggers a full drain + snapshot + reply.
}

message MetricsResponse {
    bytes body = 1;  // OpenMetrics text, set as HTTP response body
}
```

Two new `TypeTag` enum values must be added to the protobuf schema: `METRICS_REQUEST = 18` and `METRICS_RESPONSE = 19` (next available numbers).

The `ReplyAdapter` pattern already in `HTTPGatewayActor` (see `src/actor/http_gateway_actor.cpp`) handles the `reply()` → HTTP response path. The `MetricsActor` calls `context()->reply(MetricsResponse{formatted_body})`, and the gateway's `PendingReply` registry maps the request ID back to the `HTTPConnection` and sends the HTTP response.

**Implementation plan addition**: Modifying the `.proto` schema and re-running protobuf codegen is a step in Phase 3 (step 14a), before registering the `/metrics` route.

### 7. Integration Point Details

#### 7a. Mailbox

In `MPSCActorMailbox::enqueue()` (`include/hpactor/mailbox/mpsc_actor_mailbox.hpp`):

```cpp
void enqueue(T* node) noexcept {
    // ... existing logic ...
    if (metrics_ring_buffer_) [[unlikely]] {
        auto* slot = metrics_ring_buffer_->reserve();
        if (slot) [[likely]] {
            slot->timestamp_ns = clock_gettime_ns();
            slot->actor_id = actor_id_;
            slot->event_type = MetricEventType::kMailboxEnqueue;
            slot->value_hi = 1;
        }
    }
}
```

Same pattern for `dequeue()`. The `metrics_ring_buffer_` pointer is set during mailbox construction (null if metrics disabled). The `[[unlikely]]` attribute on the null check tells the branch predictor this is the cold path.

#### 7b. Message Processing Latency

In `EventBasedActor::receive()` (`src/actor/event_based_actor.cpp`):

```cpp
result<void> EventBasedActor::receive(TypedMessage& msg) {
    // ... system message handling ...

    auto start = metrics_ring_buffer_ ? std::chrono::steady_clock::now() 
                                      : std::chrono::steady_clock::time_point{};
    auto result = behavior_->handle(msg);
    if (metrics_ring_buffer_) [[unlikely]] {
        auto end = std::chrono::steady_clock::now();
        auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        auto* slot = metrics_ring_buffer_->reserve();
        if (slot) [[likely]] {
            slot->timestamp_ns = to_monotonic_ns(start);
            slot->actor_id = id();
            slot->event_type = MetricEventType::kMessageProcessed;
            slot->value_hi = static_cast<uint32_t>(latency_ns > UINT32_MAX ? UINT32_MAX : latency_ns);
                    }
    }
    return result;
}
```

**Latency value clamping**: `value_hi` is `uint32_t`, so latency values > 4.29 seconds are clamped to `UINT32_MAX`. This is acceptable — sub-second latencies are the target. For longer-running handlers, the histogram's `+Inf` bucket captures the count, and the sum still contributes to `sum_seconds` (derived from count × average from events, or more precisely: the aggregator keeps a separate `sum_ns` at higher precision).

**Alternative**: Make `value_hi` a `uint64_t` by repacking the event struct. The current design prioritizes 32-byte alignment; we can repack if needed.

#### 7c. Scheduler

In `HybridScheduler::dispatch_actor()` and `try_steal()` (`src/sched/scheduler.cpp`):

```cpp
// In dispatch_actor():
if (metrics_ring_buffer_) [[unlikely]] {
    auto* slot = metrics_ring_buffer_->reserve();
    if (slot) [[likely]] {
        slot->timestamp_ns = clock_gettime_ns();
        slot->actor_id = actor_id;
        slot->event_type = MetricEventType::kSchedulerDispatch;
        slot->value_hi = worker_id;
            }
}
```

Existing `WorkerStats` atomics (`steal_attempts`, `steal_successes`) are retained — they can feed the ring buffer or be read by the aggregator during drain for scheduler utilization metrics.

#### 7d. Lifecycle

In `ActorSystem::spawn()` (after actor creation, before `on_activate()`):

```cpp
if (metrics_ring_buffer_) [[unlikely]] {
    auto* slot = metrics_ring_buffer_->reserve();
    if (slot) [[likely]] {
        slot->timestamp_ns = clock_gettime_ns();
        slot->actor_id = actor->id();
        slot->event_type = MetricEventType::kActorSpawned;
        slot->value_hi = 1;
            }
}
```

In `EventBasedActor::on_exit()` (before DownMsg):

```cpp
if (metrics_ring_buffer_) [[unlikely]] {
    auto* slot = metrics_ring_buffer_->reserve();
    if (slot) [[likely]] {
        slot->timestamp_ns = clock_gettime_ns();
        slot->actor_id = id();
        slot->event_type = MetricEventType::kActorTerminated;
        slot->value_hi = static_cast<uint32_t>(exit_reason);
            }
}
```

#### 7e. Supervision

In `SupervisorActor::restart_child()` and `SelfSupervisingActor::decide_restart()`:

```cpp
if (metrics_ring_buffer_) [[unlikely]] {
    auto* slot = metrics_ring_buffer_->reserve();
    if (slot) [[likely]] {
        slot->timestamp_ns = clock_gettime_ns();
        slot->actor_id = id();           // supervisor's ID
        slot->event_type = MetricEventType::kSupervisorRestart;
        slot->value_hi = child_id.value();
            }
}
```

Supervision metrics are **counter-only** from events. A gauge of current per-supervisor restart counts would require a supervisor registry that `MetricsActor` could iterate — the codebase has no such registry, and adding one is out of scope for this spec. The `hpactor_supervisor_restarts_total` counter, incremented by the event on each restart, is sufficient for alerting on restart rate (via Prometheus `rate()`).

#### 7f. Memory

Two options:

**Option A — Co-write**: The existing `TelemetryRingBuffer` continues to operate independently for memory debugging. Memory allocation events are co-written to the `MetricsRingBuffer` with a `kMemoryAlloc`/`kMemoryFree` event. Simple but two ring buffer writes per allocation.

**Option B — Consolidate**: Refactor `TelemetryRingBuffer` to use `MpscRingBuffer<AllocationEvent>`. The memory debugging system drains one consumer; the metrics system drains another (both can share the same ring buffer with independent cursors, or the memory system's existing consumer forwards events to the metrics aggregator).

**Recommendation**: Option A for the initial implementation — minimal refactoring risk. The memory allocator hot path already does one ring buffer write; a second write is acceptable (both are lock-free CAS). Consolidation can follow as a separate cleanup task.

### 8. Configuration

```cpp
// include/hpactor/metrics/metrics_config.hpp

namespace hpactor::metrics {

struct MetricsConfig {
    bool        enabled              = true;
    uint32_t    ring_buffer_capacity = 65536;   // power of 2
    std::string metrics_path         = "/metrics";
    bool        per_actor_labels     = true;    // include actor_id in labels
    bool        scheduler_metrics    = true;
    bool        memory_metrics       = true;
};

} // namespace hpactor::metrics
```

Wired into `ActorSystem::Config`:

```cpp
struct Config {
    // ... existing fields ...
    metrics::MetricsConfig metrics;
};
```

**TOML representation:**

```toml
[system]
scheduler_threads = 4

[system.metrics]
enabled = true
ring_buffer_capacity = 65536
metrics_path = "/metrics"
per_actor_labels = true
scheduler_metrics = true
memory_metrics = true
```

**Compile-time disable**: A CMake option `ENABLE_ACTOR_METRICS` (default ON) guards the feature. When OFF, all `metrics_ring_buffer_` pointers are `nullptr` and the event struct/enum are not compiled. This provides zero-overhead for deployments that don't need metrics.

---

## Implementation Plan (Phases)

### Phase 1: Core Infrastructure
1. Extract `MpscRingBuffer<T>` from `TelemetryRingBuffer` (refactor, no functional change).
2. Define `MetricEvent`, `MetricEventType`, `MetricsConfig`.
3. Implement `CounterValue`, `GaugeValue`, `HistogramValue`, `MetricFamily`, `MetricRegistry`.
4. Implement `Aggregator`.
5. Implement `OpenMetricsFormatter`.
6. Implement `MetricsActor`.

### Phase 2: Integration Points
7. Wire `MPSCActorMailbox` — enqueue/dequeue events.
8. Wire `EventBasedActor::receive()` — latency events.
9. Wire `ActorSystem::spawn()` — spawn events.
10. Wire `EventBasedActor::on_exit()` — terminate events.
11. Wire `HybridScheduler` — dispatch/steal events.
12. Wire `SupervisorActor` / `SelfSupervisingActor` — restart events.
13. Wire memory allocator — alloc/free events.

### Phase 3: HTTP Endpoint & Config
14. Add `METRICS_REQUEST` and `METRICS_RESPONSE` to the protobuf schema (`TypeTag` enum, message definitions). Re-run protobuf codegen.
15. Register `/metrics` route on `HTTPGatewayActor`.
16. Add `MetricsConfig` to `ActorSystem::Config` and TOML parsing.
17. Add `ENABLE_ACTOR_METRICS` CMake option.
18. End-to-end test: spawn actors, send messages, scrape `/metrics`, verify output.

---

## Testing Strategy

### Unit Tests (Phase 1)
- `test_metrics_ring_buffer`: MPSC correctness (concurrent produce/consume, overflow, drain).
- `test_metrics_registry`: get_or_create, snapshot consistency, histogram bucket insertion.
- `test_metrics_aggregator`: event dispatch produces correct counter/gauge/histogram values.
- `test_metrics_formatter`: snapshot → valid OpenMetrics text (parseable by Prometheus parser).

### Integration Tests (Phase 2)
- `test_metrics_mailbox`: enqueue/dequeue produces correct `hpactor_mailbox_depth` gauge.
- `test_metrics_latency`: message processing produces non-zero `hpactor_message_processing_seconds` histogram.
- `test_metrics_lifecycle`: spawn/terminate produces correct `hpactor_actor_lifecycle_total` counter.
- `test_metrics_supervision`: supervised actor restart produces `hpactor_supervisor_restarts_total` counter.

### End-to-End Test (Phase 3)
- `test_metrics_endpoint`: Full system with actors under load. Scrape `/metrics`, assert expected metric names and label cardinality.

---

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Ring buffer overflow at high throughput | 64K events at 30s scrape interval = ~2K events/s sustained. At 100K msgs/s, overflow occurs. Expose `events_lost` counter; configurable capacity |
| Label cardinality explosion | Bounded by number of actors (max 1M via ActorSystem). `per_actor_labels = false` drops `actor_id` for production |
| `histogram_insert` via `__builtin_clzll` not portable | Fallback to loop-based bucket search; `__builtin_clzll` is available on Clang/GCC (the project's target compilers) |
| `clock_gettime` overhead on hot path | Two calls per message (start + end) for `kMessageProcessed`. LTTng/perf-level overhead (~20ns each). Acceptable given the informational value |
| OpenMetrics format edge cases | Validate output against Prometheus `promtool check metrics` in CI |
