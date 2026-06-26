# HPActor CAF Performance Benchmark Port - Design Spec

**Date:** 2026-06-26
**Status:** Phase 3 implemented
**Issue:** [#371](https://github.com/skg7on/HPActor/issues/371)
**Proposed app:** `apps/bench_caf/` - CAF-style actor benchmark suite for HPActor

## 1. Overview

This spec defines a CAF-style benchmark suite for HPActor. The goal is to port
the benchmark method and scenario contracts from CAF, not the CAF
implementation code. HPActor should use native actors, `TypedMessage` payloads,
mailboxes, scheduler configuration, metrics, tracing, memory counters, and
delivery paths so that benchmark results expose HPActor's own runtime behavior.

The suite should be useful for two audiences:

1. Developers who want repeatable regression coverage for HPActor internals.
2. Operators and maintainers who want comparable performance evidence across
   commits, build modes, machines, and eventually distributed deployments.

CAF's original evaluation emphasized actor creation, mailbox contention, mixed
actor lifecycle/message/CPU workloads, Mandelbrot CPU scheduling, distributed
ping/pong, and microbenchmarks for message creation, pattern matching,
serialization, and streaming. HPActor should preserve these benchmark contracts
while extending the matrix with explicit message size, message shape, traffic
distribution, and internal counters.

## 2. References

- CAF paper: [Revisiting Actor Programming in C++](https://arxiv.org/abs/1505.07368)
- CAF benchmark repository: [actor-framework/benchmarks](https://github.com/actor-framework/benchmarks)
- CAF source scenarios:
  - [`actor_creation.cpp`](https://github.com/actor-framework/benchmarks/blob/main/src/caf/actor_creation.cpp)
  - [`mailbox_performance.cpp`](https://github.com/actor-framework/benchmarks/blob/main/src/caf/mailbox_performance.cpp)
  - [`mixed_case.cpp`](https://github.com/actor-framework/benchmarks/blob/main/src/caf/mixed_case.cpp)
  - [`mandelbrot.cpp`](https://github.com/actor-framework/benchmarks/blob/main/src/caf/mandelbrot.cpp)
  - [`distributed.cpp`](https://github.com/actor-framework/benchmarks/blob/main/src/caf/distributed.cpp)
  - [`scheduling.cpp`](https://github.com/actor-framework/benchmarks/blob/main/src/caf/scheduling.cpp)
  - [`message-creation.cpp`](https://github.com/actor-framework/benchmarks/blob/main/src/caf/microbenchmarks/message-creation.cpp)
  - [`pattern-matching.cpp`](https://github.com/actor-framework/benchmarks/blob/main/src/caf/microbenchmarks/pattern-matching.cpp)
  - [`serialization.cpp`](https://github.com/actor-framework/benchmarks/blob/main/src/caf/microbenchmarks/serialization.cpp)
  - [`streaming.cpp`](https://github.com/actor-framework/benchmarks/blob/main/src/caf/microbenchmarks/streaming.cpp)
- HPActor benchmark references:
  - `apps/bench_perf/`
  - `apps/bench_saturate/`
  - `docs/superpowers/specs/2026-06-13-perf-benchmark-app-design.md`
  - `docs/superpowers/specs/2026-06-17-bench-saturate-design.md`
- HPActor runtime rules:
  - `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
  - `docs/architecture/production/production-reliability-plane.md`
  - `docs/architecture/production/feature-gap-refined-requirement-backlog.md`

## 3. Goals

1. Add a CAF-style benchmark surface for HPActor with stable scenario contracts.
2. Make local actor creation, mailbox contention, scheduler fairness,
   allocation pressure, dispatch cost, serialization cost, and remote messaging
   regressions visible.
3. Support small smoke runs for CI and larger nightly, paper-scale, and stress
   runs for performance tracking.
4. Emit stable JSON and CSV output with enough raw metadata to compare runs
   across commits and machines.
5. Extend CAF's method with HPActor-specific dimensions:
   - message size
   - message shape
   - traffic distribution
   - bounded mailbox behavior
   - delivery failures, drops, DLQ handoff, and backpressure counters
   - scheduler and memory-region counters
6. Keep benchmark behavior deterministic enough for regression detection while
   still allowing high-load stress presets.

## 4. Non-Goals

- Do not copy CAF implementation code.
- Do not use CAF as a dependency.
- Do not replace `apps/bench_perf/` or `apps/bench_saturate/`.
- Do not require distributed tests in normal CI.
- Do not make benchmark success depend on exact throughput values from one
  developer machine.
- Do not bypass HPActor delivery semantics by default. Fast-path variants may
  exist, but each must explicitly state which checks are skipped.

## 5. Design Principles

### 5.1 Port Contracts, Not Code

Each CAF scenario becomes an HPActor scenario with equivalent topology,
parameters, and measurement semantics. The actor classes, payloads, scheduler
configuration, and output format are HPActor-native.

### 5.2 Measure Regression Signals, Not Only Peak Numbers

The benchmark should report wall-clock time and throughput, but the main value
is detecting changes in:

- mailbox admission behavior
- scheduler wakeup and fairness behavior
- allocator and memory-region pressure
- actor spawn and teardown cost
- message dispatch cost
- serialization and payload-copy cost
- remote transport setup and ping/pong cost

### 5.3 Keep CI Runs Small And Deterministic

Smoke presets should run quickly with conservative message counts. They verify
the scenario topology, output schema, and gross performance regressions. Larger
presets belong in nightly or manually triggered jobs.

### 5.4 Preserve Actor Runtime Contracts

Benchmark actors must not violate HPActor's core runtime rules:

- One serialized consumer per actor mailbox.
- Actor state remains private to actor turns.
- Scheduler readiness goes through the ready-gate contract.
- User-facing sends enter through normal delivery paths unless a scenario is
  explicitly marked as a fast-path microbenchmark.
- Bounded capacity failures are counted and reported.

## 6. Proposed App Surface

Add a new application:

```text
apps/bench_caf/
```

Candidate binary:

```text
18_bench_caf
```

Candidate command line:

```bash
./build/apps/bench_caf/18_bench_caf \
  --scenario mailbox-n1 \
  --preset smoke \
  --scheduler-threads 8 \
  --message-size 1024 \
  --message-shape fixed-bytes \
  --distribution n-to-one \
  --trials 3 \
  --format json \
  --output caf-mailbox-smoke.json
```

Required flags:

| Flag | Purpose |
|------|---------|
| `--scenario <name>` | Selects one benchmark scenario. |
| `--preset <name>` | Selects `smoke`, `nightly`, `paper-scale`, or `stress`. |
| `--format json|csv` | Selects output format. Default: `json`. |
| `--output <path>` | Writes output to a file. Default: stdout. |

Common optional flags:

| Flag | Purpose |
|------|---------|
| `--scheduler-threads <N>` | Overrides HPActor scheduler thread count. |
| `--trials <N>` | Runs the scenario N independent times. |
| `--warmup <N>` | Runs N warmup iterations before measurement. |
| `--message-size <bytes>` | Overrides fixed payload size for supported scenarios. |
| `--message-shape <shape>` | Selects payload encoding shape. |
| `--distribution <shape>` | Selects traffic distribution. |
| `--mailbox-capacity <N>` | Overrides receiver mailbox capacity where applicable. |
| `--seed <N>` | Controls deterministic distribution and payload generation. |
| `--sample-rss-ms <N>` | Sets RSS sampler interval. Default: 50 ms. |
| `--delivery-path normal|fast-local` | Selects normal delivery or an explicit fast-path microbenchmark mode. |

## 7. Presets

Presets provide safe default dimensions for different execution lanes.

| Preset | Target lane | Trial count | Runtime goal | Purpose |
|--------|-------------|-------------|--------------|---------|
| `smoke` | CI | 1-3 | seconds | Validate topology, output schema, and basic regression signals. |
| `nightly` | Scheduled perf job | 3-5 | minutes | Track trends across commits with moderate load. |
| `paper-scale` | Manual comparison | 10 | minutes to longer | Match CAF-style historical dimensions where practical. |
| `stress` | Manual reliability/perf run | configurable | long-running | Push overload paths, memory pressure, and distributed behavior. |

No preset should silently allocate unbounded memory. Stress presets may use very
large message counts, but mailbox capacity, DLQ capacity, runtime cap, and memory
sampling must remain explicit.

## 8. Scenario Matrix

### 8.1 Phase 1 - Core Local Runtime Scenarios

Phase 1 should implement the first three scenarios. They hit HPActor internals
hardest and give useful regression coverage early.

#### Scenario: `actor-creation`

CAF contract:

- Recursively create a binary tree of actors.
- Input `power = N` creates roughly `2^N` actors.
- Each actor waits for child results and returns a fan-in count to its parent.

HPActor topology:

```text
Coordinator
  -> root CreationNodeActor(power=N)
       -> two child CreationNodeActor(power=N-1)
       -> fan-in replies
```

Default dimensions:

| Preset | `power` | Expected role |
|--------|---------|---------------|
| `smoke` | 10-12 | Fast topology and teardown check. |
| `nightly` | 16-18 | Spawn/fan-in regression signal. |
| `paper-scale` | 20 | CAF-compatible million-actor scale. |
| `stress` | 21+ | Manual actor lifecycle and allocator stress. |

Primary metrics:

- total actors requested
- total actors completed
- wall-clock runtime
- actors created per second
- peak RSS
- actor memory-region delta
- spawn failures
- shutdown and teardown duration

Regression focus:

- actor creation overhead
- actor registry pressure
- scheduler work placement
- actor teardown and cleanup
- allocator behavior under short-lived actors

#### Scenario: `mailbox-n1`

CAF contract:

- Many sender actors send to one receiver actor.
- Historical paper-scale default: 100 senders, 1,000,000 messages per sender.
- Receiver processes all messages and reports completion.

HPActor topology:

```text
Coordinator
  -> Collector
  -> ReceiverActor(1)
  -> SenderActor[senders] -> ReceiverActor
```

Default dimensions:

| Preset | Senders | Messages per sender | Payload |
|--------|---------|---------------------|---------|
| `smoke` | 4-8 | 10,000 | header-only |
| `nightly` | 32-64 | 100,000 | header-only plus 1 KB sweep |
| `paper-scale` | 100 | 1,000,000 | header-only baseline |
| `stress` | configurable | configurable | full size and distribution sweep |

Primary metrics:

- total sent
- total received
- total rejected
- total dropped
- DLQ handoff count
- throughput messages per second
- p50/p95/p99/p999 enqueue-to-receive latency when timestamping is enabled
- receiver mailbox peak depth
- receiver mailbox pressure-state transitions
- scheduler wakeups and requeues if exposed
- peak RSS and message memory-region delta

Regression focus:

- MPSC producer contention
- bounded mailbox admission
- edge-triggered wakeup behavior
- receiver drain throughput
- memory pressure under queue growth

#### Scenario: `mixed-case`

CAF contract:

- Create many token rings.
- Each ring passes a token through a fixed number of actors.
- Rings are recreated several times.
- One worker per ring performs CPU work, historically prime factorization.

HPActor topology:

```text
Coordinator
  -> Collector
  -> RingMasterActor[rings]
       -> RingNodeActor[ring_size]
       -> CpuWorkerActor
```

Default dimensions:

| Preset | Rings | Ring size | Token value | Repetitions |
|--------|-------|-----------|-------------|-------------|
| `smoke` | 4 | 16 | 100 | 1 |
| `nightly` | 32 | 64 | 500 | 2 |
| `paper-scale` | 100 | 100 | 1000 | 4 |
| `stress` | configurable | configurable | configurable | configurable |

Primary metrics:

- rings completed
- actors created and stopped
- token hops completed
- CPU tasks completed
- wall-clock runtime
- per-ring completion spread
- scheduler fairness spread across rings
- actor lifecycle failures
- peak RSS and actor/message memory deltas

Regression focus:

- actor lifecycle churn
- mixed message and CPU scheduling
- fairness under many independent actor groups
- fan-in completion reporting

### 8.2 Phase 2 - Message Size, Shape, And Distribution Sweeps

Phase 2 extends `mailbox-n1` and adds standalone traffic-distribution scenarios.

Message sizes:

| Size | Lane |
|------|------|
| 0 B / header-only | smoke and all presets |
| 16 B | nightly and above |
| 64 B | nightly and above |
| 256 B | nightly and above |
| 1 KB | nightly and above |
| 4 KB | nightly and above |
| 16 KB | stress |
| 64 KB | stress |
| 1 MB | manual stress only |

Message shapes:

| Shape | Meaning |
|-------|---------|
| `header-only` | Minimal `TypedMessage` payload. |
| `fixed-bytes` | `StreamBuffer` filled to a fixed size. |
| `protobuf-small` | Small protobuf payload. |
| `protobuf-nested` | Nested protobuf payload to exercise encode/decode shape. |
| `shared-buffer` | Shared or zero-copy candidate path if available. |
| `mixed-80-20` | 80 percent small, 20 percent larger payloads. |

Traffic distributions:

| Distribution | Shape | Runtime pressure |
|--------------|-------|------------------|
| `one-to-one` | 1 sender, 1 receiver | Baseline send/receive overhead. |
| `n-to-one` | many senders, 1 receiver | MPSC contention and hotspot pressure. |
| `one-to-n` | 1 sender, many receivers | Fanout and scheduler wakeup spread. |
| `n-to-n-random` | many senders, many receivers | Routing and cache locality. |
| `ring` | actor i sends to i+1 | Steady-state fairness and latency. |
| `pipeline` | staged actors | Handoff and batching effects. |
| `zipf-hotspot` | skewed receiver choice | Overload concentration and fairness. |
| `bursty-waves` | send in waves with idle gaps | Queue growth and recovery. |

Phase 2 metrics add:

- bytes per second
- payload size histogram
- distribution seed
- per-receiver load skew
- per-sender throughput spread
- admission failure reasons by `FailureReason`
- drop and DLQ reasons

### 8.3 Phase 3 - Distributed And Microbenchmark Scenarios

#### Scenario: `distributed-ping`

CAF contract:

- Start server processes that publish actors.
- Benchmark process connects nodes and triggers ping/pong between remote actors.
- Complete after all ping actors report done.

HPActor topology:

```text
Node process A: RemotePingActor
Node process B: RemotePingActor
Benchmark driver: connects nodes, starts all-to-all ping/pong pairs
```

Default dimensions:

| Preset | Nodes | Pings per pair | Transport |
|--------|-------|----------------|-----------|
| `smoke` | 2 loopback processes | 1,000 | loopback |
| `nightly` | 2-4 loopback processes | 10,000 | loopback |
| `paper-scale` | configurable host list | configurable | TCP |
| `stress` | configurable | configurable | TCP with fault injection optional |

Primary metrics:

- connection setup time
- pings completed
- remote messages per second
- p50/p95/p99/p999 round-trip latency
- serialization bytes
- transport failures
- route-cache hits and misses if exposed
- node unavailable failures

#### Scenario: `message-creation`

HPActor focus:

- `TypedMessage` construction
- `StreamBuffer` construction
- payload copy/move behavior
- message-region allocation
- small-message inlining if enabled

Measurement:

- operations per second
- allocation count
- active bytes delta
- size-class distribution

#### Scenario: `dispatch-match`

HPActor focus:

- TypeTag dispatch through `EventBasedActor::on<T>()`
- typed actor dispatch where available
- dynamic protobuf message dispatch
- fast-tag dispatch if configured

Measurement:

- dispatch operations per second
- handler-match count
- fast-path versus normal-path delta

#### Scenario: `serialization`

HPActor focus:

- protobuf encode/decode
- `TypedMessage` frame construction
- binary payload sizes
- nested message shapes

Measurement:

- encode operations per second
- decode operations per second
- bytes per second
- allocation count
- malformed input rejection for test coverage

#### Scenario: `mandelbrot`

HPActor focus:

- CPU-heavy actor scheduling
- row actor versus row-chunk actor granularity
- scheduler fairness under dense compute

Required behavior:

- No image file output during timed portion.
- Problem size and max iterations are configurable.
- Work is deterministic for a given parameter set.

#### Scenario: `scheduling-mix`

HPActor focus:

- recursive actor creation
- CPU tasks
- actor pools
- scheduler thread count sweeps
- max actor messages per run or equivalent scheduling budget if exposed

This scenario should stay separate from `mixed-case`: `mixed-case` ports the
CAF benchmark contract, while `scheduling-mix` is an HPActor-specific scheduler
diagnostic inspired by CAF's scheduling benchmark.

## 9. Benchmark Architecture

### 9.1 File Layout

```text
apps/bench_caf/
├── CMakeLists.txt
├── 18_bench_caf.cpp
├── caf_bench_config.hpp
├── caf_bench_metrics.hpp
├── caf_bench_output.hpp
├── caf_bench_runner.hpp
├── caf_bench_scenarios.hpp
├── caf_bench_sampler.hpp
├── messages.hpp
├── actors/
│   ├── caf_bench_collector_actor.hpp
│   ├── caf_bench_coordinator_actor.hpp
│   ├── actor_creation_actor.hpp
│   ├── mailbox_n1_actor.hpp
│   ├── mixed_case_actor.hpp
│   ├── traffic_distribution_actor.hpp
│   ├── distributed_ping_actor.hpp
│   └── cpu_worker_actor.hpp
└── README.md
```

### 9.2 Components

| Component | Responsibility |
|-----------|----------------|
| `18_bench_caf.cpp` | CLI parsing, HPActor config setup, scenario selection, process exit codes. |
| `CafBenchConfig` | Parsed scenario, preset, scheduler, payload, distribution, and sampler options. |
| `CafBenchRunner` | Runs warmups, trials, runtime caps, sampler lifecycle, and output writing. |
| `CafBenchCollectorActor` | Receives scenario metrics, latency samples, counters, and completion events. |
| `CafBenchCoordinatorActor` | Owns scenario lifecycle for actor-based scenarios. |
| `CafBenchSampler` | Samples RSS every configured interval and collects optional runtime counters. |
| `CafBenchOutput` | Serializes stable JSON and CSV output. |
| Scenario actors | Implement one scenario contract each. |

### 9.3 Runtime Configuration

Benchmark runs should construct an explicit `hpactor::Config`:

- `scheduler_threads` from flag or preset.
- `mailbox.default_capacity` from flag or preset.
- CLI disabled for headless mode.
- actor metrics enabled when built with metrics support.
- tracing enabled only when a scenario asks for tracing overhead measurement.
- shutdown drain configured with a finite timeout.

The benchmark should record the final runtime configuration in output. This
prevents misreading results from two runs with different scheduler or mailbox
settings.

### 9.4 Delivery Path Modes

| Mode | Meaning | Default |
|------|---------|---------|
| `normal` | Use public HPActor delivery path with delivery semantics and observability. | yes |
| `fast-local` | Use an explicit local fast path for hot-path microbenchmarks only. | no |

If `fast-local` is used, output must include:

- `delivery_path: "fast-local"`
- checks intentionally skipped
- whether TTL, dedup, DLQ, tracing, backpressure, quarantine, and failure
  envelopes were bypassed

## 10. Measurement Method

### 10.1 Trial Model

Each benchmark run contains:

1. Process setup and runtime config recording.
2. Warmup iterations, not included in summary metrics.
3. Independent measured trials.
4. RSS and optional internal counter sampling during each trial.
5. Output of raw trial records plus summary statistics.

Recommended defaults:

| Preset | Warmups | Trials |
|--------|---------|--------|
| `smoke` | 0 | 1 |
| `nightly` | 1 | 3 |
| `paper-scale` | 1 | 10 |
| `stress` | configurable | configurable |

### 10.2 Time And Memory

CAF's benchmark runner sampled process memory while measuring wall-clock time.
HPActor should do the same:

- wall-clock time: `std::chrono::steady_clock`
- RSS sampling interval: default 50 ms
- Linux RSS source: `/proc/<pid>/status` or `/proc/self/statm`
- macOS RSS source: Mach task info
- max runtime cap per trial

RSS sampling should run outside cooperative actor handlers. It may be a simple
host thread owned by the runner, because it performs process-level sampling and
must not block actor scheduler workers.

### 10.3 HPActor Internal Counters

When available, output should include:

| Counter family | Examples |
|----------------|----------|
| Mailbox | peak depth, lane depths, admission failures, overflow policy actions, pressure transitions. |
| Delivery | failure reason counts, DLQ handoffs, deadline expiry, dedup suppressions. |
| Scheduler | worker count, requeue count, steal attempts, successful steals, idle time if exposed. |
| Memory | region active bytes, allocation count, rejection count, high-water mark, slab fallback count. |
| Actor lifecycle | actors spawned, actors stopped, forced shutdowns, drain duration. |
| Network | bytes sent/received, connect failures, route misses, serialization failures. |

The first implementation may omit counters that do not yet have public
introspection APIs. Missing counter families must appear in output as
`available: false`, not as zero values.

### 10.4 Output Schema

JSON output should keep raw trials and summaries separate:

```json
{
  "schema_version": 1,
  "benchmark": "caf-port",
  "scenario": "mailbox-n1",
  "preset": "smoke",
  "git": {
    "commit": "unknown",
    "branch": "unknown",
    "dirty": false
  },
  "runtime": {
    "scheduler_threads": 8,
    "mailbox_capacity": 4096,
    "delivery_path": "normal"
  },
  "parameters": {
    "senders": 4,
    "messages_per_sender": 10000,
    "message_size_bytes": 0,
    "message_shape": "header-only",
    "distribution": "n-to-one",
    "seed": 1
  },
  "summary": {
    "trials": 1,
    "mean_runtime_ms": 0,
    "mean_throughput_msgps": 0,
    "peak_rss_bytes": 0
  },
  "trials": [
    {
      "trial": 1,
      "runtime_ms": 0,
      "throughput_msgps": 0,
      "total_sent": 0,
      "total_received": 0,
      "latency": {
        "available": false
      },
      "rss_samples": []
    }
  ],
  "counters": {
    "mailbox": {
      "available": false
    },
    "scheduler": {
      "available": false
    },
    "memory": {
      "available": false
    }
  }
}
```

CSV output should contain one row per trial and include stable columns for:

- scenario
- preset
- trial
- scheduler threads
- message size
- message shape
- distribution
- runtime ms
- throughput msgps
- p50/p95/p99/p999 latency us when available
- peak RSS bytes
- sent, received, rejected, dropped

## 11. Error Handling

| Condition | Handling |
|-----------|----------|
| Unknown scenario | Exit code 1, print valid scenario names. |
| Unknown preset | Exit code 1, print valid presets. |
| Invalid parameter combination | Exit code 2, print the conflicting flags. |
| Scenario timeout | Exit code 3, emit partial JSON with `completed: false`. |
| Actor spawn failure | Exit code 4, include failure reason where available. |
| Output write failure | Exit code 5, print path and error category. |
| Distributed connection failure | Exit code 6, include node address and failure reason. |
| Sampler unavailable | Continue with `rss.available: false` unless RSS sampling was required. |

Examples of invalid combinations:

- `--message-size 1048576 --preset smoke`
- `--distribution zipf-hotspot` on a scenario that only supports one receiver
- `--delivery-path fast-local` with distributed scenarios
- `--trials 0`
- `--scheduler-threads 0` for scenarios that need live worker threads

## 12. Testing Strategy

### 12.1 Unit Tests

| Test file | Scope |
|-----------|-------|
| `tests/unit/apps/test_bench_caf_config.cpp` | CLI parsing, presets, validation, seed handling. |
| `tests/unit/apps/test_bench_caf_output.cpp` | JSON and CSV schema stability. |
| `tests/unit/apps/test_bench_caf_sampler.cpp` | RSS parsing, sample aggregation, unavailable sampler behavior. |
| `tests/unit/apps/test_bench_caf_payloads.cpp` | Payload generation for size and shape modes. |
| `tests/unit/apps/test_bench_caf_metrics.cpp` | Summary statistics, percentiles, counter merging. |

### 12.2 Integration Tests

| Test file | Scope |
|-----------|-------|
| `tests/integration/apps/test_bench_caf_actor_creation.cpp` | Small recursive spawn/fan-in completes and reports expected actor count. |
| `tests/integration/apps/test_bench_caf_mailbox_n1.cpp` | Many sender actors complete against one receiver with expected counts. |
| `tests/integration/apps/test_bench_caf_mixed_case.cpp` | Small ring recreation and CPU worker workload complete. |
| `tests/integration/apps/test_bench_caf_distribution.cpp` | Traffic distributions route expected counts under deterministic seeds. |

### 12.3 System Tests

| Test file | Scope |
|-----------|-------|
| `tests/system/apps/test_bench_caf_smoke.cpp` | Binary runs smoke scenarios and emits valid JSON. |
| `tests/system/apps/test_bench_caf_csv.cpp` | Binary emits stable CSV headers and one row per trial. |
| `tests/system/apps/test_bench_caf_distributed_smoke.cpp` | Optional loopback distributed smoke when networking is enabled. |

### 12.4 Regression Gates

CI should avoid hard-coded throughput thresholds. Stable gates should verify:

- process exits successfully
- output schema parses
- total sent and received counts match the scenario contract unless drops are
  expected
- no unexpected delivery failure reasons appear in smoke mode
- runtime is below a generous timeout
- RSS sampler records at least one sample when enabled

Nightly jobs may compare trend metrics against recent baselines and flag large
relative changes for review. Trend alerts should not fail normal PR CI without
human calibration.

## 13. Implementation Phases

### Phase 1 - Core CAF Local Runtime Port

Deliver:

- `apps/bench_caf/` scaffold and binary.
- Config parser for scenario, preset, trial, scheduler, output, and sampler
  flags.
- JSON and CSV output writer.
- RSS sampler.
- `actor-creation`.
- `mailbox-n1`.
- `mixed-case`.
- Smoke system test for all three scenarios.

Why first:

These three scenarios hit actor creation, mailbox contention, scheduler work
placement, actor lifecycle churn, allocator behavior, and mixed CPU/message
fairness. They provide the strongest early regression value.

### Phase 2 - Message Size, Shape, And Distribution Sweeps

Deliver:

- Message-size sweep support.
- Message-shape support.
- Traffic distribution scenarios.
- Per-receiver and per-sender skew metrics.
- Payload histogram output.
- Nightly preset support.

Why second:

This turns the CAF-compatible baseline into an HPActor-specific diagnostic for
allocation, dispatch, mailbox pressure, and workload skew.

### Phase 3 - Distributed And Microbenchmark Coverage

Deliver:

- `distributed-ping` loopback mode.
- `message-creation`.
- `dispatch-match`.
- `serialization`.
- `mandelbrot`.
- `scheduling-mix`.
- Optional distributed smoke test gated on networking support.

Why third:

These scenarios need more runtime surface area and stronger platform guards.
They are valuable, but they should build on a stable runner and output schema.

## 14. Acceptance Criteria

The design is implemented when:

1. `apps/bench_caf/18_bench_caf` builds when `ENABLE_APPS=ON`.
2. `--scenario actor-creation --preset smoke --format json` runs and emits
   valid JSON.
3. `--scenario mailbox-n1 --preset smoke --format json` runs and emits valid
   JSON with sent and received counts.
4. `--scenario mixed-case --preset smoke --format json` runs and emits valid
   JSON with completed ring and CPU-work counts.
5. `--format csv` emits stable headers and one row per measured trial.
6. `git diff --check` is clean for the implementation branch.
7. Smoke tests cover the three Phase 1 scenarios.
8. The documentation maps each implemented scenario to the HPActor internals it
   stresses.
9. Output records scenario parameters, scheduler configuration, delivery path,
   git metadata, and whether each internal counter family is available.
10. Normal delivery mode is the default for actor-system scenarios.

## 15. Review Notes

This benchmark should remain separate from the Savina stress suite. Savina is a
broad actor benchmark catalog. The CAF port is a smaller, CAF-compatible
performance and regression surface with explicit HPActor extensions for message
size, message shape, traffic distribution, and internal counters.

The recommended order is:

1. Port `actor-creation`, `mailbox-n1`, and `mixed-case`.
2. Add message-size, message-shape, and traffic-distribution sweeps.
3. Add distributed ping/pong and serialization/dispatch microbenchmarks.

That sequence gives HPActor useful regression coverage early and leaves the more
platform-sensitive distributed and microbenchmark work for a stable second pass.
