# CAF Performance Benchmark Phase 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add distributed ping/pong, message-creation, dispatch-match, serialization microbenchmarks, and the mandelbrot and scheduling-mix scenarios. Complete the CAF-style benchmark surface.

**Architecture:** Phase 3 adds six scenarios that fall into two categories. *Distributed ping/pong* is a full actor-system scenario that needs networking enabled and loopback transports for smoke tests. The four *microbenchmarks* (`message-creation`, `dispatch-match`, `serialization`, `scheduling-mix`) measure individual HPActor internal paths in tight loops without actor spawn/schedule/teardown overhead — they are pure single-threaded diagnostic benchmarks. *Mandelbrot* is a CPU-bound scheduling scenario that stresses the scheduler under dense compute, distinct from mixed-case (which blends message passing with CPU work).

**Tech Stack:** C++20, HPActor `ActorSystem`, `EventBasedActor`, `DenseComputingActor`, `TypedMessage`, `StreamBuffer`, Protobuf, kqueue/epoll EventLoop for loopback transports, GTest, CMake/Ninja, existing `18_bench_caf` binary.

## Global Constraints

- Do not copy CAF implementation code.
- Do not use CAF as a dependency.
- Do not replace `apps/bench_perf/` or `apps/bench_saturate/`.
- Do not require distributed tests in normal CI (loopback-only for smoke; multi-process gated behind a CMake option).
- Do not make benchmark success depend on exact throughput values from one developer machine.
- Do not bypass HPActor delivery semantics by default. Fast-path variants must state which checks are skipped.
- Every design or implementation write MUST happen in an isolated git worktree.
- Production code changes MUST follow RED → GREEN → REFACTOR.
- NEVER introduce `dynamic_cast`, `typeid`, exception-based control flow, or public APIs that require RTTI or exceptions.
- Preserve actor boundaries; avoid shared mutable state between actors.
- One serialized consumer per actor mailbox.
- Bounded capacity failures are counted and reported.
- Networking scenarios (`distributed-ping`) must use loopback for smoke tests; multi-process mode is manual-only.

---

## File Structure

Create these app files:

```text
apps/bench_caf/
├── caf_bench_config.hpp          (MODIFY — add ScenarioKind values, delivery-path flag)
├── caf_bench_runner.hpp          (MODIFY — dispatch new scenarios)
├── caf_bench_scenarios.hpp       (MODIFY — add scenario entry points)
├── caf_bench_micro.hpp           (CREATE — microbenchmark harness)
└── actors/
    ├── distributed_ping_actor.hpp (CREATE)
    ├── mandelbrot_actor.hpp       (CREATE)
    └── scheduling_mix_actor.hpp   (CREATE)
```

Modify these build files:

```text
apps/bench_caf/CMakeLists.txt
tests/integration/apps/CMakeLists.txt
tests/system/apps/CMakeLists.txt
tests/unit/apps/CMakeLists.txt
```

Create these tests:

```text
tests/unit/apps/test_bench_caf_micro.cpp                  (CREATE)
tests/integration/apps/test_bench_caf_mandelbrot.cpp       (CREATE)
tests/integration/apps/test_bench_caf_scheduling_mix.cpp   (CREATE)
tests/integration/apps/test_bench_caf_distributed_ping.cpp (CREATE)
tests/system/apps/test_bench_caf_distributed_smoke.cpp     (CREATE — loopback only)
```

---

## Shared Interfaces

All types live under `hpactor::apps::bench_caf`.

### Extended ScenarioKind (append to `caf_bench_config.hpp`)

```cpp
enum class ScenarioKind {
    // Phase 1
    ActorCreation, MailboxN1, MixedCase,
    // Phase 2
    TrafficOneToOne, TrafficOneToN, TrafficNToNRandom,
    TrafficRing, TrafficPipeline, TrafficZipf, TrafficBursty,
    // Phase 3
    DistributedPing,
    MessageCreation,
    DispatchMatch,
    Serialization,
    Mandelbrot,
    SchedulingMix,
};

// New config field
enum class DeliveryPath {
    Normal,    // full delivery pipeline (default)
    FastLocal, // bypass TTL, dedup, DLQ, tracing, backpressure
};
```

### DeliveryPath On CafBenchConfig

```cpp
struct CafBenchConfig {
    // ... existing fields ...
    DeliveryPath delivery_path = DeliveryPath::Normal;
};
```

### Microbenchmark Harness (`caf_bench_micro.hpp`)

```cpp
struct MicroResult {
    uint64_t iterations = 0;
    uint64_t runtime_ns = 0;
    uint64_t ops_per_sec = 0;
    uint64_t alloc_count = 0;
    uint64_t alloc_bytes = 0;
};

// Runs a microbenchmark lambda `op_count` times, measures wall-clock.
// Returns ops/sec and allocation delta.
MicroResult run_micro_benchmark(std::function<void()> setup,
                                 std::function<void()> op,
                                 uint64_t op_count);
```

### Scenario Entry Points

```cpp
TrialMetrics run_distributed_ping_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_message_creation_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_dispatch_match_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_serialization_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_mandelbrot_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_scheduling_mix_trial(const CafBenchConfig& cfg, uint32_t idx);
```

---

### Task 1: Microbenchmark Harness And Message-Creation

**Files:**
- Create: `apps/bench_caf/caf_bench_micro.hpp`
- Create: `tests/unit/apps/test_bench_caf_micro.cpp`
- Modify: `apps/bench_caf/caf_bench_config.hpp`
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`
- Modify: `apps/bench_caf/caf_bench_runner.hpp`
- Modify: `tests/unit/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: `TrialMetrics`, existing config types.
- Produces: `run_micro_benchmark()`, `run_message_creation_trial()`, `MicroResult`.

**Message-creation benchmark:**

This is a pure microbenchmark — no actors. It measures the cost of constructing `TypedMessage` objects with various payload sizes and shapes.

```cpp
TrialMetrics run_message_creation_trial(const CafBenchConfig& cfg, uint32_t idx) {
    // 1. Allocate payloads with encode_shaped_payload() for the requested size/shape.
    // 2. Use run_micro_benchmark() to time TypedMessage(tag, payload) construction.
    // 3. Report ops/sec, alloc counts, and active bytes delta.
    // 4. No ActorSystem is created.
}
```

**MicroResult harness:**

```cpp
MicroResult run_micro_benchmark(std::function<void()> setup,
                                 std::function<void()> op,
                                 uint64_t op_count) {
    // 1. Record baseline alloc state.
    // 2. Call setup() once.
    // 3. Start steady_clock.
    // 4. Call op() op_count times in a tight loop.
    // 5. Stop clock, record final alloc state.
    // 6. Compute ops/sec, alloc delta.
    // 7. Return MicroResult.
}
```

- [ ] **Step 1: Write the failing micro harness test**

```cpp
TEST(MicroBenchmark, ComputesOpsPerSec) {
    uint64_t counter = 0;
    auto result = bench_caf::run_micro_benchmark(
        []{}, [&counter]{ ++counter; }, 1000000);
    EXPECT_GT(result.ops_per_sec, 0u);
    EXPECT_EQ(counter, 1000000u);
}

TEST(MessageCreation, SmokeCompletes) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MessageCreation;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.message_size_bytes = 0;

    auto metrics = bench_caf::run_message_creation_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_GT(metrics.throughput_msgps, 0.0);
}
```

- [ ] **Step 2: Run the tests to verify RED**

Expected: compile fails — `caf_bench_micro.hpp` does not exist.

- [ ] **Step 3: Implement micro harness and message-creation**

Create `caf_bench_micro.hpp` with `run_micro_benchmark()`. Implement `run_message_creation_trial()` that times `TypedMessage(MailboxLoadTag, payload)` construction across a range of sizes.

Update `ScenarioKind` enum and scenario-name/parse functions.

Update `caf_bench_runner.hpp` to dispatch `MessageCreation`.

- [ ] **Step 4: Run the tests to verify GREEN**

```bash
ninja -C build test_unit_apps
./build/tests/unit/apps/test_unit_apps --gtest_filter="MicroBenchmark.*:MessageCreation.*"
```

- [ ] **Step 5: Commit Task 1**

```bash
git add apps/bench_caf/caf_bench_micro.hpp apps/bench_caf/caf_bench_config.hpp apps/bench_caf/caf_bench_scenarios.hpp apps/bench_caf/caf_bench_runner.hpp tests/unit/apps/CMakeLists.txt tests/unit/apps/test_bench_caf_micro.cpp
git commit -m "feat(bench-caf): add microbenchmark harness and message-creation"
```

---

### Task 2: Dispatch-Match Microbenchmark

**Files:**
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`
- Modify: `tests/unit/apps/test_bench_caf_micro.cpp`
- Modify: `apps/bench_caf/caf_bench_config.hpp`

**Interfaces:**
- Produces: `run_dispatch_match_trial()`.

**Dispatch-match benchmark:**

Measures TypeTag-based dispatch cost. Tests three dispatch paths:
1. `EventBasedActor::on<T>()` — protobuf handler dispatch via `proto_handlers_` map lookup.
2. Typed actor dispatch — compile-time typed handler dispatch.
3. Fast-tag dispatch — `add_fast_tag()` path that skips drain/lifecycle gates.

Each path is measured by sending `N` messages to a single actor and timing the total processing time.

```cpp
TrialMetrics run_dispatch_match_trial(const CafBenchConfig& cfg, uint32_t idx) {
    // 1. Create an ActorSystem with 1 worker thread.
    // 2. Spawn one actor with a registered handler for MailboxLoadTag.
    // 3. Optionally add fast tag if delivery_path == FastLocal.
    // 4. Send N messages directly via deliver_local.
    // 5. Wait for all messages to be processed (counter).
    // 6. Compute ops/sec = N / runtime_sec.
    // 7. Shut down system.
}
```

- [ ] **Step 1: Write the failing dispatch-match tests**

```cpp
TEST(DispatchMatch, NormalPathSmokeCompletes) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::DispatchMatch;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.delivery_path = bench_caf::DeliveryPath::Normal;

    auto metrics = bench_caf::run_dispatch_match_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_GT(metrics.throughput_msgps, 0.0);
}

TEST(DispatchMatch, FastLocalPathSmokeCompletes) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::DispatchMatch;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.delivery_path = bench_caf::DeliveryPath::FastLocal;

    auto metrics = bench_caf::run_dispatch_match_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_GT(metrics.throughput_msgps, 0.0);
}
```

- [ ] **Step 2: Run the tests to verify RED**

Expected: compile fails.

- [ ] **Step 3: Implement dispatch-match**

Create `DispatchMatchActor` in the scenarios header (inline). Add `run_dispatch_match_trial()`. Wire `ScenarioKind::DispatchMatch` and `DeliveryPath` parsing.

- [ ] **Step 4: Run the tests to verify GREEN**

```bash
ninja -C build test_unit_apps
./build/tests/unit/apps/test_unit_apps --gtest_filter="DispatchMatch.*"
```

- [ ] **Step 5: Commit Task 2**

```bash
git add apps/bench_caf/caf_bench_scenarios.hpp apps/bench_caf/caf_bench_config.hpp apps/bench_caf/caf_bench_runner.hpp tests/unit/apps/test_bench_caf_micro.cpp
git commit -m "feat(bench-caf): add dispatch-match microbenchmark"
```

---

### Task 3: Serialization Microbenchmark

**Files:**
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`
- Modify: `tests/unit/apps/test_bench_caf_micro.cpp`
- Modify: `apps/bench_caf/caf_bench_config.hpp`
- Modify: `apps/bench_caf/messages.hpp`

**Interfaces:**
- Produces: `run_serialization_trial()`.

**Serialization benchmark:**

Measures protobuf serialize/deserialize cost using `BenchSmallPayload` and `BenchNestedPayload` from Phase 2.

```cpp
TrialMetrics run_serialization_trial(const CafBenchConfig& cfg, uint32_t idx) {
    // 1. Construct BenchSmallPayload or BenchNestedPayload per message_shape.
    // 2. Use run_micro_benchmark() to time:
    //    a. SerializeToArray() — encode cost.
    //    b. ParseFromArray() — decode cost.
    // 3. Report ops/sec for encode and decode separately.
    // 4. Report allocation delta and bytes/sec.
}
```

- [ ] **Step 1: Write the failing serialization tests**

```cpp
TEST(Serialization, ProtoEncodeSmokeCompletes) { ... }
TEST(Serialization, ProtoDecodeSmokeCompletes) { ... }
TEST(Serialization, NestedProtoRoundTrips) { ... }
```

- [ ] **Step 2: Run the tests to verify RED**

Expected: compile fails.

- [ ] **Step 3: Implement serialization benchmark**

Add `run_serialization_trial()`. Wire `ScenarioKind::Serialization`. The benchmark uses `run_micro_benchmark()` for both encode and decode measurements, reporting two sets of throughput numbers.

- [ ] **Step 4: Run the tests to verify GREEN**

- [ ] **Step 5: Commit Task 3**

```bash
git commit -m "feat(bench-caf): add serialization microbenchmark"
```

---

### Task 4: Mandelbrot CPU Scheduling Scenario

**Files:**
- Create: `apps/bench_caf/actors/mandelbrot_actor.hpp`
- Create: `tests/integration/apps/test_bench_caf_mandelbrot.cpp`
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`
- Modify: `apps/bench_caf/caf_bench_runner.hpp`
- Modify: `tests/integration/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: `CafBenchConfig`, `ActorSystem`, `EventBasedActor`.
- Produces: `run_mandelbrot_trial()`.

**Mandelbrot scenario:**

CAF contract: compute the Mandelbrot set, one row per actor (or one row chunk per actor for larger workloads). No image file I/O during timed execution. Configurable problem size (width × height in pixels) and max iterations.

HPActor topology:

```text
Coordinator
  -> MasterActor(width, height, max_iterations)
       -> RowWorkerActor[row_start, row_end, max_iterations] × num_workers
       -> fan-in results with pixel iteration counts
```

**Default dimensions:**

| Preset | Width × Height | Max Iterations | Workers | Granularity |
|--------|---------------|----------------|---------|-------------|
| `smoke` | 128 × 128 | 256 | 4 | 32 rows/worker |
| `nightly` | 512 × 512 | 1,024 | num_cores | 16 rows/worker |
| `paper-scale` | 1,024 × 1,024 | 4,096 | num_cores | 8 rows/worker |
| `stress` | 4,096 × 4,096 | 10,000 | num_cores × 2 | 4 rows/worker |

**Mandelbrot computation (deterministic, no I/O):**

```cpp
// Returns iteration count at pixel (px, py). 0 = in set (max_iterations reached).
uint32_t mandelbrot_iterations(double cx, double cy, uint32_t max_iters) {
    double zx = 0.0, zy = 0.0;
    uint32_t iter = 0;
    while (zx * zx + zy * zy <= 4.0 && iter < max_iters) {
        double tmp = zx * zx - zy * zy + cx;
        zy = 2.0 * zx * zy + cy;
        zx = tmp;
        ++iter;
    }
    return iter;
}
```

**RowWorkerActor:**

Each worker receives a `MandelComputeTask` message with row range and bounds. It computes the Mandelbrot escape iterations for each pixel in its rows and returns the total via a `MandelRowDone` message.

- [ ] **Step 1: Write the failing mandelbrot integration test**

```cpp
TEST(Mandelbrot, SmokeCompletesComputation) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::Mandelbrot;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;

    auto metrics = bench_caf::run_mandelbrot_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_GT(metrics.actors_created, 0u);
    EXPECT_GT(metrics.runtime_ms, 0u);
}
```

- [ ] **Step 2: Run the test to verify RED**

- [ ] **Step 3: Implement mandelbrot**

Create `apps/bench_caf/actors/mandelbrot_actor.hpp` with `RowWorkerActor` and `MandelMasterActor`. The master spawns row workers, distributes row ranges, collects results, and signals completion.

Add `mandelbrot_dimensions_for_preset()` for configurable width/height/iterations/workers.

- [ ] **Step 4: Run the test to verify GREEN**

- [ ] **Step 5: Commit Task 4**

```bash
git add apps/bench_caf/actors/mandelbrot_actor.hpp apps/bench_caf/caf_bench_scenarios.hpp apps/bench_caf/caf_bench_runner.hpp tests/integration/apps/CMakeLists.txt tests/integration/apps/test_bench_caf_mandelbrot.cpp
git commit -m "feat(bench-caf): add mandelbrot cpu scheduling scenario"
```

---

### Task 5: Scheduling-Mix Scenario

**Files:**
- Create: `apps/bench_caf/actors/scheduling_mix_actor.hpp`
- Create: `tests/integration/apps/test_bench_caf_scheduling_mix.cpp`
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`
- Modify: `apps/bench_caf/caf_bench_runner.hpp`
- Modify: `tests/integration/apps/CMakeLists.txt`

**Interfaces:**
- Produces: `run_scheduling_mix_trial()`.

**Scheduling-mix scenario:**

HPActor-specific scheduler diagnostic (distinct from mixed-case). Combines:
1. Recursive actor creation (like actor-creation but smaller trees, repeated).
2. CPU tasks (factorization).
3. Actor pools (pre-spawned workers that receive tasks).
4. Scheduler thread count sweeps.

```text
Coordinator
  -> SpawnWaveActor[waves] — recursive creation bursts
  -> CpuPoolActor[pool_size] — CPU task processing
  -> MessageRingActor[ring_size] — sustained message flow
```

**Default dimensions:**

| Preset | Waves | Tree depth | Pool size | Ring size | Messages |
|--------|-------|-----------|-----------|-----------|----------|
| `smoke` | 2 | 6 | 4 | 8 | 1,000 |
| `nightly` | 4 | 10 | 8 | 16 | 10,000 |
| `paper-scale` | 8 | 14 | 16 | 32 | 100,000 |
| `stress` | 16 | 18 | 32 | 64 | 1,000,000 |

**Key difference from mixed-case:** Mixed-case measures one ring at a time with CPU tasks interleaved. Scheduling-mix runs multiple concurrent workload types simultaneously, stressing the scheduler's ability to fairly allocate worker time across heterogeneous workloads (spawn bursts, CPU-bound tasks, and message-passing rings).

- [ ] **Step 1: Write the failing scheduling-mix integration test**

```cpp
TEST(SchedulingMix, SmokeCompletesAllWorkloads) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::SchedulingMix;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;

    auto metrics = bench_caf::run_scheduling_mix_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_GT(metrics.actors_created, 0u);
    EXPECT_GT(metrics.cpu_tasks_completed, 0u);
    EXPECT_GT(metrics.token_hops, 0u);
}
```

- [ ] **Step 2: Run the test to verify RED**

- [ ] **Step 3: Implement scheduling-mix**

Create `apps/bench_caf/actors/scheduling_mix_actor.hpp` with `SpawnWaveActor` (recursive tree creation), `CpuPoolWorkerActor` (CPU task processing), and `MessageRingActor` (ring message passing). The scenario runner spawns all three workload types concurrently and waits for all to complete.

- [ ] **Step 4: Run the test to verify GREEN**

- [ ] **Step 5: Commit Task 5**

```bash
git add apps/bench_caf/actors/scheduling_mix_actor.hpp apps/bench_caf/caf_bench_scenarios.hpp apps/bench_caf/caf_bench_runner.hpp tests/integration/apps/CMakeLists.txt tests/integration/apps/test_bench_caf_scheduling_mix.cpp
git commit -m "feat(bench-caf): add scheduling-mix scenario"
```

---

### Task 6: Distributed Ping/Pong

**Files:**
- Create: `apps/bench_caf/actors/distributed_ping_actor.hpp`
- Create: `tests/integration/apps/test_bench_caf_distributed_ping.cpp`
- Create: `tests/system/apps/test_bench_caf_distributed_smoke.cpp`
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`
- Modify: `apps/bench_caf/caf_bench_runner.hpp`
- Modify: `tests/integration/apps/CMakeLists.txt`
- Modify: `tests/system/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: `ActorSystem` with `enable_network = true`, loopback transport.
- Produces: `run_distributed_ping_trial()`.

**Distributed ping/pong scenario:**

CAF contract: start nodes with published ping actors, connect, exchange pings, report done.

HPActor topology:

```text
Process A:
  -> PingActor A1, A2, ..., An

Process B:
  -> PingActor B1, B2, ..., Bn

Benchmark driver (process):
  -> Connects to both processes
  -> Initiates all-to-all ping/pong between actors
  -> Waits for completion
```

**Loopback smoke mode:**

For CI, run both "processes" in-process with loopback transport. Two `ActorSystem` instances on separate ports, connected via localhost TCP. Each system spawns `PingActor` instances. The benchmark driver (third `ActorSystem` or the main process) coordinates.

```cpp
// Loopback smoke: two ActorSystem instances, localhost TCP
ActorSystem node_a(config_endpoint("127.0.0.1", 51000));
ActorSystem node_b(config_endpoint("127.0.0.1", 51001));

// Spawn ping actors on each node
for each node: spawn PingActor × N

// Cross-connect: node_a's actors ping node_b's actors
// Each actor sends pings_per_pair messages, counts responses
```

**Default dimensions:**

| Preset | Nodes | Pings per pair | Transport | Timeout |
|--------|-------|----------------|-----------|---------|
| `smoke` | 2 (loopback) | 1,000 | TCP localhost | 15s |
| `nightly` | 2–4 (loopback) | 10,000 | TCP localhost | 30s |
| `paper-scale` | host list | configurable | TCP | 60s |
| `stress` | host list | configurable | TCP + fault injection | 120s |

**PingActor:**

```cpp
class DistributedPingActor : public EventBasedActor {
    // Receives PingMsg, replies with PongMsg.
    // Sender counts pongs received.
    // After all pings complete, reports done to coordinator.
};
```

- [ ] **Step 1: Write the failing distributed ping integration test**

```cpp
TEST(DistributedPing, LoopbackSmokeCompletes) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::DistributedPing;
    cfg.preset = bench_caf::PresetKind::Smoke;

    auto metrics = bench_caf::run_distributed_ping_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_EQ(metrics.total_sent, metrics.total_received);
    EXPECT_GT(metrics.throughput_msgps, 0.0);
}
```

- [ ] **Step 2: Run the tests to verify RED**

- [ ] **Step 3: Implement distributed ping/pong**

Create `apps/bench_caf/actors/distributed_ping_actor.hpp`. The scenario runner:
1. Creates two ActorSystem instances with `enable_network = true` and loopback endpoints.
2. Spawns ping actors on each node.
3. Initiates cross-node ping/pong via `ActorRef::send()`.
4. Waits for all pongs.
5. Shuts down both systems.
6. Reports pings completed, remote messages per second, and round-trip latency.

Gated behind networking support; skip test if networking is disabled at build time.

Add `--delivery-path` flag parsing. Distributed scenarios must reject `FastLocal` with exit code 2.

- [ ] **Step 4: Run the tests to verify GREEN**

```bash
ninja -C build test_bench_caf_distributed_ping test_bench_caf_distributed_smoke
./build/tests/integration/apps/test_bench_caf_distributed_ping
./build/tests/system/apps/test_bench_caf_distributed_smoke
```

- [ ] **Step 5: Commit Task 6**

```bash
git add apps/bench_caf/actors/distributed_ping_actor.hpp apps/bench_caf/caf_bench_scenarios.hpp apps/bench_caf/caf_bench_runner.hpp apps/bench_caf/caf_bench_config.hpp tests/integration/apps/CMakeLists.txt tests/integration/apps/test_bench_caf_distributed_ping.cpp tests/system/apps/CMakeLists.txt tests/system/apps/test_bench_caf_distributed_smoke.cpp
git commit -m "feat(bench-caf): add distributed ping/pong scenario"
```

---

### Task 7: Final Verification, README Update, And Design Spec Sign-off

**Files:**
- Modify: `apps/bench_caf/README.md`
- Modify: `docs/superpowers/specs/2026-06-26-caf-performance-benchmark-design.md`

- [ ] **Step 1: Update README with Phase 3 scenarios**

Add Phase 3 scenario table to `apps/bench_caf/README.md`:

```markdown
## Phase 3 Scenarios

| Scenario | Purpose |
|----------|---------|
| `distributed-ping` | All-to-all remote ping/pong via loopback or TCP. |
| `message-creation` | `TypedMessage` and `StreamBuffer` construction cost. |
| `dispatch-match` | TypeTag, typed actor, and fast-tag dispatch paths. |
| `serialization` | Protobuf encode/decode cost. |
| `mandelbrot` | CPU-heavy Mandelbrot computation, scheduler fairness. |
| `scheduling-mix` | Concurrent spawn bursts, CPU tasks, and message rings. |
```

- [ ] **Step 2: Update the design spec status**

```markdown
**Status:** Phase 3 implemented
```

- [ ] **Step 3: Run final targeted verification**

```bash
ninja -C build 18_bench_caf test_unit_apps \
  test_bench_caf_actor_creation test_bench_caf_mailbox_n1 \
  test_bench_caf_mixed_case test_bench_caf_distribution \
  test_bench_caf_mandelbrot test_bench_caf_scheduling_mix \
  test_bench_caf_distributed_ping \
  test_bench_caf_smoke test_bench_caf_distributed_smoke

# Unit tests
./build/tests/unit/apps/test_unit_apps --gtest_filter="BenchCaf*:MicroBench*:Dispatch*:Serialization*"

# Integration tests
./build/tests/integration/apps/test_bench_caf_actor_creation
./build/tests/integration/apps/test_bench_caf_mailbox_n1
./build/tests/integration/apps/test_bench_caf_mixed_case
./build/tests/integration/apps/test_bench_caf_distribution
./build/tests/integration/apps/test_bench_caf_mandelbrot
./build/tests/integration/apps/test_bench_caf_scheduling_mix
./build/tests/integration/apps/test_bench_caf_distributed_ping

# System tests
./build/tests/system/apps/test_bench_caf_smoke
./build/tests/system/apps/test_bench_caf_distributed_smoke

# Binary smoke
./build/apps/bench_caf/18_bench_caf --scenario mandelbrot --preset smoke --format json
./build/apps/bench_caf/18_bench_caf --scenario scheduling-mix --preset smoke --format json
./build/apps/bench_caf/18_bench_caf --scenario distributed-ping --preset smoke --format json
./build/apps/bench_caf/18_bench_caf --scenario message-creation --preset smoke --format json
./build/apps/bench_caf/18_bench_caf --scenario dispatch-match --preset smoke --format json
./build/apps/bench_caf/18_bench_caf --scenario serialization --preset smoke --format json

git diff --check
```

Expected:
- All selected targets build.
- All selected tests pass (distributed tests skipped if networking unavailable).
- All six new scenario CLI invocations exit 0 and emit valid JSON/CSV.
- `git diff --check` exits 0.
- Phase 1 and Phase 2 scenarios unchanged in behavior.

- [ ] **Step 4: Record no full-suite run**

In the final implementation response, state:

```text
I did not run the full C++ test suite; verification used targeted app/unit/integration/system tests for the new bench_caf Phase 3 surface. Distributed tests were run in loopback mode only.
```

- [ ] **Step 5: Commit Task 7**

```bash
git add apps/bench_caf/README.md docs/superpowers/specs/2026-06-26-caf-performance-benchmark-design.md
git commit -m "docs(bench-caf): document phase three benchmark scenarios"
```

---

## Final Branch Verification (Phase 3)

After all Phase 3 tasks are complete:

```bash
git log --oneline --decorate -10
ninja -C build 18_bench_caf test_unit_apps test_bench_caf_actor_creation test_bench_caf_mailbox_n1 test_bench_caf_mixed_case test_bench_caf_distribution test_bench_caf_mandelbrot test_bench_caf_scheduling_mix test_bench_caf_distributed_ping test_bench_caf_smoke test_bench_caf_distributed_smoke
ctest -R "bench_caf" --output-on-failure --parallel 4
git diff --check
```

Expected:
- All bench_caf-related targets build.
- All bench_caf tests pass via CTest.
- `git diff --check` exits 0.
- `18_bench_caf` supports all 15 scenario names (3 Phase 1 + 7 Phase 2 + 6 Phase 3 = 16, minus 1 overlap).
- Output schema contains Phase 3 fields (`delivery_path`, microbenchmark metrics, distributed counters) only when relevant.

## Phase 3 Completion Criteria

The CAF benchmark port is complete when:

1. `18_bench_caf` supports all scenarios in the design spec (Sections 8.1–8.3).
2. Every scenario has a smoke preset that completes in seconds for CI.
3. JSON and CSV output schemas are stable across all three phases.
4. Message-size, message-shape, and traffic-distribution sweeps work under nightly preset.
5. Distributed ping/pong smoke runs in loopback mode without external processes.
6. Microbenchmarks report ops/sec, alloc counts, and active bytes delta.
7. Mandelbrot produces no file output during timed execution.
8. Scheduling-mix runs heterogeneous workloads concurrently.
9. All 16 scenario names are parseable and listed in `--scenario` error output.
10. Acceptance criteria from the design spec (Section 14) remain satisfied.
