# Savina-Inspired Stress Regression Benchmark — Design Spec

**Date:** 2026-06-26
**Status:** Draft for user review
**App:** `apps/bench_savina/`
**Purpose:** Stress/regression testing for HPActor internals
**Source references:** Savina paper, "Savina - An Actor Benchmark Suite:
Enabling Empirical Evaluation of Actor Libraries" (Imam and Sarkar, AGERE!
2014), and the public `shamsimam/savina` benchmark suite.

## 1. Overview

This design adds a Savina-inspired benchmark app for HPActor. The goal is not a
cross-language leaderboard against Akka, Scala Actors, or other JVM actor
libraries. The goal is a repeatable internal torture suite that catches HPActor
regressions in scheduler fairness, mailbox correctness, actor lifecycle,
delivery accounting, backpressure, and hot-path throughput.

Savina is useful here because its workloads cover several actor communication
shapes: ping-pong request/reply, hot counter fan-in, token rings, recursive
actor fan-out/fan-in, and producer/consumer coordination. HPActor should port
those behavior contracts into native C++ actors, keep deterministic correctness
oracles, and report enough runtime evidence for CI and nightly stress jobs.

## 2. Existing Project Fit

HPActor already has the pieces this app should reuse:

| Existing component | Reuse |
|--------------------|-------|
| `apps/bench_perf/` | Coordinator/collector pattern and benchmark reporting style |
| `apps/bench_saturate/` | Headless `--headless`, JSON/CSV output, scripted run shape |
| `EventBasedActor` | Default actor implementation for benchmark workloads |
| `ActorSystem::spawn()` and `ActorContext::send()` | Native actor topology and message flow |
| Bounded mailbox, multi-lane queue, DLQ, failure envelopes | Regression surfaces for overload and accounting |
| Metrics/logging/tracing config | Optional instrumentation dimensions |
| `tests/system/apps/` | Smoke regression tests for app-level correctness |

The new app should be separate from `bench_saturate`. Saturation discovery is a
specific load-test algorithm; Savina-style workloads are a broader set of actor
interaction patterns and correctness oracles.

## 3. Goals

1. Add `apps/bench_savina/` as a standalone benchmark executable.
2. Provide headless, scriptable execution for CI and nightly stress lanes.
3. Port a first workload slice: `count`, `pingpong`, `threadring`, `fib`, and
   `bounded-buffer`.
4. Make every workload fail fast on invariant violations, not merely report
   timing.
5. Emit JSON and CSV summaries with timing, counters, error state, and selected
   HPActor runtime configuration.
6. Exercise HPActor internals with deterministic presets: mailbox hot spots,
   scheduler handoff, actor spawn pressure, bounded admission, overflow/DLQ,
   and shutdown/lifecycle cleanup.
7. Keep smoke presets short enough for app-level CI and larger presets suitable
   for manual or nightly stress runs.

## 4. Non-Goals

- Reproducing Savina's JVM runner or Maven project.
- Claiming fair absolute performance comparisons against upstream Savina
  results.
- Building a dashboard or web UI.
- Multi-node or cross-process distributed benchmarking.
- Replacing existing unit, integration, system, sanitizer, or Relacy tests.
- Adding new runtime behavior to make a benchmark pass. Benchmark failures
  should reveal runtime defects or workload bugs.

## 5. Command Surface

The executable should support one benchmark, a named suite, or all smoke
workloads:

```bash
bench_savina --benchmark pingpong --preset smoke --iterations 12 --format json
bench_savina --suite smoke --format json
bench_savina --suite nightly --output savina-nightly.json
```

Initial flags:

| Flag | Meaning |
|------|---------|
| `--benchmark <name>` | Run one workload: `count`, `pingpong`, `threadring`, `fib`, `bounded-buffer` |
| `--suite <name>` | Run a named collection: `smoke`, `scheduler`, `mailbox`, `lifecycle`, `nightly` |
| `--preset <name>` | Workload size profile, default `smoke` |
| `--iterations <N>` | Timed iterations, default 12 for parity with Savina |
| `--warmup <N>` | Untimed warmup iterations, default 1 for smoke and 2 for stress |
| `--format <json|csv|text>` | Output format, default `json` |
| `--output <path>` | Optional output file |
| `--scheduler-threads <N>` | Override `Config::scheduler_threads` |
| `--mailbox-capacity <N>` | Override default mailbox capacity |
| `--delivery <normal|fast-local>` | Select normal `context()->send()` or explicit local fast path where a workload supports it |
| `--timeout-ms <N>` | Per-iteration hard timeout |

The app exits nonzero when any workload invariant fails, any iteration times
out, or actor-system shutdown fails.

## 6. Architecture

### 6.1 Modules

```
apps/bench_savina/
├── 19_bench_savina.cpp
├── CMakeLists.txt
├── runner/
│   ├── savina_runner.hpp
│   ├── savina_result.hpp
│   ├── savina_stats.hpp
│   └── savina_cli.hpp
├── workloads/
│   ├── count.hpp
│   ├── pingpong.hpp
│   ├── threadring.hpp
│   ├── fib.hpp
│   └── bounded_buffer.hpp
└── actors/
    ├── savina_messages.hpp
    └── savina_collector_actor.hpp
```

All app-specific code stays in the app directory. Shared HPActor runtime headers
are used as consumers would use them; no benchmark-specific hooks should be
added to core runtime code in the first implementation.

### 6.2 Core Interfaces

`SavinaWorkload` is a small C++ interface, not a new actor base class:

```cpp
struct SavinaWorkloadConfig {
    std::string benchmark;
    std::string preset;
    uint32_t scheduler_threads = 0;
    uint32_t mailbox_capacity = 0;
    uint32_t timeout_ms = 0;
    bool use_fast_local_delivery = false;
};

struct SavinaIterationResult {
    bool ok = false;
    std::string error;
    uint64_t elapsed_us = 0;
    uint64_t messages_sent = 0;
    uint64_t messages_received = 0;
    uint64_t actors_spawned = 0;
    uint64_t dead_letters = 0;
    uint64_t dropped = 0;
};

class SavinaWorkload {
public:
    virtual ~SavinaWorkload() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual SavinaIterationResult run_iteration(
        const SavinaWorkloadConfig& config) = 0;
};
```

Each `run_iteration()` creates a fresh `ActorSystem`, builds the workload actor
topology, starts a coordinator/latch, waits for completion or timeout, collects
results, and shuts the system down. Fresh systems make lifecycle leaks, shutdown
regressions, and configuration-sensitive failures visible.

### 6.3 Runner Responsibilities

`SavinaRunner` owns:

1. Argument parsing and suite expansion.
2. Warmup iteration execution.
3. Timed iteration execution.
4. Median-centered outlier filtering matching Savina's spirit.
5. Summary statistics: best, worst, median, arithmetic mean, standard
   deviation, confidence low/high, coefficient of variation.
6. Aggregation of invariant failures and timeout failures.
7. JSON/CSV/text output.

The runner must never hide correctness failures as statistical outliers. If an
iteration reports `ok=false`, the entire run fails.

## 7. Workloads

### 7.1 `count`

Topology:

```
ProducerActor -> CounterActor -> ProducerActor
```

Behavior:

1. Producer sends `N` increment messages to a single counter actor.
2. Producer sends a retrieve message.
3. Counter replies with the final count.

Regression target:

- Hot mailbox enqueue/dequeue behavior.
- MPSC contention when future presets use multiple producers.
- Message ordering to one receiver.

Invariant:

- Final count equals total increments sent.
- Received plus dropped plus dead-lettered equals sent when bounded overload
  presets intentionally enable overflow.

### 7.2 `pingpong`

Topology:

```
PingActor <-> PongActor
```

Behavior:

1. Ping sends `N` ping messages.
2. Pong replies to each ping.
3. Ping stops after receiving the `N`th pong.

Regression target:

- Round-trip scheduler handoff.
- Reply/sender metadata path.
- Latency distribution stability.

Invariant:

- Ping receives exactly `N` pong replies.
- No unexpected dead letters.

### 7.3 `threadring`

Topology:

```
A0 -> A1 -> A2 -> ... -> A(N-1) -> A0
```

Behavior:

1. Build a ring of `N` actors.
2. Send one token with `R` remaining hops.
3. Each actor forwards the token to the next actor and decrements hop count.
4. The actor that observes zero reports completion to the coordinator.

Regression target:

- Scheduler fairness across many actors.
- Actor address routing.
- Ready-gate behavior and mailbox wakeups.

Invariant:

- Completed hop count equals `R`.
- Completion ring index matches the deterministic workload oracle for the
  selected forwarding convention.

### 7.4 `fib`

Topology:

```
FibActor(n)
├── FibActor(n-1)
└── FibActor(n-2)
```

Behavior:

1. A root actor receives `fib(N)`.
2. Non-base actors spawn two child actors and wait for both responses.
3. The root reports the final result.

Regression target:

- Actor creation/destruction pressure.
- Recursive fan-out/fan-in.
- Lifecycle cleanup and shutdown.

Invariant:

- Result equals a local iterative Fibonacci oracle.
- Actors spawned equals the expected recursive tree size for the selected `N`.

### 7.5 `bounded-buffer`

Topology:

```
ProducerActor(s) -> BufferActor -> ConsumerActor(s)
```

Behavior:

1. Producers submit `N` items to a bounded buffer actor.
2. Consumers request or receive items until the configured count is resolved.
3. Overflow-enabled presets use small mailbox capacity and explicit overflow
   accounting.

Regression target:

- Mailbox capacity and backpressure.
- Overflow policy accounting.
- DLQ handoff when configured.

Invariant:

- Every produced item is accounted for as consumed, rejected, dropped, or
  dead-lettered.
- No duplicated item ids are consumed.

## 8. Presets and Suites

### 8.1 Presets

| Preset | Intended lane | Size | Timeout |
|--------|---------------|------|---------|
| `smoke` | CI app smoke | Small, completes in seconds | 5s per iteration |
| `stress` | Developer/manual | Larger counts, more actors | 30s per iteration |
| `nightly` | Nightly regression | Long duration, multiple configs | 120s per iteration |
| `overflow` | Mailbox/DLQ stress | Small capacity, high pressure | 30s per iteration |
| `lifecycle` | Spawn/shutdown stress | Higher `fib` spawn pressure | 30s per iteration |

### 8.2 Suites

| Suite | Workloads | Purpose |
|-------|-----------|---------|
| `smoke` | all first-slice workloads with `smoke` preset | Fast correctness gate |
| `scheduler` | `pingpong`, `threadring` | Handoff/fairness regression |
| `mailbox` | `count`, `bounded-buffer` | Hot mailbox and bounded admission |
| `lifecycle` | `fib`, `threadring` | Spawn, actor cleanup, shutdown |
| `nightly` | all workloads across `stress`, `overflow`, and `lifecycle` presets | Broader regression evidence |

## 9. TypeTag Allocation

Use app-local tags in `0x00010300` through `0x000103FF` to avoid the existing
`bench_saturate` range.

| Tag | Value | Purpose |
|-----|-------|---------|
| `SavinaStartTag` | `0x00010300` | Coordinator starts a workload |
| `SavinaDoneTag` | `0x00010301` | Actor reports completion |
| `SavinaErrorTag` | `0x00010302` | Actor reports invariant failure |
| `SavinaPingTag` | `0x00010310` | Ping-pong request |
| `SavinaPongTag` | `0x00010311` | Ping-pong reply |
| `SavinaCountIncTag` | `0x00010320` | Counter increment |
| `SavinaCountGetTag` | `0x00010321` | Counter retrieve |
| `SavinaTokenTag` | `0x00010330` | Thread-ring token |
| `SavinaFibReqTag` | `0x00010340` | Fibonacci request |
| `SavinaFibRespTag` | `0x00010341` | Fibonacci response |
| `SavinaBufferPutTag` | `0x00010350` | Bounded-buffer put |
| `SavinaBufferGetTag` | `0x00010351` | Bounded-buffer get |

Payloads should be compact binary `StreamBuffer` values built with `std::memcpy`
helpers. No protobuf schema is needed for this app unless a future benchmark
needs wire compatibility.

## 10. Result Output

Single-benchmark JSON output shape:

```json
{
  "suite": "smoke",
  "benchmark": "pingpong",
  "preset": "smoke",
  "iterations": 12,
  "warmup": 1,
  "ok": true,
  "runtime": {
    "scheduler_threads": 8,
    "mailbox_capacity": 16384,
    "delivery": "normal"
  },
  "summary": {
    "best_us": 1200,
    "worst_us": 1700,
    "median_us": 1400,
    "mean_us": 1425,
    "stddev_us": 90,
    "confidence_low_us": 1370,
    "confidence_high_us": 1480,
    "coefficient_of_variation": 0.063
  },
  "counters": {
    "messages_sent": 1000,
    "messages_received": 1000,
    "actors_spawned": 2,
    "dropped": 0,
    "dead_letters": 0
  },
  "errors": []
}
```

Suite JSON wraps the same per-benchmark objects in a top-level `runs` array and
sets the top-level `ok` field to false when any run fails.

CSV output should include one row per benchmark run with the same summary and
counter fields. Text output is for humans and should not be used by CI parsers.

## 11. Error Handling and Timeouts

Each iteration has a hard deadline. On timeout:

1. The runner records `ok=false` with `error="timeout"`.
2. The actor system is shut down with the configured drain policy.
3. The app exits nonzero after emitting the partial result.

Workload actors report invariant failures to a coordinator/latch actor instead
of printing and continuing. Unexpected DLQ records or delivery failures count as
errors unless the preset explicitly enables overflow accounting.

## 12. Testing Strategy

### 12.1 Unit Tests

Add focused unit tests for runner-only logic:

- Argument parsing.
- Suite expansion.
- Statistics and outlier filtering.
- JSON/CSV serialization.
- Fibonacci oracle and expected actor-count calculation.

### 12.2 System Tests

Add app smoke tests under `tests/system/apps/`:

```bash
bench_savina --suite smoke --iterations 2 --warmup 0 --format json
bench_savina --benchmark bounded-buffer --preset overflow --iterations 2 --format json
```

Assertions:

- Exit code is zero for valid smoke runs.
- JSON parses and has `ok=true`.
- Required counters are present.
- Invalid benchmark or preset exits nonzero.

### 12.3 Manual and Nightly Verification

Nightly runs should execute:

```bash
bench_savina --suite nightly --iterations 12 --format json --output savina-nightly.json
```

Optional stress matrices:

- `--scheduler-threads` values: 1, 2, host default, host default x2.
- `--mailbox-capacity` values: 64, 1024, 16384.
- `--delivery` values: `normal`, `fast-local` where supported.
- Observability enabled/disabled in the build/runtime config.

Sanitizer lanes should prefer smoke or targeted stress presets to keep runtime
bounded.

## 13. Implementation Phasing

### Phase 1: Runner and Smoke Workloads

- Add app skeleton and CMake integration.
- Implement runner, stats, JSON/CSV output.
- Implement `count`, `pingpong`, and `threadring`.
- Add system smoke tests.

### Phase 2: Lifecycle and Bounded Admission

- Implement `fib`.
- Implement `bounded-buffer`.
- Add overflow preset and accounting checks.
- Add runner unit tests.

### Phase 3: Nightly Stress Expansion

- Add stress/nightly suites.
- Add optional fast-local delivery mode for supported workloads.
- Capture DLQ/dead-letter counters and selected metrics snapshots.
- Document CI/nightly invocation.

### Phase 4: Larger Savina Workloads

Future workloads can include quicksort, radix sort, recursive matrix multiply,
SOR, and A* once the first slice proves stable.

## 14. Acceptance Criteria

1. `bench_savina --suite smoke --iterations 2 --warmup 0 --format json` exits
   zero and reports all first-slice workloads with `ok=true`.
2. Invalid benchmark and preset names exit nonzero with a clear error.
3. Each first-slice workload has a deterministic correctness oracle.
4. Timeout paths emit partial JSON and exit nonzero.
5. At least one system test covers the smoke suite.
6. At least one system test covers intentional bounded-buffer overflow
   accounting.
7. The app is built only when `ENABLE_APPS=ON`, matching existing app behavior.
8. No HPActor core runtime API changes are required for the first implementation.
