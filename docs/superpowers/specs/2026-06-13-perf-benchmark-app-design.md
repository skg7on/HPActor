# HPActor Performance Benchmark App — Design Spec

**Date:** 2026-06-13
**Status:** Design approved
**App:** `apps/bench_perf/` — Actor System Performance Benchmark (App 16)

## Overview

A standalone performance benchmarking app for the HPActor actor system, following
the `apps/cli_demo/` patterns (EventBasedActor, Behavior, CLI integration, thread-safe
RNG). The app exercises two core scenarios:

1. **Many-actors throughput** — 5000 cooperative actors scheduled by 8 worker threads
   under sustained high message pressure (100 Hz per actor).
2. **Hot-actor fairness** — 1 dedicated actor with heavy CPU work (500μs burn) +
   high message rate (1000 Hz) alongside 1000 cold actors with light work (10μs burn,
   10 Hz). Measures whether the hot actor starves cold-actor tail latency.

All actor counts, burn durations, message rates, and run durations are parameterized
via named presets selected at `/bench start <name>`.

## Architecture

### Actor Topology

```
BenchCoordinatorActor ──[BenchStart]──► N × BenchWorkerActor ──[LatencySample]──► BenchCollectorActor
                      ──[BenchStart]──► M × BenchHotActor    ──[LatencySample]──► BenchCollectorActor
                      ◄──[StatsPoll/Reply]──► BenchCollectorActor
```

| Actor | Class | Instances | Purpose |
|---|---|---|---|
| BenchWorkerActor | `EventBasedActor` | 1000–10000 | Light CPU burn per msg, self-schedules next tick, sends latency+throughput samples to collector |
| BenchHotActor | `EventBasedActor` | 1–N | Heavy CPU burn + high message rate, same sample interface as worker |
| BenchCollectorActor | `EventBasedActor` | 1 | Receives samples, computes streaming percentiles (reservoir sample: 10K values, sort-on-snapshot), tracks throughput windows per group |
| BenchCoordinatorActor | `EventBasedActor` | 1 | Orchestrates start/stop, polls collector, registers `/bench` CLI command tree |

### Message Flow

1. User types `/bench start many-actors` in CLI.
2. Coordinator sends `BenchStartTag` messages to all workers, hot actors, and collector (with burn duration, rate, and duration parameters).
3. Workers/hot actors self-schedule via `context()->schedule()`, do CPU burn inline, then send `LatencySampleTag` and `ThroughputTickTag` to collector.
4. On timer expiry or `/bench stop`, coordinator sends `BenchStopTag`.
5. User runs `/bench report` — coordinator polls collector via `StatsPollTag`/`StatsReplyTag` and formats output.
6. User runs `/bench export --json` for raw data dump.

### CPU Burn Model

Portable busy-wait using `std::chrono::steady_clock` polling in a tight loop — no
`rdtsc` portability issues. Burn duration passed via `BenchStartTag` payload.
The burn is measured per-iteration so the scheduler can preempt on policy.

## CLI Command Tree

```
/bench start <preset>   — Start a benchmark run (presets: "many-actors", "hot-actor", or custom)
/bench stop             — Stop the current run, finalize results
/bench status           — Show current run state, elapsed wall clock, aggregate throughput
/bench report [group]   — Full latency/throughput report; [group] filters to hot/cold/collector
/bench export [--json]  — Export raw data as JSON to stdout
/bench list             — List available benchmark preset names
/bench help             — Show available /bench commands
```

## Data Model

### Messages (`messages.hpp`)

```cpp
LatencySampleTag     // payload: {actor_id: uint64, latency_us: uint64, group: uint8}
ThroughputTickTag    // payload: {actor_id: uint64, msg_count: uint64, window_ms: uint32}
BenchStartTag        // payload: {burn_us: uint32, rate_hz: uint32, duration_ms: uint32}
BenchStopTag         // payload: empty
StatsPollTag         // coordinator → collector, requests snapshot
StatsReplyTag        // collector → coordinator, aggregated results
```

TypeTag range: `0x00010100` – `0x000101FF` (application range for benchmark app).

### Metrics Tracked by Collector

| Metric | Method | Scope |
|---|---|---|
| `throughput_msgps` | Sliding 1s window counter | Per group + total |
| `latency_p50_us` | Reservoir sample (10K values, sort on snapshot) | Per group |
| `latency_p99_us` | Reservoir sample (10K values, sort on snapshot) | Per group |
| `latency_p999_us` | Reservoir sample (10K values, sort on snapshot) | Per group |
| `messages_total` | Monotonic counter | Per group + total |
| `scheduler_utilization` | Active cycles / wall clock | Global |
| `mailbox_peak_depth` | Max depth observed during run | Per group |
| `elapsed_ms` | Wall clock since BenchStart | Global |

## File Layout

```
apps/bench_perf/
├── CMakeLists.txt               # add_executable(16_bench_perf ...)
├── 16_bench_perf.cpp            # main() — system setup, spawn, CLI loop
├── messages.hpp                 # TypeTags, payload helpers
└── actors/
    ├── bench_worker_actor.hpp   # BenchWorkerActor — light CPU burn
    ├── bench_hot_actor.hpp      # BenchHotActor — heavy CPU burn + high rate
    ├── bench_collector_actor.hpp # BenchCollectorActor — streaming stats
    └── bench_coordinator_actor.hpp # BenchCoordinatorActor — CLI /bench commands
```

**Registration** in `apps/CMakeLists.txt`: `add_subdirectory(bench_perf)`.

## Benchmark Presets

### `many-actors` — Throughput under high fan-out

| Parameter | Value |
|---|---|
| Actor count | 5000 BenchWorkerActors |
| Burn per message | 10 μs |
| Message rate per actor | 100 Hz |
| Scheduler threads | 8 |
| Mailbox capacity | Unbounded (use default) |
| Duration | 30 s |
| Expected throughput | ~500K msgs/sec total |
| Expected p99 latency | < 5 ms |

### `hot-actor` — Noisy neighbor fairness

| Parameter | Value |
|---|---|
| Hot actor count | 1 BenchHotActor |
| Hot burn per message | 500 μs |
| Hot message rate | 1000 Hz |
| Cold actor count | 1000 BenchWorkerActors |
| Cold burn per message | 10 μs |
| Cold message rate | 10 Hz |
| Scheduler threads | 8 |
| Duration | 30 s |
| Expected cold p99 latency | < 10 ms |

Additional presets can be added by extending the preset table in `16_bench_perf.cpp`.

## Testing

- **Smoke test** in `tests/system/apps/`: spawn 10 actors, 2-second run, verify
  `/bench report` produces output with throughput > 0 and latencies in expected range.
- No unit tests for individual actors — they are thin wrappers around the framework.
- Preset configurations are validated at `/bench start` time (reject unknown presets).

## Implementation Constraints

- C++20, no exceptions, no RTTI (matches project standards).
- Uses existing HPActor public API only (`EventBasedActor`, `Behavior`, `context()->schedule()`, `context()->send()`, `system.deliver_local()`).
- Thread-safe RNG: same `thread_local` Xorshift pattern from `apps/cli_demo/actors/worker_actor.hpp`.
- CLI commands self-register via coordinator's `on_spawn()` into the CliActor command tree.
- CPU burn is purely cooperative — no blocking, no sleeps, no syscalls in the hot path.

## Implementation Sequence

1. **Create worktree** — isolate work per CLAUDE.md requirement.
2. **Scaffold `apps/bench_perf/`** — CMakeLists.txt, messages.hpp, main skeleton.
3. **Implement `BenchCollectorActor`** — streaming stats, T-digest or reservoir.
4. **Implement `BenchWorkerActor`** — light burn, self-scheduling, sample emission.
5. **Implement `BenchHotActor`** — heavy burn variant.
6. **Implement `BenchCoordinatorActor`** — CLI command registration, run orchestration.
7. **Wire `16_bench_perf.cpp` main()** — system setup, spawn, CLI loop.
8. **Write smoke test** — 10 actors, verify output.
9. **Build, test, verify** — per CLAUDE.md build verification discipline.
