# Actor-Level Metrics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an out-of-band actor-level metrics subsystem with lock-free ring buffer instrumentation, OpenMetrics formatting, and an HTTP `/metrics` endpoint for Prometheus scraping.

**Architecture:** Instrumentation points (mailbox, scheduler, lifecycle hooks) write 32-byte `MetricEvent` structs to a lock-free `MpscRingBuffer` via CAS-based reserve. A `MetricsActor` (system `EventBasedActor`) drains the buffer on each HTTP scrape, aggregates events into `Counter`/`Gauge`/`Histogram` objects in a `MetricRegistry`, snapshots atomics, and formats OpenMetrics text for the HTTP response via the existing `HTTPGatewayActor` route registry.

**Tech Stack:** C++20, `std::atomic`, OpenMetrics text format, existing HPActor protobuf messaging, llhttp (vendored), CMake with Ninja.

**Spec:** `docs/superpowers/specs/2026-05-04-actor-metrics-design.md`
**Architecture doc:** `docs/architecture/actor/actor-metrics-design.md`

---

## File Structure

| File | Purpose |
|------|---------|
| `include/hpactor/metrics/metrics_config.hpp` | `MetricsConfig` struct |
| `include/hpactor/metrics/metrics_ring_buffer.hpp` | `MpscRingBuffer<T>` template (extracted from `TelemetryRingBuffer`) |
| `include/hpactor/metrics/metrics_event.hpp` | `MetricEvent` struct, `MetricEventType` enum |
| `include/hpactor/metrics/metrics_registry.hpp` | `CounterValue`, `GaugeValue`, `HistogramValue`, `MetricFamily`, `MetricRegistry` |
| `include/hpactor/metrics/metrics_aggregator.hpp` | `Aggregator` (event → metric dispatch) |
| `include/hpactor/metrics/metrics_formatter.hpp` | `OpenMetricsFormatter` |
| `include/hpactor/metrics/metrics_actor.hpp` | `MetricsActor` class |
| `src/metrics/metrics_aggregator.cpp` | Aggregator implementation |
| `src/metrics/metrics_formatter.cpp` | Formatter implementation |
| `src/metrics/metrics_registry.cpp` | Registry implementation |
| `src/metrics/metrics_actor.cpp` | MetricsActor implementation |
| `include/hpactor/mem/telemetry_ring_buffer.hpp` | **Modified** — becomes alias for `MpscRingBuffer<AllocationEvent>` |
| `include/hpactor/types/types.hpp` | **Modified** — add `MetricsRequestTag`, `MetricsResponseTag` |
| `include/hpactor/core/actor_system.hpp` | **Modified** — add `MetricsConfig` to `Config`, add `metrics_ring_buffer_` member |
| `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` | **Modified** — add `metrics_ring_buffer_` pointer, emit enqueue/dequeue events |
| `include/hpactor/actor/abstract_actor.hpp` | **Modified** — add `virtual type_name()` |
| `include/hpactor/actor/event_based_actor.hpp` | **Modified** — add `metrics_ring_buffer_` pointer + setter |
| `src/actor/event_based_actor.cpp` | **Modified** — emit `kMessageProcessed` in `receive()`, emit `kActorTerminated` in `on_exit()` |
| `src/actor/actor_system.cpp` | **Modified** — emit spawn events, spawn MetricsActor, wire `/metrics` route |
| `src/sched/scheduler.cpp` | **Modified** — emit dispatch/steal events |
| `src/supervision/supervision.cpp` | **Modified** — emit restart events |
| `src/config/toml_parser.cpp` | **Modified** — parse `[system.metrics]` |
| `include/hpactor/config/topology_model.hpp` | **Modified** — add metrics fields to `SystemDef` |
| `CMakeLists.txt` | **Modified** — add new source files, add `ENABLE_ACTOR_METRICS` option |
| `protos/hpactor/common.proto` | **Modified** — add `MetricsRequest`, `MetricsResponse` messages |

---

### Task 1: Extract Generic `MpscRingBuffer<T>` from `TelemetryRingBuffer`

**Files:**
- Create: `include/hpactor/metrics/metrics_ring_buffer.hpp`
- Modify: `include/hpactor/mem/telemetry_ring_buffer.hpp`

- [ ] **Step 1: Create the generic ring buffer header**

Create `include/hpactor/metrics/metrics_ring_buffer.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hpactor {
namespace metrics {

// Lock-free MPSC ring buffer. Multi-producer, single-consumer.
// Capacity must be a power of two.
// Producer: reserve() CAS-claims a slot, caller writes fields directly.
// Consumer: drain(callback) reads all committed slots since last drain.
template <typename T, size_t Capacity = 65536>
class MpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    static constexpr size_t kDefaultCapacity = Capacity;

    MpscRingBuffer() : buffer_(Capacity) {}

    // Producer: atomically claim a slot. Returns nullptr if buffer is full.
    // Caller writes directly into the returned slot; no separate commit needed.
    T* reserve() noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_relaxed);
        do {
            if (w - read_cursor_.load(std::memory_order_acquire) >= Capacity) {
                events_lost_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
        } while (!write_cursor_.compare_exchange_weak(
            w, w + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
        return &buffer_[w & mask_];
    }

    // Consumer: drain all committed slots since last drain.
    template <typename Fn>
    size_t drain(Fn&& callback) {
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        size_t count = 0;
        while (r < w) {
            callback(buffer_[r & mask_]);
            ++r;
            ++count;
        }
        read_cursor_.store(r, std::memory_order_release);
        return count;
    }

    uint64_t events_lost() const noexcept {
        return events_lost_.load(std::memory_order_relaxed);
    }

    size_t size() const noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        return static_cast<size_t>(w - r);
    }

    bool empty() const noexcept { return size() == 0; }

private:
    static constexpr size_t mask_ = Capacity - 1;
    alignas(64) std::atomic<uint64_t> write_cursor_{0};
    alignas(64) std::atomic<uint64_t> read_cursor_{0};
    alignas(64) std::atomic<uint64_t> events_lost_{0};
    std::vector<T> buffer_;
};

} // namespace metrics
} // namespace hpactor
```

- [ ] **Step 2: Make `TelemetryRingBuffer` an alias for `MpscRingBuffer<AllocationEvent>`**

Read `include/hpactor/mem/telemetry_ring_buffer.hpp` first.

Replace its contents — keep the `AllocationEvent` struct but replace the ring buffer class with:

```cpp
#pragma once

#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <cstdint>

namespace hpactor {
namespace mem {

struct AllocationEvent {
    uint64_t timestamp;
    uint32_t actor_id;
    uint16_t block_size;
    uint8_t  size_class;
    uint8_t  region_type;
    uint8_t  event_type;  // 0=alloc, 1=free, 2=corruption, 3=hibernate_in, 4=hibernate_out
    uint8_t  _pad[7];
};

// Alias: reuse the generic MpscRingBuffer
template <size_t Capacity = 65536>
using TelemetryRingBuffer = metrics::MpscRingBuffer<AllocationEvent, Capacity>;

} // namespace mem
} // namespace hpactor
```

Note: The existing `TelemetryRingBuffer` uses `try_push(const AllocationEvent&)` which copies the event. The new API uses `reserve()` returning a pointer. All existing call sites of `try_push(event)` must be updated to:
```cpp
auto* slot = buffer.reserve();
if (slot) { *slot = event; }
```

Search for `try_push` call sites and update them.

- [ ] **Step 3: Find and fix all `try_push` call sites**

Run: `grep -rn "try_push" src/ include/ tests/`

Expected: hits in `src/mem/` or wherever memory tracking calls it. Update each to the new `reserve()` pattern.

- [ ] **Step 4: Build and verify**

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 5: Run existing tests**

Run: `ctest --output-on-failure`

Expected: all 88 tests pass (regression check).

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/metrics/metrics_ring_buffer.hpp include/hpactor/mem/telemetry_ring_buffer.hpp src/mem/*.cpp
git commit -m "refactor: extract generic MpscRingBuffer template from TelemetryRingBuffer"
```

---

### Task 2: Define `MetricEvent`, `MetricEventType`, and `MetricsConfig`

**Files:**
- Create: `include/hpactor/metrics/metrics_event.hpp`
- Create: `include/hpactor/metrics/metrics_config.hpp`
- Modify: `include/hpactor/types/types.hpp`

- [ ] **Step 1: Create `metrics_event.hpp`**

Create `include/hpactor/metrics/metrics_event.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <hpactor/ref/actor_id.hpp>

namespace hpactor {
namespace metrics {

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
};

struct alignas(32) MetricEvent {
    uint64_t        timestamp_ns;
    ActorId         actor_id;
    MetricEventType event_type;
    uint8_t         _pad[3];
    uint32_t        value_hi;
};

static_assert(sizeof(MetricEvent) == 32, "MetricEvent must be 32 bytes");

} // namespace metrics
} // namespace hpactor
```

- [ ] **Step 2: Create `metrics_config.hpp`**

Create `include/hpactor/metrics/metrics_config.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <string>

namespace hpactor {
namespace metrics {

struct MetricsConfig {
    bool        enabled              = true;
    uint32_t    ring_buffer_capacity = 65536;
    std::string metrics_path         = "/metrics";
    bool        per_actor_labels     = true;
    bool        scheduler_metrics    = true;
    bool        memory_metrics       = true;
};

} // namespace metrics
} // namespace hpactor
```

- [ ] **Step 3: Add TypeTags for metrics messages**

Read `include/hpactor/types/types.hpp` around line 439.

Add two new TypeTag values after `SystemInitTag = 12`:

```cpp
MetricsRequestTag  = 13,
MetricsResponseTag = 14,
```

The relevant section becomes:

```cpp
// TOML config bootstrapping
SystemInitTag = 12,

// Metrics subsystem
MetricsRequestTag = 13,
MetricsResponseTag = 14,

// First available user tag
User = 100,
```

- [ ] **Step 4: Build and verify**

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/metrics/metrics_event.hpp include/hpactor/metrics/metrics_config.hpp include/hpactor/types/types.hpp
git commit -m "feat: add MetricEvent, MetricEventType, MetricsConfig, and metrics TypeTags"
```

---

### Task 3: Implement `MetricRegistry` — Counter, Gauge, Histogram

**Files:**
- Create: `include/hpactor/metrics/metrics_registry.hpp`
- Create: `src/metrics/metrics_registry.cpp`

- [ ] **Step 1: Write the registry header**

Create `include/hpactor/metrics/metrics_registry.hpp`:

```cpp
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace hpactor {
namespace metrics {

enum class MetricType { kCounter, kGauge, kHistogram };

struct alignas(64) CounterValue {
    std::atomic<uint64_t> total{0};
};

struct alignas(64) GaugeValue {
    std::atomic<int64_t> value{0};
};

static constexpr size_t kHistogramNumBuckets = 16;
static constexpr double kBucketBoundsSec[kHistogramNumBuckets - 1] = {
    0.001, 0.002, 0.004, 0.008, 0.016, 0.032, 0.064,
    0.128, 0.256, 0.512, 1.024, 2.048, 4.096, 8.192, 16.384
};

struct alignas(64) HistogramValue {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> sum_ns{0};
    std::atomic<uint64_t> buckets[kHistogramNumBuckets]{};

    // Insert a latency observation in nanoseconds. Thread-safe.
    void observe(uint64_t value_ns) noexcept;
};

// LabelSet wraps metric name + key=value labels for uniqueness.
// Strings are owned — acceptable since labels are created once during drain.
struct LabelSet {
    std::string metric_name;
    std::vector<std::pair<std::string, std::string>> labels;

    bool operator==(const LabelSet& other) const;
};

struct LabelSetHash {
    size_t operator()(const LabelSet& ls) const;
};

struct MetricFamily {
    std::string name;
    std::string help;
    MetricType  type;
    std::unordered_map<LabelSet,
        std::variant<CounterValue, GaugeValue, HistogramValue>,
        LabelSetHash> values;
};

class MetricRegistry {
public:
    MetricFamily& register_family(std::string name, std::string help, MetricType type);

    template <typename V>
    V& get_or_create(MetricFamily& family, const LabelSet& labels);

    struct Snapshot {
        struct HistogramEntry {
            LabelSet labels;
            uint64_t count;
            double   sum_seconds;
            uint64_t buckets[kHistogramNumBuckets];
        };
        struct FamilySnapshot {
            std::string name;
            std::string help;
            MetricType type;
            std::vector<std::pair<LabelSet, uint64_t>> counters;
            std::vector<std::pair<LabelSet, int64_t>> gauges;
            std::vector<HistogramEntry> histograms;
        };
        std::vector<FamilySnapshot> families;
    };

    Snapshot snapshot() const;

private:
    std::vector<std::unique_ptr<MetricFamily>> families_;
    std::unordered_map<std::string, MetricFamily*> family_index_;
};

} // namespace metrics
} // namespace hpactor
```

- [ ] **Step 2: Write the registry implementation**

Create `src/metrics/metrics_registry.cpp`:

```cpp
#include <hpactor/metrics/metrics_registry.hpp>

namespace hpactor {
namespace metrics {

// HistogramValue::observe — exponential bucket insertion via clz
void HistogramValue::observe(uint64_t value_ns) noexcept {
    uint64_t ms = value_ns / 1'000'000;
    // bucket index = number of bits needed to represent ms
    int idx = 0;
    if (ms > 1) {
        idx = 64 - __builtin_clzll(ms - 1);
        if (idx < 0) idx = 0;
    }
    if (idx >= static_cast<int>(kHistogramNumBuckets)) {
        idx = kHistogramNumBuckets - 1;
    }
    buckets[idx].fetch_add(1, std::memory_order_relaxed);
    count.fetch_add(1, std::memory_order_relaxed);
    sum_ns.fetch_add(value_ns, std::memory_order_relaxed);
}

bool LabelSet::operator==(const LabelSet& other) const {
    if (metric_name != other.metric_name) return false;
    return labels == other.labels;
}

size_t LabelSetHash::operator()(const LabelSet& ls) const {
    size_t h = std::hash<std::string>()(ls.metric_name);
    for (const auto& [k, v] : ls.labels) {
        h ^= std::hash<std::string>()(k) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>()(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
}

MetricFamily& MetricRegistry::register_family(std::string name, std::string help,
                                                MetricType type) {
    auto it = family_index_.find(name);
    if (it != family_index_.end()) {
        return *it->second;
    }
    auto family = std::make_unique<MetricFamily>();
    family->name = name;
    family->help = std::move(help);
    family->type = type;
    MetricFamily* ptr = family.get();
    family_index_[name] = ptr;
    families_.push_back(std::move(family));
    return *ptr;
}

template <typename V>
V& MetricRegistry::get_or_create(MetricFamily& family, const LabelSet& labels) {
    auto it = family.values.find(labels);
    if (it != family.values.end()) {
        return std::get<V>(it->second);
    }
    auto [inserted, _] = family.values.emplace(labels, V{});
    return std::get<V>(inserted->second);
}

// Explicit instantiation for each value type
template CounterValue& MetricRegistry::get_or_create<CounterValue>(MetricFamily&, const LabelSet&);
template GaugeValue& MetricRegistry::get_or_create<GaugeValue>(MetricFamily&, const LabelSet&);
template HistogramValue& MetricRegistry::get_or_create<HistogramValue>(MetricFamily&, const LabelSet&);

MetricRegistry::Snapshot MetricRegistry::snapshot() const {
    Snapshot snap;
    for (const auto& family : families_) {
        Snapshot::FamilySnapshot fs;
        fs.name = family->name;
        fs.help = family->help;
        fs.type = family->type;

        for (const auto& [ls, val] : family->values) {
            switch (family->type) {
            case MetricType::kCounter: {
                uint64_t v = std::get<CounterValue>(val).total.load(std::memory_order_relaxed);
                fs.counters.emplace_back(ls, v);
                break;
            }
            case MetricType::kGauge: {
                int64_t v = std::get<GaugeValue>(val).value.load(std::memory_order_relaxed);
                fs.gauges.emplace_back(ls, v);
                break;
            }
            case MetricType::kHistogram: {
                const auto& hv = std::get<HistogramValue>(val);
                Snapshot::HistogramEntry he;
                he.labels = ls;
                he.count = hv.count.load(std::memory_order_relaxed);
                he.sum_seconds = static_cast<double>(hv.sum_ns.load(std::memory_order_relaxed)) / 1e9;
                for (size_t i = 0; i < kHistogramNumBuckets; ++i) {
                    he.buckets[i] = hv.buckets[i].load(std::memory_order_relaxed);
                }
                fs.histograms.push_back(std::move(he));
                break;
            }
            }
        }
        snap.families.push_back(std::move(fs));
    }
    return snap;
}

} // namespace metrics
} // namespace hpactor
```

- [ ] **Step 3: Add source file to CMakeLists.txt**

Read `CMakeLists.txt` around line 159. Add to the source list:

```
src/metrics/metrics_registry.cpp
```

Note: create the `src/metrics/` directory first if it doesn't exist.

- [ ] **Step 4: Build and verify**

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 5: Write and run unit test**

Create `tests/metrics/test_metrics_registry.cpp`:

```cpp
#include <hpactor/metrics/metrics_registry.hpp>
#include <cassert>
#include <cstdio>

int main() {
    using namespace hpactor::metrics;

    // Test Counter
    MetricRegistry reg;
    auto& family = reg.register_family("test_counter", "help", MetricType::kCounter);
    CounterValue& c = reg.get_or_create<CounterValue>(family, LabelSet{});
    c.total.fetch_add(5, std::memory_order_relaxed);
    auto snap = reg.snapshot();
    assert(snap.families.size() == 1);
    assert(snap.families[0].counters.size() == 1);
    assert(snap.families[0].counters[0].second == 5);

    // Test Gauge
    auto& gfamily = reg.register_family("test_gauge", "help", MetricType::kGauge);
    GaugeValue& g = reg.get_or_create<GaugeValue>(gfamily, LabelSet{});
    g.value.fetch_add(3, std::memory_order_relaxed);
    g.value.fetch_sub(1, std::memory_order_relaxed);
    snap = reg.snapshot();
    // find the gauge family
    for (auto& fs : snap.families) {
        if (fs.name == "test_gauge") {
            assert(fs.gauges[0].second == 2);
        }
    }

    // Test Histogram
    auto& hfamily = reg.register_family("test_hist", "help", MetricType::kHistogram);
    HistogramValue& h = reg.get_or_create<HistogramValue>(hfamily, LabelSet{});
    h.observe(5'000'000);  // 5ms → bucket index 4 or 5 depending on clz
    snap = reg.snapshot();
    for (auto& fs : snap.families) {
        if (fs.name == "test_hist") {
            assert(fs.histograms[0].count == 1);
            assert(fs.histograms[0].sum_seconds > 0.0);
        }
    }

    printf("test_metrics_registry: PASSED\n");
    return 0;
}
```

Add to `tests/CMakeLists.txt` or root CMakeLists under the test target.

Run: `ninja -C build && ./build/tests/test_metrics_registry`

Expected: `PASSED`

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/metrics/metrics_registry.hpp src/metrics/metrics_registry.cpp tests/metrics/test_metrics_registry.cpp CMakeLists.txt
git commit -m "feat: add MetricRegistry with Counter, Gauge, and Histogram support"
```

---

### Task 4: Implement `Aggregator` — Event-to-Metric Dispatch

**Files:**
- Create: `include/hpactor/metrics/metrics_aggregator.hpp`
- Create: `src/metrics/metrics_aggregator.cpp`

- [ ] **Step 1: Write the aggregator header**

Create `include/hpactor/metrics/metrics_aggregator.hpp`:

```cpp
#pragma once

#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_registry.hpp>
#include <hpactor/ref/actor_id.hpp>
#include <string>
#include <unordered_map>

namespace hpactor {
class ActorSystem;
} // namespace hpactor

namespace hpactor {
namespace metrics {

class Aggregator {
public:
    Aggregator(MetricRegistry& registry, ActorSystem& system);

    void on_event(const MetricEvent& e);

    void begin_drain();
    void end_drain();

private:
    MetricRegistry& registry_;
    ActorSystem&    system_;

    // Pre-resolved MetricFamily references
    MetricFamily* mailbox_depth_family_     = nullptr;
    MetricFamily* mailbox_messages_family_  = nullptr;
    MetricFamily* processing_latency_family_ = nullptr;
    MetricFamily* lifecycle_family_         = nullptr;
    MetricFamily* scheduler_dispatch_family_ = nullptr;
    MetricFamily* scheduler_steal_family_   = nullptr;
    MetricFamily* supervisor_restart_family_ = nullptr;
    MetricFamily* memory_bytes_family_      = nullptr;

    int64_t active_actors_{0};

    // actor_id → type_name cache (populated lazily during drain)
    mutable std::unordered_map<ActorId, std::string> actor_type_cache_;

    void ensure_families_registered();
    LabelSet make_actor_labels(ActorId id);
};

} // namespace metrics
} // namespace hpactor
```

- [ ] **Step 2: Write the aggregator implementation**

Create `src/metrics/metrics_aggregator.cpp`:

```cpp
#include <hpactor/metrics/metrics_aggregator.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/actor/abstract_actor.hpp>
#include <string>

namespace hpactor {
namespace metrics {

Aggregator::Aggregator(MetricRegistry& registry, ActorSystem& system)
    : registry_(registry), system_(system) {}

void Aggregator::ensure_families_registered() {
    if (mailbox_depth_family_) return;  // already done
    mailbox_depth_family_      = &registry_.register_family(
        "hpactor_mailbox_depth", "Current mailbox queue depth.", MetricType::kGauge);
    mailbox_messages_family_   = &registry_.register_family(
        "hpactor_mailbox_messages_total", "Total messages enqueued.", MetricType::kCounter);
    processing_latency_family_ = &registry_.register_family(
        "hpactor_message_processing_seconds", "Message processing latency.",
        MetricType::kHistogram);
    lifecycle_family_          = &registry_.register_family(
        "hpactor_actor_lifecycle_total", "Actor lifecycle events.", MetricType::kCounter);
    scheduler_dispatch_family_ = &registry_.register_family(
        "hpactor_scheduler_dispatches_total", "Scheduler dispatches.", MetricType::kCounter);
    scheduler_steal_family_    = &registry_.register_family(
        "hpactor_scheduler_steals_total", "Work steals.", MetricType::kCounter);
    supervisor_restart_family_ = &registry_.register_family(
        "hpactor_supervisor_restarts_total", "Actor restarts.", MetricType::kCounter);
    memory_bytes_family_       = &registry_.register_family(
        "hpactor_memory_active_bytes", "Active allocated bytes.", MetricType::kGauge);
}

LabelSet Aggregator::make_actor_labels(ActorId id) {
    LabelSet ls;
    std::string type_name;

    auto it = actor_type_cache_.find(id);
    if (it != actor_type_cache_.end()) {
        type_name = it->second;
    } else {
        auto actor = system_.get_actor(id);
        type_name = actor ? std::string(actor->type_name()) : "unknown";
        actor_type_cache_[id] = type_name;
    }

    ls.labels.emplace_back("actor_id", std::to_string(id.value()));
    ls.labels.emplace_back("actor_type", type_name);
    return ls;
}

void Aggregator::begin_drain() {
    ensure_families_registered();
}

void Aggregator::end_drain() {
    // Write derived active_actors gauge
    LabelSet ls;
    auto& active_family = registry_.register_family(
        "hpactor_actors_active", "Number of currently active actors.", MetricType::kGauge);
    auto& g = registry_.get_or_create<GaugeValue>(active_family, ls);
    g.value.store(active_actors_, std::memory_order_relaxed);
}

void Aggregator::on_event(const MetricEvent& e) {
    ensure_families_registered();

    switch (e.event_type) {
    case MetricEventType::kMailboxEnqueue: {
        auto lb = make_actor_labels(e.actor_id);
        {
            auto& g = registry_.get_or_create<GaugeValue>(*mailbox_depth_family_, lb);
            g.value.fetch_add(1, std::memory_order_relaxed);
        }
        {
            auto& c = registry_.get_or_create<CounterValue>(*mailbox_messages_family_, lb);
            c.total.fetch_add(1, std::memory_order_relaxed);
        }
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
        h.observe(e.value_hi);
        break;
    }
    case MetricEventType::kActorSpawned:
        active_actors_++;
        [[fallthrough]];
    case MetricEventType::kActorTerminated: {
        if (e.event_type == MetricEventType::kActorTerminated)
            active_actors_--;
        auto& c = registry_.get_or_create<CounterValue>(
            *lifecycle_family_, make_actor_labels(e.actor_id));
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kSchedulerDispatch: {
        LabelSet ls;
        ls.labels.emplace_back("worker_id", std::to_string(e.value_hi));
        auto& c = registry_.get_or_create<CounterValue>(*scheduler_dispatch_family_, ls);
        c.total.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case MetricEventType::kSchedulerSteal: {
        LabelSet ls;
        ls.labels.emplace_back("source_worker", std::to_string(e.value_hi));
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

} // namespace metrics
} // namespace hpactor
```

- [ ] **Step 3: Add source file to CMakeLists.txt**

Add `src/metrics/metrics_aggregator.cpp` to CMakeLists.txt.

- [ ] **Step 4: Temporarily add a forward declaration header or ensure build**

We need `ActorSystem::get_actor()`. Check `include/hpactor/core/actor_system.hpp` that it's declared and returns `std::shared_ptr<AbstractActor>`. It is (line 178 of `actor_system.hpp`: `std::shared_ptr<AbstractActor> get_actor(ActorId id)`).

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 5: Write unit test**

Create `tests/metrics/test_metrics_aggregator.cpp`:

```cpp
#include <hpactor/metrics/metrics_aggregator.hpp>
#include <hpactor/metrics/metrics_registry.hpp>
#include <hpactor/core/actor_system.hpp>
#include <cassert>
#include <cstdio>

int main() {
    // Minimal test: verify aggregator handles events without crashing
    using namespace hpactor::metrics;

    // We need an ActorSystem to construct Aggregator. Skip for now if
    // ActorSystem has heavy construction; instead test the event dispatch
    // logic by constructing the Aggregator with a mock/stub.

    // For the initial implementation, this is tested by the integration
    // test (Task 13). Here we just verify compilation and the registry
    // interaction.

    MetricRegistry reg;
    // Aggregator aggr(reg, system);  -- needs system, defer to integration test

    printf("test_metrics_aggregator: SKIP (needs ActorSystem, tested in integration)\n");
    return 0;
}
```

Add to CMakeLists.txt.

Run: `ninja -C build && ./build/tests/test_metrics_aggregator`

Expected: `SKIP` message.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/metrics/metrics_aggregator.hpp src/metrics/metrics_aggregator.cpp tests/metrics/test_metrics_aggregator.cpp CMakeLists.txt
git commit -m "feat: add Aggregator for event-to-metric dispatch"
```

---

### Task 5: Implement `OpenMetricsFormatter`

**Files:**
- Create: `include/hpactor/metrics/metrics_formatter.hpp`
- Create: `src/metrics/metrics_formatter.cpp`

- [ ] **Step 1: Write the formatter header**

Create `include/hpactor/metrics/metrics_formatter.hpp`:

```cpp
#pragma once

#include <hpactor/metrics/metrics_registry.hpp>
#include <string>

namespace hpactor {
namespace metrics {

class OpenMetricsFormatter {
public:
    std::string format(const MetricRegistry::Snapshot& snapshot) const;

private:
    static std::string format_labels(const LabelSet& ls);
    static std::string escape_label_value(const std::string& s);
    static constexpr double kBucketBounds[15] = {
        0.001, 0.002, 0.004, 0.008, 0.016, 0.032, 0.064,
        0.128, 0.256, 0.512, 1.024, 2.048, 4.096, 8.192, 16.384
    };
};

} // namespace metrics
} // namespace hpactor
```

- [ ] **Step 2: Write the formatter implementation**

Create `src/metrics/metrics_formatter.cpp`:

```cpp
#include <hpactor/metrics/metrics_formatter.hpp>
#include <cstdio>

namespace hpactor {
namespace metrics {

std::string OpenMetricsFormatter::escape_label_value(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        default:   out += c;
        }
    }
    return out;
}

std::string OpenMetricsFormatter::format_labels(const LabelSet& ls) {
    if (ls.labels.empty()) return "";
    std::string out = "{";
    for (size_t i = 0; i < ls.labels.size(); ++i) {
        if (i > 0) out += ",";
        out += ls.labels[i].first;
        out += "=\"";
        out += escape_label_value(ls.labels[i].second);
        out += "\"";
    }
    out += "}";
    return out;
}

std::string OpenMetricsFormatter::format(const MetricRegistry::Snapshot& snapshot) const {
    std::string out;
    out.reserve(8192);

    char buf[256];

    for (const auto& fam : snapshot.families) {
        out += "# HELP ";
        out += fam.name;
        out += " ";
        out += fam.help;
        out += "\n";

        switch (fam.type) {
        case MetricType::kCounter: {
            out += "# TYPE ";
            out += fam.name;
            out += " counter\n";
            for (const auto& [ls, val] : fam.counters) {
                int n = snprintf(buf, sizeof(buf), "%s%s %llu\n",
                                 fam.name.c_str(), format_labels(ls).c_str(),
                                 (unsigned long long)val);
                out.append(buf, static_cast<size_t>(n));
            }
            break;
        }
        case MetricType::kGauge: {
            out += "# TYPE ";
            out += fam.name;
            out += " gauge\n";
            for (const auto& [ls, val] : fam.gauges) {
                int n = snprintf(buf, sizeof(buf), "%s%s %lld\n",
                                 fam.name.c_str(), format_labels(ls).c_str(),
                                 (long long)val);
                out.append(buf, static_cast<size_t>(n));
            }
            break;
        }
        case MetricType::kHistogram: {
            out += "# TYPE ";
            out += fam.name;
            out += " histogram\n";
            for (const auto& he : fam.histograms) {
                std::string base_label_str = format_labels(he.labels);
                // Per-bucket lines
                uint64_t cumulative = 0;
                for (size_t i = 0; i < kHistogramNumBuckets - 1; ++i) {
                    cumulative += he.buckets[i];
                    int n = snprintf(buf, sizeof(buf),
                                     "%s_bucket%sle=\"%g\"} %llu\n",
                                     fam.name.c_str(),
                                     he.labels.empty() ? "{" : base_label_str.substr(0, base_label_str.size() - 1) + ",",
                                     kBucketBounds[i],
                                     (unsigned long long)cumulative);
                    out.append(buf, static_cast<size_t>(n));
                }
                // +Inf bucket
                cumulative += he.buckets[kHistogramNumBuckets - 1];
                {
                    int n = snprintf(buf, sizeof(buf),
                                     "%s_bucket%sle=\"+Inf\"} %llu\n",
                                     fam.name.c_str(),
                                     he.labels.empty() ? "{" : base_label_str.substr(0, base_label_str.size() - 1) + ",",
                                     (unsigned long long)cumulative);
                    out.append(buf, static_cast<size_t>(n));
                }
                // _sum and _count
                {
                    int n = snprintf(buf, sizeof(buf), "%s_sum%s %g\n",
                                     fam.name.c_str(), base_label_str.c_str(),
                                     he.sum_seconds);
                    out.append(buf, static_cast<size_t>(n));
                }
                {
                    int n = snprintf(buf, sizeof(buf), "%s_count%s %llu\n",
                                     fam.name.c_str(), base_label_str.c_str(),
                                     (unsigned long long)he.count);
                    out.append(buf, static_cast<size_t>(n));
                }
            }
            break;
        }
        }
        out += "\n";
    }
    out += "# EOF\n";
    return out;
}

} // namespace metrics
} // namespace hpactor
```

**Fix the `_bucket` label formatting.** The format_labels helper needs an `extra_label` parameter to handle `le="..."` appended to existing labels. Refactor as:

```cpp
static std::string format_labels_with_extra(const LabelSet& ls,
                                            const std::string& extra_key,
                                            const std::string& extra_val);
```

This is an implementation detail — implement it during the coding step.

- [ ] **Step 3: Add to CMakeLists.txt and build**

Add `src/metrics/metrics_formatter.cpp` to CMakeLists.txt.

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 4: Write and run unit test**

Create `tests/metrics/test_metrics_formatter.cpp`. Build a `MetricRegistry::Snapshot` with one counter, one gauge, one histogram. Call `formatter.format(snapshot)`. Assert output contains `# HELP`, `# TYPE`, `_total`, `_bucket`, `_sum`, `_count`, `# EOF`.

Run: `ninja -C build && ./build/tests/test_metrics_formatter`

Expected: `PASSED`

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/metrics/metrics_formatter.hpp src/metrics/metrics_formatter.cpp tests/metrics/test_metrics_formatter.cpp CMakeLists.txt
git commit -m "feat: add OpenMetricsFormatter for snapshot-to-text serialization"
```

---

### Task 6: Implement `MetricsActor`

**Files:**
- Create: `include/hpactor/metrics/metrics_actor.hpp`
- Create: `src/metrics/metrics_actor.cpp`
- Modify: `protos/hpactor/common.proto`

- [ ] **Step 1: Add protobuf messages for MetricsRequest/MetricsResponse**

Read `protos/hpactor/common.proto`. Add at the end:

```protobuf
message MetricsRequest {
    // Empty — triggers full ring buffer drain + snapshot + reply.
}

message MetricsResponse {
    bytes body = 1;  // OpenMetrics formatted text.
}
```

Regenerate protobuf codegen:
```bash
# Check cmake/protobuf.cmake for the generation command.
# Typically: ninja -C build regenerates proto files automatically.
ninja -C build
```

- [ ] **Step 2: Write MetricsActor header**

Create `include/hpactor/metrics/metrics_actor.hpp`:

```cpp
#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/metrics/metrics_aggregator.hpp>
#include <hpactor/metrics/metrics_formatter.hpp>
#include <hpactor/metrics/metrics_registry.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <memory>

namespace hpactor {
namespace metrics {

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

} // namespace metrics
} // namespace hpactor
```

- [ ] **Step 3: Write MetricsActor implementation**

Create `src/metrics/metrics_actor.cpp`:

```cpp
#include <hpactor/metrics/metrics_actor.hpp>
#include <hpactor/proto/messages.pb.h>  // for MetricsResponse
#include <hpactor/proto/common.pb.h>    // for MetricsRequest

namespace hpactor {
namespace metrics {

MetricsActor::MetricsActor(ActorSystem& system,
                           std::shared_ptr<MpscRingBuffer<MetricEvent>> ring_buffer)
    : ring_buffer_(std::move(ring_buffer))
    , aggregator_(registry_, system) {}

void MetricsActor::register_handlers() {
    on_request<MetricsRequest, MetricsResponse>(
        [this](const MetricsRequest& /*req*/) -> MetricsResponse {
            // Drain ring buffer
            aggregator_.begin_drain();
            ring_buffer_->drain([this](const MetricEvent& e) {
                aggregator_.on_event(e);
                return true;
            });
            aggregator_.end_drain();

            // Track lost events
            events_lost_ += ring_buffer_->events_lost();

            // Snapshot and format
            auto snapshot = registry_.snapshot();
            std::string body = formatter_.format(snapshot);

            // Append events_lost metric
            char buf[128];
            int n = snprintf(buf, sizeof(buf),
                "# HELP hpactor_metrics_events_lost_total Events lost due to ring buffer overflow.\n"
                "# TYPE hpactor_metrics_events_lost_total counter\n"
                "hpactor_metrics_events_lost_total %llu\n",
                (unsigned long long)events_lost_);
            body.append(buf, static_cast<size_t>(n));

            MetricsResponse resp;
            resp.set_body(body);
            return resp;
        });
}

} // namespace metrics
} // namespace hpactor
```

- [ ] **Step 4: Add to CMakeLists.txt and build**

Add `src/metrics/metrics_actor.cpp` to CMakeLists.txt.

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/metrics/metrics_actor.hpp src/metrics/metrics_actor.cpp protos/hpactor/common.proto CMakeLists.txt
git commit -m "feat: add MetricsActor with protobuf request/response support"
```

---

### Task 7: Add `type_name()` to `AbstractActor` and ActorFactoryRegistry

**Files:**
- Modify: `include/hpactor/actor/abstract_actor.hpp`
- Modify: `include/hpactor/config/actor_factory_registry.hpp` (if needed)
- Modify: `include/hpactor/core/actor_system.hpp` (spawn sets type_name)

- [ ] **Step 1: Add virtual `type_name()` to AbstractActor**

Read `include/hpactor/actor/abstract_actor.hpp` around line 97 (after `dispatch_hints`).

Add:

```cpp
// Returns the actor's type name (e.g., "EchoActor", "MetricsActor").
// Used by the metrics subsystem for labeling.
// Default returns empty; subclasses may override or set via set_type_name().
virtual std::string_view type_name() const { return type_name_; }

void set_type_name(std::string name) { type_name_ = std::move(name); }

private:
    std::string type_name_;
```

- [ ] **Step 2: Set type_name during spawn**

In `include/hpactor/core/actor_system.hpp`, in `spawn<T>()` (around line 308, after `set_address`):

```cpp
// Set type name for metrics labeling
if constexpr (requires { T::kActorTypeName; }) {
    actor->set_type_name(T::kActorTypeName);
} else {
    actor->set_type_name("unknown");
}
```

Actors that want a custom type name can add `static constexpr const char* kActorTypeName = "MyActor";`.

- [ ] **Step 3: Build and verify**

Run: `ninja -C build`

Expected: clean build. All 88 tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/abstract_actor.hpp include/hpactor/core/actor_system.hpp
git commit -m "feat: add type_name() to AbstractActor for metrics labeling"
```

---

### Task 8: Integrate Metrics into Mailbox

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

- [ ] **Step 1: Add metrics_ring_buffer_ pointer and setter to mailbox**

Read `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`.

Add to private members (after `continuation_callback_` at line 111):

```cpp
metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_ = nullptr;
```

Add setter method:

```cpp
void set_metrics_ring_buffer(metrics::MpscRingBuffer<metrics::MetricEvent>* buf) noexcept {
    metrics_ring_buffer_ = buf;
}
```

Add include: `#include <hpactor/metrics/metrics_ring_buffer.hpp>` at top.

- [ ] **Step 2: Emit enqueue/dequeue events**

In `enqueue()` (around line 40), add after the mailbox enqueue:

```cpp
void enqueue(T* node) noexcept {
    bool was_empty = empty();
    mailbox_.enqueue(node);
    // ... existing logic ...

    // Metrics: emit kMailboxEnqueue event
    if (metrics_ring_buffer_) [[unlikely]] {
        auto* slot = metrics_ring_buffer_->reserve();
        if (slot) [[likely]] {
            slot->actor_id = actor_id_;
            slot->event_type = metrics::MetricEventType::kMailboxEnqueue;
            slot->value_hi = 1;
        }
    }
}
```

In `dequeue()` (around line 67), add at the end before return:

```cpp
if (metrics_ring_buffer_) [[unlikely]] {
    auto* slot = metrics_ring_buffer_->reserve();
    if (slot) [[likely]] {
        slot->actor_id = actor_id_;
        slot->event_type = metrics::MetricEventType::kMailboxDequeue;
        slot->value_hi = 1;
    }
}
```

- [ ] **Step 3: Build and verify**

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp
git commit -m "feat: integrate metrics into MPSCActorMailbox (enqueue/dequeue events)"
```

---

### Task 9: Integrate Metrics into EventBasedActor (Latency + Lifecycle)

**Files:**
- Modify: `include/hpactor/actor/event_based_actor.hpp`
- Modify: `src/actor/event_based_actor.cpp`

- [ ] **Step 1: Add metrics_ring_buffer_ to EventBasedActor**

Read `include/hpactor/actor/event_based_actor.hpp`. Find the private members section. Add:

```cpp
metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_ = nullptr;
```

Add public setter:

```cpp
void set_metrics_ring_buffer(metrics::MpscRingBuffer<metrics::MetricEvent>* buf) noexcept {
    metrics_ring_buffer_ = buf;
}
```

Add `#include <hpactor/metrics/metrics_ring_buffer.hpp>` at top.

- [ ] **Step 2: Emit kMessageProcessed in receive()**

Read `src/actor/event_based_actor.cpp` — the `receive()` method (around lines 28-98). After `behavior_->handle(msg)` returns:

```cpp
result<void> EventBasedActor::receive(TypedMessage& msg) {
    // ... system message handling on msg.type_tag() ...

    // Capture start time for metrics
    auto t0 = metrics_ring_buffer_
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};

    auto result = behavior_->handle(msg);

    // Emit kMessageProcessed event
    if (metrics_ring_buffer_) [[unlikely]] {
        auto t1 = std::chrono::steady_clock::now();
        auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        auto* slot = metrics_ring_buffer_->reserve();
        if (slot) [[likely]] {
            slot->actor_id = id();
            slot->event_type = metrics::MetricEventType::kMessageProcessed;
            slot->value_hi = static_cast<uint32_t>(
                latency_ns > UINT32_MAX ? UINT32_MAX : latency_ns);
        }
    }
    return result;
}
```

Add `#include <chrono>` if not already present.

- [ ] **Step 3: Emit kActorTerminated in on_exit()**

Read `src/actor/event_based_actor.cpp` lines 176-201 (`on_exit()`). At the top of the function, before building DownMsg:

```cpp
void EventBasedActor::on_exit() {
    auto* ctx = context();
    if (ctx == nullptr) { return; }

    // Emit kActorTerminated event
    if (metrics_ring_buffer_) [[unlikely]] {
        auto* slot = metrics_ring_buffer_->reserve();
        if (slot) [[likely]] {
            slot->actor_id = id();
            slot->event_type = metrics::MetricEventType::kActorTerminated;
            slot->value_hi = static_cast<uint32_t>(exit_reason_);
        }
    }

    // ... existing DownMsg logic ...
}
```

- [ ] **Step 4: Build and verify**

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/actor/event_based_actor.hpp src/actor/event_based_actor.cpp
git commit -m "feat: integrate metrics into EventBasedActor (latency + lifecycle events)"
```

---

### Task 10: Integrate Metrics into ActorSystem (Spawn + MetricsActor wire-up)

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add MetricsConfig to Config and metrics_ring_buffer_ to ActorSystem**

Read `include/hpactor/core/actor_system.hpp`.

Add `#include <hpactor/metrics/metrics_config.hpp>` at top.

In `Config` struct (after `use_coroutines` at line 88), add:

```cpp
// Metrics subsystem
metrics::MetricsConfig metrics;
```

In `ActorSystem` private members, add:

```cpp
std::shared_ptr<metrics::MpscRingBuffer<metrics::MetricEvent>> metrics_ring_buffer_;
```

Add public getter:

```cpp
auto* metrics_ring_buffer() const { return metrics_ring_buffer_.get(); }
```

- [ ] **Step 2: Create MetricsRingBuffer and MetricsActor during init**

Read `src/actor/actor_system.cpp`. Find the initialization method (likely `init()` or the constructor body).

Add after HTTP gateway initialization:

```cpp
// Initialize metrics subsystem
if (config_.metrics.enabled) {
    metrics_ring_buffer_ = std::make_shared<metrics::MpscRingBuffer<metrics::MetricEvent>>(
        config_.metrics.ring_buffer_capacity);

    auto metrics_actor = spawn<metrics::MetricsActor>(*this, metrics_ring_buffer_);

    if (http_gateway_actor_) {
        http_gateway_actor_->route(
            HttpMethod::GET, config_.metrics.metrics_path,
            [metrics_actor](const HttpRequest& req) {
                metrics::MetricsRequest unused;
                TypedMessage msg;
                msg.set_type_tag(TypeTag::MetricsRequestTag);
                msg.set_payload(unused.SerializeAsString());
                return std::make_pair(metrics_actor->address(), std::move(msg));
            });
    }
}
```

- [ ] **Step 3: Emit kActorSpawned in spawn()**

Read `include/hpactor/core/actor_system.hpp` lines 304-354 (`spawn<T>()`).

After `actor->on_activate()` at line 351, add:

```cpp
// Emit kActorSpawned event for metrics
if (metrics_ring_buffer_) [[unlikely]] {
    auto* slot = metrics_ring_buffer_->reserve();
    if (slot) [[likely]] {
        slot->actor_id = id;
        slot->event_type = metrics::MetricEventType::kActorSpawned;
        slot->value_hi = 1;
    }
}
```

Also in `spawn_configured()` in `src/actor/actor_system.cpp` (around line 342, after `local->on_activate()`).

- [ ] **Step 4: Pass metrics_ring_buffer_ to actors and mailboxes during spawn**

In `spawn<T>()`, after setting mailbox (line 330), add:

```cpp
if (metrics_ring_buffer_) [[unlikely]] {
    auto* mbox = mailboxes_[id].get();
    mbox->set_metrics_ring_buffer(metrics_ring_buffer_.get());
    actor->set_metrics_ring_buffer(metrics_ring_buffer_.get());
}
```

- [ ] **Step 5: Build and verify**

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat: wire metrics subsystem into ActorSystem (spawn events, MetricsActor, /metrics route)"
```

---

### Task 11: Integrate Metrics into Scheduler (Dispatch + Steal events)

**Files:**
- Modify: `src/sched/scheduler.cpp`
- Modify: `include/hpactor/sched/scheduler.hpp`

- [ ] **Step 1: Add metrics_ring_buffer_ to HybridScheduler**

Read `include/hpactor/sched/scheduler.hpp`. Add private member:

```cpp
metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_ = nullptr;
```

Add include for `#include <hpactor/metrics/metrics_ring_buffer.hpp>` at top.

Add public setter:

```cpp
void set_metrics_ring_buffer(metrics::MpscRingBuffer<metrics::MetricEvent>* buf) noexcept {
    metrics_ring_buffer_ = buf;
}
```

- [ ] **Step 2: Emit dispatch event in execute_actor()**

Read `src/sched/scheduler.cpp`. Find `execute_actor()` (around line 210).

After the actor is picked up for execution, add:

```cpp
if (metrics_ring_buffer_) [[unlikely]] {
    auto* slot = metrics_ring_buffer_->reserve();
    if (slot) [[likely]] {
        slot->actor_id = item.actor_id;
        slot->event_type = metrics::MetricEventType::kSchedulerDispatch;
        slot->value_hi = worker_id;
    }
}
```

- [ ] **Step 3: Emit steal event in try_steal()**

Find `HybridScheduler::try_steal()` (around line 129).

After a successful steal, add:

```cpp
if (metrics_ring_buffer_) [[unlikely]] {
    auto* slot = metrics_ring_buffer_->reserve();
    if (slot) [[likely]] {
        slot->actor_id = out.actor_id;
        slot->event_type = metrics::MetricEventType::kSchedulerSteal;
        slot->value_hi = source_worker_id;  // the worker we stole from
    }
}
```

- [ ] **Step 4: Wire metrics_ring_buffer_ from ActorSystem**

In the ActorSystem initialization, after creating `metrics_ring_buffer_`, pass it to the scheduler:

```cpp
if (metrics_ring_buffer_) {
    scheduler_->set_metrics_ring_buffer(metrics_ring_buffer_.get());
}
```

- [ ] **Step 5: Build and verify**

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp src/actor/actor_system.cpp
git commit -m "feat: integrate metrics into HybridScheduler (dispatch + steal events)"
```

---

### Task 12: Integrate Metrics into Supervision (Restart events)

**Files:**
- Modify: `src/supervision/supervision.cpp`
- Modify: `include/hpactor/supervision/supervision.hpp`

- [ ] **Step 1: Add metrics_ring_buffer_ to SupervisorActor base**

Read `include/hpactor/supervision/supervision.hpp`. Add:

```cpp
// In SupervisorActor class (around line 55)
void set_metrics_ring_buffer(metrics::MpscRingBuffer<metrics::MetricEvent>* buf) noexcept {
    metrics_ring_buffer_ = buf;
}
protected:
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_ = nullptr;
```

- [ ] **Step 2: Emit restart event**

Read `src/supervision/supervision.cpp`. Find `SupervisorActor::restart_child()` (around line 90) and `SelfSupervisingActor::decide_restart()` (around line 183).

After incrementing the restart count, add:

```cpp
if (metrics_ring_buffer_) [[unlikely]] {
    auto* slot = metrics_ring_buffer_->reserve();
    if (slot) [[likely]] {
        slot->actor_id = id();  // supervisor's own ID
        slot->event_type = metrics::MetricEventType::kSupervisorRestart;
        slot->value_hi = child_id.value();
    }
}
```

- [ ] **Step 3: Wire metrics_ring_buffer_ from spawn**

In `ActorSystem::spawn()` or wherever supervisor actors are created, if the actor is a supervisor:

```cpp
if (metrics_ring_buffer_) [[unlikely]] {
    if (auto* sup = dynamic_cast<SupervisorActor*>(actor.get())) {
        sup->set_metrics_ring_buffer(metrics_ring_buffer_.get());
    }
}
```

Wait — `-fno-rtti` means no `dynamic_cast`. Instead, add a virtual method `set_metrics_ring_buffer` to `AbstractActor` (or check if the dispatch policy supports it):

```cpp
actor->set_metrics_ring_buffer(metrics_ring_buffer_.get());
```

The base `AbstractActor::set_metrics_ring_buffer()` is a no-op. `SupervisorActor` overrides it to store the pointer.

**Add to AbstractActor** (`include/hpactor/actor/abstract_actor.hpp`):

```cpp
virtual void set_metrics_ring_buffer(void* buf) {}
```

**Override in SupervisorActor** (`include/hpactor/supervision/supervision.hpp`):

```cpp
void set_metrics_ring_buffer(void* buf) override {
    metrics_ring_buffer_ = static_cast<metrics::MpscRingBuffer<metrics::MetricEvent>*>(buf);
}
```

- [ ] **Step 4: Build and verify**

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/supervision/supervision.hpp src/supervision/supervision.cpp include/hpactor/actor/abstract_actor.hpp
git commit -m "feat: integrate metrics into supervision (restart events)"
```

---

### Task 12a: Integrate Metrics into Memory Allocator

**Files:**
- Modify: `src/mem/slab_cache.cpp` (or whichever file does `malloc`/`free` tracking)

- [ ] **Step 1: Identify the allocation hot path**

The slab allocator in `src/mem/slab_cache.cpp` calls the existing `TelemetryRingBuffer::try_push()` for memory debugging. For metrics, we co-write `kMemoryAlloc`/`kMemoryFree` events to the `MetricsRingBuffer`.

Search for the existing `TelemetryRingBuffer::try_push` call sites:
```bash
grep -rn "try_push\|TelemetryRingBuffer" src/mem/
```

- [ ] **Step 2: Accept metrics_ring_buffer_ pointer in the memory subsystem**

In the slab allocator (likely `src/mem/slab_cache.cpp` or `include/hpactor/mem/slab_cache.hpp`), add a static or member pointer:

```cpp
static metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_ = nullptr;

void set_metrics_ring_buffer(metrics::MpscRingBuffer<metrics::MetricEvent>* buf) {
    metrics_ring_buffer_ = buf;
}
```

- [ ] **Step 3: Co-write MetricEvent alongside AllocationEvent**

At each existing `try_push(alloc_event)` call site, add a co-write:

```cpp
// Existing memory debug event
mem::telemetry_buffer.try_push(alloc_event);

// Co-write to metrics ring buffer
if (metrics_ring_buffer_) [[unlikely]] {
    auto* slot = metrics_ring_buffer_->reserve();
    if (slot) [[likely]] {
        slot->actor_id = ActorId(alloc_event.actor_id);
        slot->event_type = is_free
            ? metrics::MetricEventType::kMemoryFree
            : metrics::MetricEventType::kMemoryAlloc;
        slot->value_hi = alloc_event.block_size;
    }
}
```

- [ ] **Step 4: Wire from ActorSystem init**

In `src/actor/actor_system.cpp`, after creating `metrics_ring_buffer_`, pass it to the memory subsystem:

```cpp
if (metrics_ring_buffer_) {
    mem::set_metrics_ring_buffer(metrics_ring_buffer_.get());
}
```

- [ ] **Step 5: Build and verify**

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add src/mem/slab_cache.cpp src/mem/*.cpp src/actor/actor_system.cpp
git commit -m "feat: integrate metrics into memory allocator (alloc/free events)"
```

---

### Task 13: Add TOML Configuration for Metrics

**Files:**
- Modify: `include/hpactor/config/topology_model.hpp`
- Modify: `src/config/toml_parser.cpp`

- [ ] **Step 1: Add metrics fields to SystemDef**

Read `include/hpactor/config/topology_model.hpp` lines 74-91 (`SystemDef` struct).

Add member:

```cpp
metrics::MetricsConfig metrics;
```

Add `#include <hpactor/metrics/metrics_config.hpp>` at top.

- [ ] **Step 2: Parse `[system.metrics]` in TOML parser**

Read `src/config/toml_parser.cpp` lines 225-239 (the `[system]` section parsing).

After all existing system fields are parsed, add:

```cpp
// Parse [system.metrics] table
if (auto* metrics_tbl = table["metrics"].as_table()) {
    read_bool(metrics_tbl->get("enabled"), system.metrics.enabled);
    read_uint32(metrics_tbl->get("ring_buffer_capacity"), system.metrics.ring_buffer_capacity);
    read_string(metrics_tbl->get("metrics_path"), system.metrics.metrics_path);
    read_bool(metrics_tbl->get("per_actor_labels"), system.metrics.per_actor_labels);
    read_bool(metrics_tbl->get("scheduler_metrics"), system.metrics.scheduler_metrics);
    read_bool(metrics_tbl->get("memory_metrics"), system.metrics.memory_metrics);
}
```

- [ ] **Step 3: Wire SystemDef.metrics into Config during bootstrap**

In `ActorSystem::load_topology()` (`src/actor/actor_system.cpp`, around line 350-393), after parsing the topology model and before spawning actors, copy the system metrics config:

```cpp
if (topo.system.metrics.enabled) {
    config_.metrics = topo.system.metrics;
}
```

- [ ] **Step 4: Build and verify**

Run: `ninja -C build`

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/config/topology_model.hpp src/config/toml_parser.cpp src/actor/actor_system.cpp
git commit -m "feat: add TOML [system.metrics] configuration support"
```

---

### Task 14: Add `ENABLE_ACTOR_METRICS` CMake Option + Directory Setup

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add CMake option**

In the root `CMakeLists.txt`, near other `ENABLE_*` options:

```cmake
option(ENABLE_ACTOR_METRICS "Enable actor-level metrics subsystem" ON)
```

- [ ] **Step 2: Conditionally add source files and compile definition**

When `ENABLE_ACTOR_METRICS=ON`:
- Add all `src/metrics/*.cpp` to the library source list
- Add `include/hpactor/metrics/` to the include path (already covered by the root include)
- Add compile definition: `target_compile_definitions(hpactor_lib PUBLIC HPACTOR_ENABLE_ACTOR_METRICS)`

When `ENABLE_ACTOR_METRICS=OFF`:
- Skip metrics sources
- Define `HPACTOR_DISABLE_METRICS` so that header code can `#ifdef` out metrics_ring_buffer_ members

- [ ] **Step 3: Guard metrics members in headers with #ifdef**

In every modified header that adds `metrics_ring_buffer_` pointer, wrap it:

```cpp
#ifdef HPACTOR_DISABLE_METRICS
    void* metrics_ring_buffer_ = nullptr;
#else
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer_ = nullptr;
#endif
```

This avoids the need to include the metrics headers when metrics are disabled.

- [ ] **Step 4: Build with both configurations**

```bash
cmake -S . -B build -GNinja -DENABLE_ACTOR_METRICS=ON && ninja -C build
cmake -S . -B build-no-metrics -GNinja -DENABLE_ACTOR_METRICS=OFF && ninja -C build-no-metrics
```

Expected: both configurations build cleanly.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/hpactor/mailbox/mpsc_actor_mailbox.hpp include/hpactor/actor/event_based_actor.hpp include/hpactor/core/actor_system.hpp include/hpactor/sched/scheduler.hpp include/hpactor/supervision/supervision.hpp include/hpactor/actor/abstract_actor.hpp
git commit -m "feat: add ENABLE_ACTOR_METRICS CMake option with compile-time guards"
```

---

### Task 15: Integration Test — End-to-End Metrics Pipeline

**Files:**
- Create: `tests/metrics/test_metrics_integration.cpp`

- [ ] **Step 1: Write end-to-end integration test**

Create `tests/metrics/test_metrics_integration.cpp`:

```cpp
#include <hpactor/core/actor_system.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/metrics/metrics_registry.hpp>
#include <hpactor/metrics/metrics_aggregator.hpp>
#include <hpactor/metrics/metrics_formatter.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <string>
#include <cassert>
#include <cstdio>

using namespace hpactor;
using namespace hpactor::metrics;

// Simple test actor that processes messages
class TestActor : public EventBasedActor {
public:
    void register_handlers() override {
        on<int>([this](const int& /*val*/) {
            // Simulate some work
        });
    }
};

int main() {
    ActorSystem::Config cfg;
    cfg.scheduler_threads = 1;
    cfg.metrics.enabled = true;
    cfg.metrics.ring_buffer_capacity = 1024;

    ActorSystem system(cfg);

    // Create a ring buffer and aggregator directly for testing
    auto ring_buf = std::make_shared<MpscRingBuffer<MetricEvent>>(1024);
    MetricRegistry registry;
    Aggregator aggr(registry, system);

    // Write some events
    {
        auto* slot = ring_buf->reserve();
        assert(slot != nullptr);
        slot->actor_id = ActorId(1);
        slot->event_type = MetricEventType::kMailboxEnqueue;
        slot->value_hi = 1;
    }
    {
        auto* slot = ring_buf->reserve();
        assert(slot != nullptr);
        slot->actor_id = ActorId(1);
        slot->event_type = MetricEventType::kMailboxEnqueue;
        slot->value_hi = 1;
    }
    {
        auto* slot = ring_buf->reserve();
        assert(slot != nullptr);
        slot->actor_id = ActorId(1);
        slot->event_type = MetricEventType::kMailboxDequeue;
        slot->value_hi = 1;
    }
    {
        auto* slot = ring_buf->reserve();
        assert(slot != nullptr);
        slot->actor_id = ActorId(1);
        slot->event_type = MetricEventType::kMessageProcessed;
        slot->value_hi = 5'000'000;  // 5ms
    }
    {
        auto* slot = ring_buf->reserve();
        assert(slot != nullptr);
        slot->actor_id = ActorId(1);
        slot->event_type = MetricEventType::kActorSpawned;
        slot->value_hi = 1;
    }

    // Drain and aggregate
    aggr.begin_drain();
    size_t drained = ring_buf->drain([&aggr](const MetricEvent& e) {
        aggr.on_event(e);
        return true;
    });
    aggr.end_drain();
    assert(drained == 5);

    // Snapshot and verify
    auto snap = registry.snapshot();
    assert(!snap.families.empty());

    // Find mailbox_depth family and check gauge = +2 enqueue - 1 dequeue = 1
    bool found_depth = false;
    bool found_latency = false;
    for (auto& fam : snap.families) {
        if (fam.name == "hpactor_mailbox_depth") {
            found_depth = true;
            assert(!fam.gauges.empty());
            assert(fam.gauges[0].second == 1);  // 2 enqueue, 1 dequeue
        }
        if (fam.name == "hpactor_message_processing_seconds") {
            found_latency = true;
            assert(!fam.histograms.empty());
            assert(fam.histograms[0].count == 1);
            assert(fam.histograms[0].sum_seconds > 0.0);
        }
    }
    assert(found_depth);
    assert(found_latency);

    // Format as OpenMetrics
    OpenMetricsFormatter fmt;
    std::string text = fmt.format(snap);
    assert(!text.empty());
    assert(text.find("# HELP hpactor_mailbox_depth") != std::string::npos);
    assert(text.find("# TYPE hpactor_mailbox_depth gauge") != std::string::npos);
    assert(text.find("# HELP hpactor_message_processing_seconds") != std::string::npos);
    assert(text.find("# TYPE hpactor_message_processing_seconds histogram") != std::string::npos);
    assert(text.find("_bucket{") != std::string::npos);
    assert(text.find("_sum{") != std::string::npos);
    assert(text.find("_count{") != std::string::npos);
    assert(text.find("# EOF") != std::string::npos);

    printf("test_metrics_integration: PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Add to CMakeLists.txt and build**

Add `tests/metrics/test_metrics_integration.cpp` to test targets.

- [ ] **Step 3: Run**

```bash
ninja -C build && ./build/tests/test_metrics_integration
```

Expected: `PASSED`

- [ ] **Step 4: Commit**

```bash
git add tests/metrics/test_metrics_integration.cpp CMakeLists.txt
git commit -m "test: add end-to-end metrics pipeline integration test"
```

---

### Task 16: Run Full Test Suite & Final Verification

- [ ] **Step 1: Run all tests**

```bash
ctest --output-on-failure
```

Expected: all existing 88 tests + new metrics tests pass.

- [ ] **Step 2: Manual HTTP endpoint verification (if HTTPGateway is available)**

Start the system with metrics enabled and an HTTP gateway:

```bash
./build/examples/01_echo_actor &
sleep 1
curl -s http://localhost:8080/metrics
```

Expected: OpenMetrics text output with `hpactor_mailbox_depth`, `hpactor_actor_lifecycle_total`, etc.

- [ ] **Step 3: Verify Prometheus compatibility (optional)**

```bash
curl -s http://localhost:8080/metrics | promtool check metrics
```

Expected: no errors.

- [ ] **Step 4: Commit final changes**

```bash
git add -A
git commit -m "chore: finalize actor metrics implementation — all tests passing"
```
