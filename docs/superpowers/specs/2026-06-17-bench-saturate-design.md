# HPActor Saturation Benchmark App — Design Spec

**Date:** 2026-06-17
**Status:** Design approved
**App:** `apps/bench_saturate/` — Actor System Saturation Benchmark (App 17)
**Issue:** [#313](https://github.com/skg7on/HPActor/issues/313)

## 1. Overview

A standalone performance benchmark app that probes the saturation ceiling of the
HPActor actor system. Unlike the existing `apps/bench_perf/` (app 16) which
measures latency and throughput at fixed rates, this app discovers the **maximum
sustainable message rate** by continuously increasing send throughput until the
receivers' bounded mailboxes overflow and messages are dropped.

The app exercises:

1. **Exponential saturation ramp** — double send rate each step until drop rate
   exceeds 1%, then binary search to pinpoint the exact ceiling.
2. **Allocator stress** — configurable payload modes (header-only 20B, 1KB–64KB junk,
   80/20 mixed) stress the slab allocator, system allocator fallback, and memory
   bandwidth.
3. **System probing** — detect CPU topology (P/E core split), available memory,
   cache hierarchy, scheduler thread model, and wake strategy.
4. **Dual operation modes** — interactive CLI (`/saturate` command tree) and
   headless mode (`--headless <preset>`) for CI/scripting integration.

### 1.1 Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Ramp strategy | Exponential (2×) + binary search refinement | Fast ceiling discovery; binary search pinpoints the exact saturation point |
| Payload model | Three modes: small (20B header-only), junk (1KB–64KB), mixed (80/20) | Covers envelope stress, allocator pressure, and realistic workloads |
| Overflow policy | `DropHead` on bounded mailboxes | Oldest messages dropped first; new messages most likely to carry fresh timestamps |
| Drop threshold | 1% drop rate triggers refinement | Noise-tolerant; single dropped messages don't prematurely end the ramp |
| Control surface | CLI + headless dual-mode | Matches bench_perf interactive pattern; headless for CI/automation |
| Actor model | `EventBasedActor` subclasses, header-only | Follows existing bench_perf and cli_demo patterns |
| Metrics collection | `SaturateCollectorActor` with streaming percentiles | Matches bench_perf collector pattern; 10K-sample reservoir per group |

### 1.2 Goals

1. Discover the maximum sustainable message rate (saturation ceiling) with
   configurable sender/receiver counts, payload sizes, and mailbox capacities.
2. Characterize the drop-rate curve — drops/sec vs send rate — including the
   saturation cliff shape.
3. Report latency percentiles (p50/p99/p999) at each load level to capture the
   latency/throughput tradeoff.
4. Measure allocator behavior: slab cache hit/miss ratios, system allocator
   fallbacks, per-size-class breakdown, memory pressure events.
5. Report worker thread model (polling vs CV wake) and its effect on saturation.
6. Support both interactive exploration (CLI) and automated CI execution
   (headless mode with JSON/CSV output).

### 1.3 Non-Goals

- Multi-node or cross-process benchmarking (that requires ClusterHarness).
- Long-running soak/stability testing (separate chaos/soak test lane).
- Replacing or modifying the existing `bench_perf` app.
- General-purpose HTTP load generation (not an HTTP benchmark).
- Real-time visualization dashboards (CLI and file output only).

## 2. Architecture

### 2.1 Actor Topology

```
                          ┌─────────────────────────┐
                          │  SaturateCoordinatorActor │
                          │  - ramp state machine      │
                          │  - rate broadcasting       │
                          │  - preset management       │
                          │  - result aggregation      │
                          └─────┬──────────────┬──────┘
                                │              │
                   ┌────────────┘              └────────────┐
                   ▼                                       ▼
          ┌─────────────────┐                    ┌─────────────────┐
          │ N × Saturate    │                    │ M × Saturate    │
          │ SenderActor     │───LoadMessageTag───▶│ ReceiverActor   │
          │                 │                    │                 │
          │ - self-schedule │                    │ - bounded mbx   │
          │ - rate-directed │                    │ - DropHead ovfl │
          │ - round-robin   │                    │ - atomic drops  │
          │   dispatch      │                    │ - latency calc  │
          └────────┬────────┘                    └────────┬────────┘
                   │                                      │
                   │ ThroughputSampleTag                  │ LatencySampleTag
                   │                                      │ DropReportTag
                   └──────────────────┬───────────────────┘
                                      ▼
                          ┌─────────────────────────┐
                          │ SaturateCollectorActor   │
                          │ - streaming percentiles   │
                          │ - drop-rate curve (TS)    │
                          │ - alloc stat deltas       │
                          │ - throughput rollup       │
                          └─────────────────────────┘
```

| Actor | Class | Instances | Purpose |
|-------|-------|-----------|---------|
| `SaturateCoordinatorActor` | `EventBasedActor` | 1 | Orchestrates runs: ramp state machine, rate-change broadcasting, preset management, result aggregation, CLI command handling |
| `SaturateSenderActor` | `EventBasedActor` | N (configurable) | Self-schedules ticks at coordinator-directed rate, dispatches `LoadMessageTag` messages to receiver pool via round-robin, reports throughput to collector |
| `SaturateReceiverActor` | `EventBasedActor` | M (configurable) | Bounded mailbox with `DropHead` overflow policy, atomic drop counter, latency extraction from payload timestamps, reports to collector |
| `SaturateCollectorActor` | `EventBasedActor` | 1 | Receives samples (throughput, latency, drops), computes streaming percentiles, maintains drop-rate time-series curve, captures allocator stat deltas |

### 2.2 Message Flow

1. User types `/saturate start alloc-stress` in CLI (or app starts with `--headless alloc-stress`).
2. Coordinator resets all counters, broadcasts `SaturateStartTag` with preset parameters to all senders, receivers, and collector.
3. Coordinator enters **Probing** phase, broadcasts `RateChangeTag` with initial rate.
4. Senders self-schedule via `context()->schedule()`, send `LoadMessageTag` messages to receivers at the directed rate.
5. Receivers process messages (extract latency, increment counters), overflow drops handled by `DropHead` policy with atomic drop counter.
6. Receivers periodically send `LatencySampleTag` and `DropReportTag` to collector.
7. Senders periodically send `ThroughputSampleTag` to collector.
8. Coordinator polls collector for current drop rate; when drop rate > threshold, records ceiling and enters **Refining** phase.
9. During Refining, coordinator performs binary search between last-good and first-bad rate.
10. On completion, coordinator enters **Stable** phase (hold at ceiling for observation), then **Reporting**.
11. User runs `/saturate report` to view results; `/saturate export` for JSON/CSV.
12. User runs `/saturate stop` to halt an in-progress run.

### 2.3 Ramp State Machine

```
     ┌──────┐   start    ┌─────────┐   rate_ok &&   ┌──────────┐
     │ Idle │───────────▶│ Probing │  drops>thresh  │ Refining │
     └──────┘            └─────────┘───────────────▶└──────────┘
         ▲                    │                          │
         │                    │ drops>thresh             │ refine_done
         │                    │ (no refine needed)       │
         │                    ▼                          ▼
         │               ┌──────────┐              ┌──────────┐
         │               │  Stable  │◀─────────────│  Stable  │
         │               └────┬─────┘              └────┬─────┘
         │                    │                         │
         │                    │ duration_ok             │
         │                    ▼                         ▼
         │               ┌───────────┐            ┌───────────┐
         └───────────────│ Reporting │◀───────────│ Reporting │
            stop /       └───────────┘            └───────────┘
            duration_cap
```

**Phases:**

| Phase | Behavior | Exit Condition |
|-------|----------|----------------|
| `Idle` | No activity, awaiting start | `/saturate start` received |
| `Probing` | Double rate every `step_interval_ms` (default 1000ms). Monitor drop rate from collector. | Drop rate > `drop_threshold_pct` (default 1%) |
| `Refining` | Binary search between last-good-rate and first-bad-rate. 5 iterations default (`refine_iterations`). | Refine iterations exhausted |
| `Stable` | Hold at discovered ceiling rate for observation period (default 5000ms). | Duration elapsed or `/saturate stop` |
| `Reporting` | Results available, no more messages sent. Awaiting user query or headless output. | `/saturate stop` or new `/saturate start` |

## 3. Messages & Payloads

### 3.1 TypeTags

Application range `0x00010200` – `0x000102FF`:

| Tag | Value | Direction | Purpose |
|-----|-------|-----------|---------|
| `SaturateStartTag` | `0x00010200` | Coordinator → Senders, Receivers, Collector | Begin a saturation run with preset parameters |
| `SaturateStopTag` | `0x00010201` | Coordinator → all | Halt the current run |
| `RateChangeTag` | `0x00010202` | Coordinator → Senders | Change target send rate and payload mode |
| `StatsPollTag` | `0x00010203` | Coordinator → Collector, Receivers | Request current stats snapshot |
| `StatsReplyTag` | `0x00010204` | Collector → Coordinator | Stats reply for CLI display |
| `ThroughputSampleTag` | `0x00010205` | Senders → Collector | Per-sender throughput sample |
| `DropReportTag` | `0x00010206` | Receivers → Collector | Per-receiver drop and receive counts |
| `LatencySampleTag` | `0x00010207` | Receivers → Collector | Per-message latency sample |
| `LoadMessageTag` | `0x00010208` | Senders → Receivers | The actual benchmark load messages |

### 3.2 Payload Formats

**SaturateStart payload** (40 bytes):
```
Offset  Size  Field
------  ----  -----
0       4     num_senders (uint32_t)
4       4     num_receivers (uint32_t)
8       1     payload_mode (uint8_t) — 0=small, 1=junk, 2=mixed
9       2     payload_size_min (uint16_t)
11      2     payload_size_max (uint16_t)
13      4     initial_rate_msgps (uint32_t)
17      2     step_interval_ms (uint16_t)
19      4     drop_threshold_pct (float)
23      1     refine_iterations (uint8_t)
24      4     mailbox_capacity (uint32_t)
28      4     stable_duration_ms (uint32_t)
32      4     duration_max_ms (uint32_t)
36      4     scheduler_threads (uint32_t) — for reference in report
```

**RateChange payload** (12 bytes):
```
Offset  Size  Field
------  ----  -----
0       4     target_rate_msgps (uint32_t) — target messages per second per sender
4       1     payload_mode (uint8_t) — 0=small, 1=junk, 2=mixed
5       2     payload_size_min (uint16_t) — minimum payload size for junk/mixed
7       2     payload_size_max (uint16_t) — maximum payload size for junk/mixed
9       2     step_interval_ms (uint16_t) — milliseconds between rate doublings
11      1     padding (uint8_t)
```

**LoadMessage payload** (variable):
```
Offset  Size  Field
------  ----  -----
0       4     sender_id (uint32_t)
4       8     seq_no (uint64_t) — monotonically increasing per sender
12      8     send_timestamp_us (uint64_t) — steady_clock timestamp at send
20      N     junk_data (uint8_t[]) — random fill to target size (small=0 bytes)
```

- Small mode: header-only (20 bytes total payload)
- Junk mode: header + random fill to size in [payload_size_min, payload_size_max]
- Mixed mode: 80% of messages are small (20B), 20% are junk (random size)

**LatencySample payload** (16 bytes):
```
Offset  Size  Field
------  ----  -----
0       4     sender_id (uint32_t)
4       8     seq_no (uint64_t)
12      4     latency_us (uint32_t) — computed as now - send_timestamp_us
```

**DropReport payload** (16 bytes):
```
Offset  Size  Field
------  ----  -----
0       4     receiver_id (uint32_t)
4       8     total_received (uint64_t)
12      4     total_dropped (uint32_t)
```

**StatsReply payload** (variable, for CLI display):
```
Key=value text format, same pattern as bench_perf collector's build_report().
```

All payloads use compact binary encoding with `std::memcpy` — no protobuf overhead
on the hot path. The existing `StreamBuffer` + helpers from bench_perf's
`messages.hpp` pattern are extended.

## 4. Presets

Pre-configured benchmark profiles for common stress scenarios:

| Preset | Senders | Receivers | Payload | Payload Size | Mailbox Cap | Description |
|--------|---------|-----------|---------|-------------|-------------|-------------|
| `quick-saturate` | 100 | 10 | small | n/a (header-only 20B) | 4096 | Fast ceiling find, ~30s |
| `deep-saturate` | 1000 | 100 | small | n/a (header-only 20B) | 8192 | Thorough saturation curve, ~60s |
| `alloc-stress` | 500 | 50 | junk | 1KB–64KB | 2048 | Slab allocator pressure, fragmentation |
| `mixed-load` | 500 | 50 | mixed (80/20) | small: 20B / junk: 1KB–64KB | 4096 | Realistic workload simulation |
| `fan-in-extreme` | 5000 | 1 | small | n/a (header-only 20B) | 16384 | Extreme many-to-one mailbox contention |
| `fan-out-burst` | 10 | 1000 | junk | 1KB–16KB | 1024 | Broad fan-out with memory pressure |

### 4.1 Configurable Parameters Per Preset

```cpp
struct SaturatePreset {
    std::string name;
    std::string description;
    uint32_t num_senders = 100;
    uint32_t num_receivers = 10;
    uint8_t  payload_mode = 0;          // 0=small, 1=junk, 2=mixed
    uint16_t payload_size_min = 16;     // bytes
    uint16_t payload_size_max = 16;     // bytes
    uint32_t initial_rate_msgps = 100;  // per-sender starting rate
    uint16_t step_interval_ms = 1000;   // ms between rate doublings
    float    drop_threshold_pct = 1.0f; // drop % that triggers ceiling
    uint8_t  refine_iterations = 5;     // binary search steps
    uint32_t mailbox_capacity = 4096;   // per-receiver mailbox capacity
    uint32_t stable_duration_ms = 5000; // observation period at ceiling
    uint32_t duration_max_ms = 120000;  // safety cap — auto-stop
};
```

## 5. System Probing

On startup (and on `/saturate probe`), the app detects and reports:

```
╔══════════════════════════════════════════════════════════╗
║               HPActor App 17 — Bench Saturate           ║
║          Actor System Saturation Benchmark              ║
╠══════════════════════════════════════════════════════════╣
║  Host:     macOS 26.1 (arm64)                           ║
║  CPU:      Apple M2 Pro                                 ║
║  Cores:    12 logical (8 performance, 4 efficiency)      ║
║  L1d/L1i:  128 KB / 192 KB per-core                     ║
║  L2:       4 MB shared                                  ║
║  Memory:   32 GB total, ~24 GB available                 ║
║  Page:     16 KB                                        ║
║                                                         ║
║  Scheduler:  8 worker threads (polling wake)             ║
║  Mailbox:    default 4096 capacity, MultiLaneQueue       ║
╚══════════════════════════════════════════════════════════╝
```

**Implementation:**

| Metric | macOS | Linux |
|--------|-------|-------|
| Logical cores | `sysconf(_SC_NPROCESSORS_ONLN)` | `sysconf(_SC_NPROCESSORS_ONLN)` |
| P/E core split | `sysctl(hw.perflevel0.logicalcpu)` | `/sys/devices/system/cpu/cpu*/cpu_capacity` or E-cores via `lscpu -p` |
| Total memory | `sysconf(_SC_PHYS_PAGES)` × `sysconf(_SC_PAGESIZE)` | `sysconf(_SC_PHYS_PAGES)` × `sysconf(_SC_PAGESIZE)` |
| Available memory | `host_statistics64` → `free_count` | `/proc/meminfo` → `MemAvailable` |
| Cache topology | `sysctl(hw.l1dcachesize)` etc. | `sysconf(_SC_LEVEL1_DCACHE_SIZE)` etc. |
| Scheduler model | `ActorSystem` config introspection | same |
| Wake strategy | `HybridScheduler` introspection | same |

Probing is best-effort — unavailable metrics are reported as `N/A`. All probing
code is wrapped in `#ifdef` platform guards.

## 6. CLI Commands

Following the existing `CommandRegistration<T>` pattern from `apps/bench_perf/commands/`:

```
/saturate start <preset>   — Start a saturation run
/saturate stop             — Stop the current run
/saturate status           — Show current phase, rate, drop%, elapsed
/saturate report [detail]  — Full report: saturation point, drop curve, latency, alloc stats
/saturate export [json|csv] — Export structured results
/saturate list             — List available presets
/saturate probe            — Show system hardware probe results
/saturate help             — Show all /saturate commands
```

### 6.1 Command Details

**`/saturate start <preset>`**
- Validates preset name against registered presets.
- Guards: rejects if a run is already in progress.
- Sends `SaturateStartTag` with preset parameters to coordinator.
- Enters `Probing` phase automatically.

**`/saturate status`**
- Displays: current phase, current target rate, actual send rate, drop rate, elapsed time, saturation ceiling (if discovered).
- Reads coordinator state via `InspectStateRequest`.

**`/saturate report [detail]`**
- Without `detail`: summary with saturation point, peak throughput, drop rate at ceiling, p50/p99 latency at ceiling.
- With `detail`: full drop-rate curve table, per-size-class allocator stats, per-phase timing breakdown.
- Reads collector state via `InspectStateRequest`.

**`/saturate export [json|csv]`**
- JSON: structured object with all metrics, time-series arrays.
- CSV: flat table of data points suitable for spreadsheet analysis.
- Default format: JSON.

**`/saturate list`**
- Prints preset table with name, description, sender/receiver counts, payload mode, mailbox capacity.

**`/saturate probe`**
- Re-runs system probing and displays current hardware snapshot.

**`/saturate help`**
- Lists all available `/saturate` commands with brief descriptions.

## 7. Headless Mode

When invoked as `./17_bench_saturate --headless <preset> [--format json|csv] [--output <path>]`:

1. No CLI actor is spawned — no interactive loop.
2. The coordinator is spawned and the preset is started automatically.
3. The app blocks until the coordinator reaches `Reporting` phase (or `duration_max_ms` safety cap).
4. Results are written to stdout (default) or to the specified file path.
5. Exit code 0 on success, non-zero on error (timeout, unknown preset, spawn failure).

This mode is designed for CI pipelines and automated regression detection.

## 8. Metrics & Reporting

### 8.1 Saturation Metrics

| Metric | Source | How Computed |
|--------|--------|--------------|
| Saturation ceiling (msg/s) | Coordinator | Last rate before drop rate > threshold, refined by binary search |
| Peak throughput (msg/s) | Collector | Maximum observed total receive rate |
| Drop rate curve | Collector | Time-series: {timestamp, send_rate, drop_count, drop_rate_pct} |
| Latency p50/p99/p999 (μs) | Collector | Streaming percentile from reservoir (10K samples) per payload size bin |
| Drop count / Drop rate | Receiver atoms → Collector | Atomic counters read on poll |
| Phase timing (ms) | Coordinator | Wall-clock duration of each phase |

### 8.2 Allocation Statistics

Captured via `MemoryRegionRegistry` and `SlabCache` introspection before and after each run:

| Metric | Source |
|--------|--------|
| Total allocations / deallocations | `MemoryRegionRegistry` delta |
| Slab cache hits / misses | `SlabCache::snapshot()` |
| System allocator fallback count | `MemoryRegionRegistry` fallback counter |
| Per-size-class hit/miss breakdown | `SlabCache::snapshot()` per-size-class |
| High-water memory mark | `MemoryRegionRegistry` region stats |
| Memory pressure events | `MemoryRegionRegistry` pressure state transitions |
| Rejected allocations | `MemoryRegionRegistry` reject counter |
| Active bytes (potential leaks) | allocations − deallocations delta |

### 8.3 Worker Thread Model

| Metric | Source |
|--------|--------|
| Scheduler thread count | `ActorSystem` config |
| Wake strategy (polling/CV) | `HybridScheduler` introspection |
| Thread affinity | Platform-specific (`sched_getaffinity` / `thread_policy_get`) |
| Work-stealing algorithm | `HybridScheduler` introspection (A2WS) |
| Steal attempts / successful steals | `HybridScheduler` metrics (if available) |

### 8.4 Message Size Distribution

Histogram buckets: `16B`, `32B`, `64B`, `128B`, `256B`, `512B`, `1KB`, `2KB`, `4KB`, `8KB`, `16KB`, `32KB`, `64KB`.

Reported as message count and percentage per bucket, based on actual payload sizes sent during the run.

## 9. File Layout

```
apps/bench_saturate/
├── CMakeLists.txt
├── 17_bench_saturate.cpp          # main, system probing, splash, headless mode
├── messages.hpp                   # TypeTags (0x00010200–0x000102FF), payload encode/decode, CPU helpers
├── actors/
│   ├── saturate_coordinator_actor.hpp   # ramp state machine, preset management, rate broadcast
│   ├── saturate_sender_actor.hpp        # rate-directed self-scheduling, round-robin dispatch
│   ├── saturate_receiver_actor.hpp      # bounded mailbox, drop counter, latency extraction
│   └── saturate_collector_actor.hpp     # streaming percentiles, drop-rate curve, alloc deltas
└── commands/
    └── saturate_commands.cpp            # CLI commands via CommandRegistration<T>
```

Registered in `apps/CMakeLists.txt` as `add_subdirectory(bench_saturate)`, gated by `ENABLE_APPS`.

`CMakeLists.txt`:
```cmake
add_executable(17_bench_saturate
    17_bench_saturate.cpp
    commands/saturate_commands.cpp
)
target_link_libraries(17_bench_saturate PRIVATE hpactor_lib)
target_include_directories(17_bench_saturate PRIVATE ${CMAKE_SOURCE_DIR})
```

## 10. Error Handling

| Scenario | Handling |
|----------|----------|
| Senders/receivers not yet spawned when start arrives | Coordinator returns error message via `last_error_` (same pattern as bench_perf) |
| Run already in progress | `/saturate start` CLI returns "Run in progress, use /saturate stop first" |
| Mailbox overflow on receivers | Expected behavior — counted as drops via atomic counter, triggers ramp refinement |
| Collector ring buffer full | Drop oldest samples, log warning via structured logging, continue |
| Duration safety cap (`duration_max_ms`) exceeded | Auto-stop the run, report partial results, log warning |
| Headless mode timeout | Exit with error code 2 + partial results to stdout |
| Unknown preset name | CLI returns error listing available presets; headless mode exits code 1 |
| Memory allocation failure | Propagate via existing `MemoryRegion` pressure/reject path; report in stats |
| Platform probing unavailable | Report `N/A` for unavailable metrics; do not fail |

## 11. Testing Strategy

### 11.1 Unit Tests

| Test file | Scope |
|-----------|-------|
| `test_saturate_messages` | Payload encode/decode roundtrip for all message types, edge cases (empty buffer, truncated, size bounds) |
| `test_saturate_collector_math` | Percentile computation, drop-rate math, throughput calculation, reservoir bounds |
| `test_saturate_coordinator` | Ramp state machine transitions, rate doubling math, binary search refinement, preset lookup, error guards |
| `test_saturate_sender` | Rate scheduler math, round-robin dispatch distribution, seq_no monotonicity, payload size distribution in mixed mode |
| `test_saturate_receiver` | Drop counting, latency extraction from timestamps, mailbox overflow policy verification |
| `test_saturate_probe` | Platform detection output format, parsing correctness, N/A handling for unavailable metrics |

### 11.2 Integration Tests

| Test file | Scope |
|-----------|-------|
| `test_saturate_message_flow` | Coordinator→Sender→Receiver→Collector message propagation, RateChange delivery, start/stop lifecycle |
| `test_saturate_drop_counting` | Bounded mailbox overflow produces correct drop counts, multiple receivers, concurrent sends |
| `test_saturate_ramp_logic` | Coordinator advances through phases correctly, rate doubling observed by senders |
| `test_saturate_headless` | Headless mode starts, runs, produces valid JSON/CSV output, exits with correct code |
| `test_saturate_commands` | Each CLI command produces expected output format, error handling for invalid args |

### 11.3 System Tests

| Test file | Scope |
|-----------|-------|
| `test_saturate_full_run` | Full saturation run with 100 senders → 10 receivers, verify saturation ceiling discovered |
| `test_saturate_alloc_pressure` | Junk payload mode, verify allocator stats captured |
| `test_saturate_mixed_mode` | Mixed 80/20 mode, verify message size distribution histogram |

## 12. References

- Existing bench_perf app: `apps/bench_perf/` (app 16) — pattern reference
- Existing design spec: `docs/superpowers/specs/2026-06-13-perf-benchmark-app-design.md`
- CLI architecture: `docs/architecture/cli/cli-architecture-detailed-design.md`
- Memory management: `docs/architecture/memory/memory-management-architecture-design.md`
- Actor concurrency rules: `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- GitHub issue: [#313](https://github.com/skg7on/HPActor/issues/313)
