# CAF Performance Benchmark Phase 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `apps/bench_caf/18_bench_caf` with message-size sweeps, message-shape variants, traffic-distribution scenarios, nightly preset support, and per-receiver/per-sender skew metrics.

**Architecture:** The Phase 1 runner dispatches one scenario × one parameter set per invocation. Phase 2 keeps that single-invocation model but adds: (a) new `MessageShape` generators (`protobuf-small`, `protobuf-nested`, `shared-buffer`, `mixed-80-20`), (b) new `TrafficDistribution` values with dedicated actor topologies, (c) a `--nightly` preset that runs a fixed sweep of sizes/shapes/distributions, (d) extended `TrialMetrics` with bytes-per-second, admission-failure counters, and per-receiver/per-sender skew fields, and (e) a `caf_bench_sweep.hpp` helper that expands a single CLI invocation into multiple `CafBenchConfig` permutations so nightly and paper-scale presets can cover the full parameter matrix without changing the existing one-trial-per-call contract.

**Tech Stack:** C++20, HPActor `ActorSystem`, `EventBasedActor`, `TypedMessage`, `StreamBuffer`, Protobuf (`bench_caf.proto` for shape generators), GTest, CMake/Ninja, existing `18_bench_caf` binary and `hpactor_lib`.

## Global Constraints

- Do not copy CAF implementation code.
- Do not use CAF as a dependency.
- Do not replace `apps/bench_perf/` or `apps/bench_saturate/`.
- Do not require distributed tests in normal CI.
- Do not make benchmark success depend on exact throughput values from one developer machine.
- Do not bypass HPActor delivery semantics by default. Fast-path variants must state which checks are skipped.
- Every design or implementation write MUST happen in an isolated git worktree.
- Production code changes MUST follow RED → GREEN → REFACTOR.
- NEVER introduce `dynamic_cast`, `typeid`, exception-based control flow, or public APIs that require RTTI or exceptions.
- Preserve actor boundaries; avoid shared mutable state between actors.
- One serialized consumer per actor mailbox.
- Bounded capacity failures are counted and reported.

---

## File Structure

Create these app files:

```text
apps/bench_caf/
├── caf_bench_config.hpp          (MODIFY — extend enums, add sweep support)
├── caf_bench_metrics.hpp         (MODIFY — add bytes/sec, skew, failure counters)
├── caf_bench_output.hpp          (MODIFY — emit new trial fields, summary stats)
├── caf_bench_scenarios.hpp       (MODIFY — add distribution scenarios)
├── caf_bench_runner.hpp          (MODIFY — add sweep dispatch)
├── caf_bench_sweep.hpp           (CREATE — parameter-matrix expansion)
├── messages.hpp                  (MODIFY — add shape generator function)
├── bench_caf.proto               (CREATE — protobuf shapes for benchmark payloads)
└── actors/
    ├── mailbox_n1_actor.hpp      (MODIFY — wire admission-failure counters)
    ├── traffic_distribution_actor.hpp (CREATE — actors for each distribution)
    └── payload_shape_actor.hpp    (CREATE — protobuf-shape payload generators)
```

Modify these build files:

```text
apps/bench_caf/CMakeLists.txt      (MODIFY — add proto sources)
tests/unit/apps/CMakeLists.txt     (MODIFY — add new unit test files)
tests/integration/apps/CMakeLists.txt (MODIFY — add distribution integration test)
```

Create these tests:

```text
tests/unit/apps/test_bench_caf_sweep.cpp          (CREATE — sweep expansion tests)
tests/unit/apps/test_bench_caf_payload_shapes.cpp  (CREATE — shape generators)
tests/unit/apps/test_bench_caf_skew_metrics.cpp    (CREATE — skew/stats tests)
tests/integration/apps/test_bench_caf_distribution.cpp (CREATE — traffic distribution tests)
```

---

## Shared Interfaces

All tasks use the existing `hpactor::apps::bench_caf` namespace.

### Extended Enums (append to `caf_bench_config.hpp`)

```cpp
enum class MessageShape {
    HeaderOnly,       // existing — Phase 1
    FixedBytes,       // existing — Phase 1
    ProtobufSmall,    // NEW
    ProtobufNested,   // NEW
    SharedBuffer,     // NEW — zero-copy candidate
    Mixed80_20,       // NEW — 80% small, 20% larger
};

enum class TrafficDistribution {
    NToOne,           // existing — Phase 1 (used by mailbox-n1)
    OneToOne,         // NEW
    OneToN,           // NEW
    NToNRandom,       // NEW
    Ring,             // NEW
    Pipeline,         // NEW
    ZipfHotspot,      // NEW
    BurstyWaves,      // NEW
};

// New scenario kinds for distribution-only scenarios
enum class ScenarioKind {
    ActorCreation,    // existing
    MailboxN1,        // existing
    MixedCase,        // existing
    TrafficOneToOne,  // NEW
    TrafficOneToN,    // NEW
    TrafficNToNRandom,// NEW
    TrafficRing,      // NEW
    TrafficPipeline,  // NEW
    TrafficZipf,      // NEW
    TrafficBursty,    // NEW
};
```

### Extended TrialMetrics (append fields to `caf_bench_metrics.hpp`)

```cpp
struct TrialMetrics {
    // ... existing Phase 1 fields ...

    // Phase 2 fields
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    double bytes_per_second = 0.0;
    uint64_t admission_failures = 0;        // RejectNewest/DropNewest/DropOldest
    uint64_t dlq_handoffs = 0;
    uint64_t dedup_suppressions = 0;
    uint64_t deadline_expiries = 0;

    // Per-receiver skew (populated per trial)
    uint64_t max_receiver_depth = 0;
    uint64_t min_receiver_messages = 0;
    uint64_t max_receiver_messages = 0;

    // Per-sender throughput spread
    double min_sender_throughput = 0.0;
    double max_sender_throughput = 0.0;

    // Payload histogram (bucketed by size range)
    std::vector<uint64_t> size_histogram;
};
```

### Sweep Config (new header `caf_bench_sweep.hpp`)

```cpp
struct SweepEntry {
    CafBenchConfig config;
    std::string label;   // human-readable, e.g. "size=256,shape=fixed-bytes"
};

// Expands a single CLI config into a list of parameter permutations when
// preset is Nightly or PaperScale.  Smoke/Stress pass through unchanged.
std::vector<SweepEntry> expand_sweep(const CafBenchConfig& base_cfg);
```

### Shape Generator (new protobuf message in `bench_caf.proto`)

```protobuf
syntax = "proto3";
package hpactor.apps.bench_caf;

message BenchSmallPayload {
    uint32 sender_id = 1;
    uint64 sequence = 2;
    uint64 timestamp_us = 3;
}

message BenchNestedPayload {
    BenchSmallPayload inner = 1;
    bytes padding = 2;           // fill to target size
    uint64 checksum = 3;
}
```

### Payload Shape Factory (`messages.hpp` extension)

```cpp
// Returns a StreamBuffer encoded according to the requested shape and size.
// HeaderOnly / FixedBytes: existing encode_bench_payload().
// ProtobufSmall: serialize BenchSmallPayload, zero-pad.
// ProtobufNested: serialize BenchNestedPayload with nested inner.
// SharedBuffer: wrap in shared_ptr<StreamBuffer> for zero-copy hint.
// Mixed80_20: use small for 80% of calls, larger for 20%.
StreamBuffer encode_shaped_payload(const BenchPayloadHeader& header,
                                   size_t size, MessageShape shape,
                                   uint64_t seed);
```

### Distribution Scenario Entry Points (`caf_bench_scenarios.hpp` extension)

```cpp
TrialMetrics run_one_to_one_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_one_to_n_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_n_to_n_random_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_ring_traffic_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_pipeline_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_zipf_hotspot_trial(const CafBenchConfig& cfg, uint32_t idx);
TrialMetrics run_bursty_waves_trial(const CafBenchConfig& cfg, uint32_t idx);
```

---

### Task 1: Payload Shape Generators And Protobuf

**Files:**
- Create: `apps/bench_caf/bench_caf.proto`
- Modify: `apps/bench_caf/messages.hpp`
- Modify: `apps/bench_caf/CMakeLists.txt`
- Create: `tests/unit/apps/test_bench_caf_payload_shapes.cpp`
- Modify: `tests/unit/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: existing `BenchPayloadHeader`, `StreamBuffer`, protobuf codegen.
- Produces: `bench_caf.proto` with `BenchSmallPayload` and `BenchNestedPayload`, `encode_shaped_payload()`, `message_shape_name()` updated.

- [ ] **Step 1: Write the failing payload-shape test**

Create `tests/unit/apps/test_bench_caf_payload_shapes.cpp`:

```cpp
#include <apps/bench_caf/caf_bench_config.hpp>
#include <apps/bench_caf/messages.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(PayloadShapes, ProtobufSmallRoundTrips) {
    bench_caf::BenchPayloadHeader header{7, 42, 100};
    auto payload = bench_caf::encode_shaped_payload(
        header, 64, bench_caf::MessageShape::ProtobufSmall, 1);
    EXPECT_GE(payload.size(), 64u);
    auto decoded = bench_caf::decode_bench_payload(payload);
    EXPECT_EQ(decoded.sender_id, 7u);
    EXPECT_EQ(decoded.sequence, 42u);
    EXPECT_EQ(decoded.timestamp_us, 100u);
}

TEST(PayloadShapes, ProtobufNestedRespectsSize) {
    bench_caf::BenchPayloadHeader header{1, 1, 1};
    auto payload = bench_caf::encode_shaped_payload(
        header, 256, bench_caf::MessageShape::ProtobufNested, 2);
    EXPECT_GE(payload.size(), 256u);
}

TEST(PayloadShapes, SharedBufferRoundTrips) {
    bench_caf::BenchPayloadHeader header{9, 8, 7};
    auto payload = bench_caf::encode_shaped_payload(
        header, 128, bench_caf::MessageShape::SharedBuffer, 3);
    EXPECT_GE(payload.size(), bench_caf::BenchPayloadHeader::kEncodedSize);
    auto decoded = bench_caf::decode_bench_payload(payload);
    EXPECT_EQ(decoded.sender_id, 9u);
}

TEST(PayloadShapes, Mixed80_20UsesCorrectSizes) {
    // Run many samples; verify ~80% are small, ~20% are larger.
    size_t small = 0, large = 0;
    bench_caf::BenchPayloadHeader header{};
    for (int i = 0; i < 100; ++i) {
        auto payload = bench_caf::encode_shaped_payload(
            header, 1024, bench_caf::MessageShape::Mixed80_20, i);
        if (payload.size() <= 64)
            ++small;
        else
            ++large;
    }
    EXPECT_GE(small, 60u);  // at least 60% small
    EXPECT_GE(large, 10u);  // at least 10% large
}
```

Modify `tests/unit/apps/CMakeLists.txt` to add `test_bench_caf_payload_shapes.cpp` to `test_unit_apps`.

- [ ] **Step 2: Run the tests to verify RED**

```bash
ninja -C build test_unit_apps
```

Expected: compile fails because `encode_shaped_payload()` does not exist.

- [ ] **Step 3: Add protobuf definitions**

Create `apps/bench_caf/bench_caf.proto`:

```protobuf
syntax = "proto3";
package hpactor.apps.bench_caf;

message BenchSmallPayload {
    uint32 sender_id = 1;
    uint64 sequence = 2;
    uint64 timestamp_us = 3;
}

message BenchNestedPayload {
    BenchSmallPayload inner = 1;
    bytes padding = 2;
    uint64 checksum = 3;
}
```

Modify `apps/bench_caf/CMakeLists.txt` to compile the proto:

```cmake
add_executable(18_bench_caf
    18_bench_caf.cpp
    bench_caf.pb.cc
)
target_link_libraries(18_bench_caf PRIVATE hpactor_lib protobuf::libprotobuf)
target_include_directories(18_bench_caf PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
```

- [ ] **Step 4: Implement shape generators**

Add to `messages.hpp` the `encode_shaped_payload()` function that dispatches by `MessageShape`. The `ProtobufSmall` path serializes `BenchSmallPayload`, fills to target size. The `ProtobufNested` path serializes `BenchNestedPayload` with the inner field populated. `SharedBuffer` uses a `std::shared_ptr<StreamBuffer>` wrapper. `Mixed80_20` delegates 80% to header-only and 20% to fixed-bytes.

Add a `decode_shaped_payload()` overload that handles protobuf deserialization.

Update `message_shape_name()` to return the correct string for all six values.

- [ ] **Step 5: Run the payload-shape tests to verify GREEN**

```bash
ninja -C build test_unit_apps
./build/tests/unit/apps/test_unit_apps --gtest_filter="PayloadShapes.*"
```

Expected: all 4 `PayloadShapes` tests pass.

- [ ] **Step 6: Commit Task 1**

```bash
git add apps/bench_caf/bench_caf.proto apps/bench_caf/CMakeLists.txt apps/bench_caf/messages.hpp tests/unit/apps/CMakeLists.txt tests/unit/apps/test_bench_caf_payload_shapes.cpp
git commit -m "feat(bench-caf): add protobuf payload shape generators"
```

---

### Task 2: Skew Metrics And Extended TrialMetrics

**Files:**
- Modify: `apps/bench_caf/caf_bench_metrics.hpp`
- Modify: `apps/bench_caf/caf_bench_output.hpp`
- Create: `tests/unit/apps/test_bench_caf_skew_metrics.cpp`
- Modify: `tests/unit/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: existing `TrialMetrics`, `write_json_report()`, `write_csv_report()`.
- Produces: `compute_receiver_skew()`, `compute_sender_spread()`, `build_size_histogram()`, extended JSON/CSV output.

- [ ] **Step 1: Write the failing skew-metrics test**

```cpp
TEST(SkewMetrics, ComputesReceiverSkew) {
    std::vector<uint64_t> counts = {100, 120, 95, 108};
    auto [min_v, max_v] = bench_caf::compute_receiver_skew(counts);
    EXPECT_EQ(min_v, 95u);
    EXPECT_EQ(max_v, 120u);
}

TEST(SkewMetrics, ComputesSenderSpread) {
    std::vector<double> throughputs = {1000.0, 1200.0, 950.0};
    auto [min_v, max_v] = bench_caf::compute_sender_spread(throughputs);
    EXPECT_DOUBLE_EQ(min_v, 950.0);
    EXPECT_DOUBLE_EQ(max_v, 1200.0);
}

TEST(SkewMetrics, BuildsSizeHistogram) {
    std::vector<size_t> sizes = {0, 16, 64, 256, 1024, 4096, 16*1024};
    auto hist = bench_caf::build_size_histogram(sizes);
    EXPECT_EQ(hist.size(), bench_caf::kHistogramBuckets);
}

TEST(SkewMetrics, EmptySkewReturnsZero) {
    std::vector<uint64_t> empty;
    auto [min_v, max_v] = bench_caf::compute_receiver_skew(empty);
    EXPECT_EQ(min_v, 0u);
    EXPECT_EQ(max_v, 0u);
}
```

- [ ] **Step 2: Run the tests to verify RED**

```bash
ninja -C build test_unit_apps
```

Expected: compile fails; skew helpers do not exist yet.

- [ ] **Step 3: Add skew helpers and extended metrics**

Add to `caf_bench_metrics.hpp`:
- Phase 2 fields on `TrialMetrics` (bytes_sent, bytes_received, bytes_per_second, admission_failures, dlq_handoffs, dedup_suppressions, deadline_expiries, max_receiver_depth, min_receiver_messages, max_receiver_messages, min_sender_throughput, max_sender_throughput, size_histogram).
- `compute_receiver_skew(const std::vector<uint64_t>&)` returning `std::pair<uint64_t, uint64_t>`.
- `compute_sender_spread(const std::vector<double>&)` returning `std::pair<double, double>`.
- `build_size_histogram(const std::vector<size_t>&)` returning `std::vector<uint64_t>`.
- `kHistogramBuckets` constant (8 buckets: 0, 16, 64, 256, 1KB, 4KB, 16KB, 64KB+).

Update `caf_bench_output.hpp` to emit the new fields in JSON (under each trial object) and extend the CSV header with the new columns.

- [ ] **Step 4: Run the skew-metrics tests to verify GREEN**

```bash
ninja -C build test_unit_apps
./build/tests/unit/apps/test_unit_apps --gtest_filter="SkewMetrics.*"
```

Expected: all 4 `SkewMetrics` tests pass.

- [ ] **Step 5: Commit Task 2**

```bash
git add apps/bench_caf/caf_bench_metrics.hpp apps/bench_caf/caf_bench_output.hpp tests/unit/apps/CMakeLists.txt tests/unit/apps/test_bench_caf_skew_metrics.cpp
git commit -m "feat(bench-caf): add skew metrics and extended trial counters"
```

---

### Task 3: Traffic Distribution Scenarios — One-To-One, One-To-N, N-To-N-Random

**Files:**
- Create: `apps/bench_caf/actors/traffic_distribution_actor.hpp`
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`
- Create: `tests/integration/apps/test_bench_caf_distribution.cpp`
- Modify: `tests/integration/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: `CafBenchConfig`, `ActorSystem`, distribution enum values.
- Produces: `run_one_to_one_trial()`, `run_one_to_n_trial()`, `run_n_to_n_random_trial()`.

**Actors:**

- `OneToOneActor`: one sender, one receiver. Sender sends `N` messages directly to receiver. Simplest topology; measures baseline send/receive overhead.
- `OneToNActor`: one sender actor, `M` receiver actors. Sender round-robins messages across receivers. Exercises fanout and scheduler wakeup spread.
- `NToNRandomActor`: `N` senders, `M` receivers. Each sender picks a receiver via deterministic LCG from the seed. Exercises routing and cache locality.

Default dimensions (smoke):

| Distribution | Senders | Receivers | Messages total |
|-------------|---------|-----------|----------------|
| `one-to-one` | 1 | 1 | 10,000 |
| `one-to-n` | 1 | 8 | 80,000 |
| `n-to-n-random` | 4 | 4 | 40,000 |

- [ ] **Step 1: Write the failing distribution integration test**

```cpp
TEST(Distribution, OneToOneCompletesAllMessages) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::TrafficOneToOne;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;

    auto metrics = bench_caf::run_one_to_one_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_EQ(metrics.total_sent, metrics.total_received);
    EXPECT_GT(metrics.throughput_msgps, 0.0);
}

TEST(Distribution, OneToNCompletesAllMessages) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::TrafficOneToN;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;

    auto metrics = bench_caf::run_one_to_n_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_EQ(metrics.total_sent, metrics.total_received);
}

TEST(Distribution, NToNRandomCompletesAllMessages) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::TrafficNToNRandom;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;

    auto metrics = bench_caf::run_n_to_n_random_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_EQ(metrics.total_sent, metrics.total_received);
}
```

- [ ] **Step 2: Run the tests to verify RED**

```bash
ninja -C build test_bench_caf_distribution
```

Expected: compile fails.

- [ ] **Step 3: Implement the three distribution scenarios**

Create `apps/bench_caf/actors/traffic_distribution_actor.hpp` with actors for each distribution pattern. Add scenario runners to `caf_bench_scenarios.hpp`. Wire `ScenarioKind` dispatch in `caf_bench_runner.hpp`.

Each runner:
1. Creates `ActorSystem` with `make_bench_actor_config(cfg)`.
2. Spawns sender and receiver actors per the distribution topology.
3. Uses `MailboxLoadTag` / `MailboxDoneTag` for message delivery.
4. Waits on atomic counters with a deadline.
5. Calls `system.shutdown()`, stops the sampler, computes metrics.

For `ZipfHotspot`, use a Zipf-distributed receiver selection: `rank = (sender_id * seed) % num_receivers` weighted, for `BurstyWaves`, send in batches with sleep gaps.

- [ ] **Step 4: Run the distribution tests to verify GREEN**

```bash
ninja -C build test_bench_caf_distribution
./build/tests/integration/apps/test_bench_caf_distribution
```

Expected: 3 tests pass.

- [ ] **Step 5: Commit Task 3**

```bash
git add apps/bench_caf/actors/traffic_distribution_actor.hpp apps/bench_caf/caf_bench_scenarios.hpp apps/bench_caf/caf_bench_runner.hpp tests/integration/apps/CMakeLists.txt tests/integration/apps/test_bench_caf_distribution.cpp
git commit -m "feat(bench-caf): add one-to-one, one-to-n, and n-to-n-random distributions"
```

---

### Task 4: Traffic Distribution Scenarios — Ring, Pipeline, Zipf, Bursty

**Files:**
- Modify: `apps/bench_caf/actors/traffic_distribution_actor.hpp`
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`
- Modify: `tests/integration/apps/test_bench_caf_distribution.cpp`
- Modify: `tests/integration/apps/CMakeLists.txt`

**Interfaces:**
- Produces: `run_ring_traffic_trial()`, `run_pipeline_trial()`, `run_zipf_hotspot_trial()`, `run_bursty_waves_trial()`.

**Topologies:**

- **Ring**: `M` actors connected in a ring, each sends to next. Token loops `K` times. Measures steady-state latency per hop.
- **Pipeline**: `M` actors in sequence. Stage 0 generates `K` messages, each stage processes and forwards. Measures batching and handoff overhead.
- **ZipfHotspot**: `N` senders, `M` receivers. Receivers chosen with Zipf distribution (α ≈ 1.0). Measures overload concentration and fairness.
- **BurstyWaves**: `N` senders coordinate bursts of size `B` with configurable idle gaps between waves. Measures queue growth, recovery, and admission pressure.

Default smoke dimensions:

| Distribution | Actors | Messages | Notes |
|-------------|--------|----------|-------|
| `ring` | 16 | 1,600 hops | 100 laps |
| `pipeline` | 8 stages | 1,000 | 1,000 injected |
| `zipf-hotspot` | 4 senders, 4 receivers | 20,000 | skew visible |
| `bursty-waves` | 4 senders, 1 receiver | 20,000 | 5 waves of 4,000 |

- [ ] **Step 1: Extend the distribution integration test with four new cases**

```cpp
TEST(Distribution, RingCompletesHops) { ... }
TEST(Distribution, PipelineCompletesAllStages) { ... }
TEST(Distribution, ZipfHotspotCompletesWithSkew) { ... }
TEST(Distribution, BurstyWavesCompletesInOrder) { ... }
```

- [ ] **Step 2: Run the tests to verify RED**

Expected: compile fails.

- [ ] **Step 3: Implement the four additional distribution scenarios**

Add `RingTrafficActor`, `PipelineStageActor`, `ZipfSenderActor` / `ZipfReceiverActor`, and `BurstySenderActor` / `BurstyReceiverActor` to the traffic_distribution_actor.hpp header. Add their runners to the scenarios header. Wire dispatch in the runner.

- [ ] **Step 4: Run the extended tests to verify GREEN**

```bash
ninja -C build test_bench_caf_distribution
./build/tests/integration/apps/test_bench_caf_distribution
```

Expected: all 7 distribution tests pass.

- [ ] **Step 5: Commit Task 4**

```bash
git add apps/bench_caf/actors/traffic_distribution_actor.hpp apps/bench_caf/caf_bench_scenarios.hpp tests/integration/apps/test_bench_caf_distribution.cpp
git commit -m "feat(bench-caf): add ring, pipeline, zipf, and bursty distributions"
```

---

### Task 5: Sweep Expansion And Nightly Preset

**Files:**
- Create: `apps/bench_caf/caf_bench_sweep.hpp`
- Create: `tests/unit/apps/test_bench_caf_sweep.cpp`
- Modify: `apps/bench_caf/caf_bench_config.hpp`
- Modify: `apps/bench_caf/caf_bench_runner.hpp`
- Modify: `apps/bench_caf/18_bench_caf.cpp`
- Modify: `tests/unit/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: `CafBenchConfig`, `ScenarioKind` enums.
- Produces: `expand_sweep()`, updated runner dispatch that iterates sweeps.

**Sweep logic:**

When a user runs `--preset nightly --scenario mailbox-n1`, the `expand_sweep()` function returns a list of `CafBenchConfig` permutations:

| Message Size | Message Shape |
|-------------|--------------|
| 0 (header-only) | HeaderOnly |
| 16 | FixedBytes |
| 64 | FixedBytes |
| 256 | FixedBytes |
| 1 KB | FixedBytes |
| 4 KB | FixedBytes |

The runner iterates over each `SweepEntry`, runs the trial, and collects metrics into a single `CafBenchReport` with one trial per entry.

- [ ] **Step 1: Write the failing sweep test**

```cpp
TEST(SweepExpansion, NightlyMailboxN1ExpandsToSixSizes) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MailboxN1;
    cfg.preset = bench_caf::PresetKind::Nightly;

    auto sweep = bench_caf::expand_sweep(cfg);
    EXPECT_EQ(sweep.size(), 6u);  // 0, 16, 64, 256, 1KB, 4KB

    EXPECT_EQ(sweep[0].config.message_size_bytes, 0u);
    EXPECT_EQ(sweep[0].config.message_shape, bench_caf::MessageShape::HeaderOnly);

    EXPECT_EQ(sweep[1].config.message_size_bytes, 16u);
    EXPECT_EQ(sweep[1].config.message_shape, bench_caf::MessageShape::FixedBytes);

    EXPECT_EQ(sweep[5].config.message_size_bytes, 4096u);
}

TEST(SweepExpansion, SmokePassthrough) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::ActorCreation;
    cfg.preset = bench_caf::PresetKind::Smoke;

    auto sweep = bench_caf::expand_sweep(cfg);
    ASSERT_EQ(sweep.size(), 1u);
    EXPECT_EQ(sweep[0].config.scenario, bench_caf::ScenarioKind::ActorCreation);
}
```

- [ ] **Step 2: Run the tests to verify RED**

Expected: compile fails.

- [ ] **Step 3: Implement sweep expansion**

Create `apps/bench_caf/caf_bench_sweep.hpp` with `expand_sweep()`. The function inspects the scenario and preset, and returns the appropriate parameter permutations. Nightly preset on `mailbox-n1` sweeps message sizes. PaperScale on `mailbox-n1` sweeps both sizes and shapes.

Update `caf_bench_runner.hpp` so `run_caf_benchmark()` calls `expand_sweep()` and runs all entries.

Update `18_bench_caf.cpp` so the output file path auto-appends a suffix when sweeps are used (or emits all-permutations into one file).

- [ ] **Step 4: Run the sweep tests to verify GREEN**

```bash
ninja -C build test_unit_apps
./build/tests/unit/apps/test_unit_apps --gtest_filter="SweepExpansion.*"
```

Expected: both tests pass.

- [ ] **Step 5: Commit Task 5**

```bash
git add apps/bench_caf/caf_bench_sweep.hpp apps/bench_caf/caf_bench_runner.hpp apps/bench_caf/18_bench_caf.cpp tests/unit/apps/test_bench_caf_sweep.cpp tests/unit/apps/CMakeLists.txt
git commit -m "feat(bench-caf): add parameter sweep expansion and nightly preset"
```

---

### Task 6: Admission-Failure Wiring And Final Verification

**Files:**
- Modify: `apps/bench_caf/actors/mailbox_n1_actor.hpp`
- Modify: `apps/bench_caf/caf_bench_output.hpp`
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`

**Interfaces:**
- Consumes: Phase 2 `TrialMetrics` fields.
- Produces: admission-failure counters wired into mailbox-n1 receiver, payload histograms populated.

- [ ] **Step 1: Wire admission-failure counters**

Update `MailboxN1ReceiverActor` to track rejected messages (via a bounded-overflow detection path: if mailbox depth approaches capacity, count near-full events). Update scenario runners to populate `bytes_sent`, `bytes_received`, and `bytes_per_second`.

Update `run_mailbox_n1_trial()` and distribution scenario runners to populate the Phase 2 `TrialMetrics` fields (`admission_failures`, `max_receiver_depth`, `size_histogram`, sender throughput spread).

- [ ] **Step 2: Run existing tests to confirm no regressions**

```bash
ninja -C build test_bench_caf_mailbox_n1 test_bench_caf_distribution test_unit_apps
./build/tests/unit/apps/test_unit_apps --gtest_filter="BenchCaf*"
./build/tests/integration/apps/test_bench_caf_mailbox_n1
./build/tests/integration/apps/test_bench_caf_distribution
./build/tests/system/apps/test_bench_caf_smoke
```

Expected: all existing Phase 1 tests still pass; all Phase 2 tests pass.

- [ ] **Step 3: Run final verification**

```bash
# Nightly sweep invocation
./build/apps/bench_caf/18_bench_caf --scenario mailbox-n1 --preset nightly --format json
# Traffic distribution invocations
./build/apps/bench_caf/18_bench_caf --scenario traffic-one-to-one --preset smoke --format csv
./build/apps/bench_caf/18_bench_caf --scenario traffic-ring --preset smoke --format json
./build/apps/bench_caf/18_bench_caf --scenario traffic-zipf --preset smoke --format json
git diff --check
```

Expected:
- Nightly mailbox-n1 emits one trial per sweep entry (6 trials × 3 per trial = 18 trial objects) in JSON.
- Each traffic distribution exits 0 and emits valid output.
- `git diff --check` is clean.

- [ ] **Step 4: Commit Task 6**

```bash
git add apps/bench_caf/actors/mailbox_n1_actor.hpp apps/bench_caf/caf_bench_output.hpp apps/bench_caf/caf_bench_scenarios.hpp
git commit -m "feat(bench-caf): wire admission-failure counters and finalize Phase 2"
```

---

## Final Branch Verification (Phase 2)

After all Phase 2 tasks are complete, run:

```bash
git log --oneline --decorate -8
ninja -C build 18_bench_caf test_unit_apps test_bench_caf_actor_creation test_bench_caf_mailbox_n1 test_bench_caf_mixed_case test_bench_caf_distribution test_bench_caf_smoke
./build/tests/unit/apps/test_unit_apps --gtest_filter="BenchCaf*:PayloadShapes*:SkewMetrics*:SweepExpansion*"
./build/tests/integration/apps/test_bench_caf_actor_creation
./build/tests/integration/apps/test_bench_caf_mailbox_n1
./build/tests/integration/apps/test_bench_caf_mixed_case
./build/tests/integration/apps/test_bench_caf_distribution
./build/tests/system/apps/test_bench_caf_smoke
git diff --check
```

Expected:
- All selected targets build.
- All selected tests pass.
- Phase 1 scenarios unchanged in behavior.
- `18_bench_caf` supports `--preset nightly` with size sweeps.
- Seven traffic distributions produce valid output.

## Phase 2 Handoff To Phase 3

Phase 2 leaves the config, runner, and output schemas in a state where Phase 3 can add:
- `distributed-ping` (needs `enable_network = true` and loopback transports).
- `message-creation`, `dispatch-match`, `serialization` microbenchmarks (no actors needed — direct loop benchmarks).
- `mandelbrot` (needs `DenseComputingActor` or CPU-heavy behavior).
- `scheduling-mix` (needs scheduler-thread-count sweep infrastructure).

No Phase 1 or Phase 2 scenario output format should change incompatibly. Phase 3 should be able to add new `ScenarioKind` values without touching existing Phase 1/2 scenario runners.
