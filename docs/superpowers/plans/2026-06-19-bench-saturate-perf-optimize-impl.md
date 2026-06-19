# Bench Saturate Performance Optimization — Implementation Plan

**Date:** 2026-06-19
**Design Spec:** `docs/superpowers/specs/2026-06-19-bench-saturate-perf-optimize-design.md`
**Issue:** #337

## Phase Overview

| Phase | Name | Tests | Files |
|-------|------|-------|-------|
| 1 | StreamBuffer `from_data()` factory | 3 | 2 |
| 2 | Bench messages use exact-capacity buffers | 0 | 1 |
| 3 | Collector lock-free ring buffer | 4 | 1 |
| 4 | Latency reporting downsampling | 0 | 1 |
| 5 | Fast local delivery path | 5 | 2 |
| 6 | Cooperative sender loop | 0 | 1 |
| 7 | End-to-end validation | — | — |

All phases follow RED → GREEN → REFACTOR (TDDFlow).

---

## Phase 1: StreamBuffer `from_data()` Factory

**Goal:** Add a static factory that creates a StreamBuffer with capacity exactly
matching the provided data, avoiding the 64KB default minimum.

### RED — Write failing tests

File: `tests/unit/adt/test_stream_buffer.cpp` (extend existing or create new)

```cpp
TEST(StreamBufferFromData, ExactCapacityAllocation) {
    uint8_t data[32];
    for (size_t i = 0; i < 32; ++i) data[i] = static_cast<uint8_t>(i);
    auto sb = StreamBuffer::from_data(data, 32);
    EXPECT_EQ(sb.size(), 32u);
    EXPECT_LE(sb.capacity(), 64u);  // No 64KB minimum
    EXPECT_GE(sb.capacity(), 32u);  // At least enough for data
    for (size_t i = 0; i < 32; ++i) EXPECT_EQ(sb[i], data[i]);
}

TEST(StreamBufferFromData, EmptyData) {
    auto sb = StreamBuffer::from_data(nullptr, 0);
    EXPECT_TRUE(sb.empty());
    EXPECT_EQ(sb.size(), 0u);
}

TEST(StreamBufferFromData, LargeDataUsesNormalCapacity) {
    std::vector<uint8_t> data(100000, 0x42);
    auto sb = StreamBuffer::from_data(data.data(), data.size());
    EXPECT_EQ(sb.size(), 100000u);
    EXPECT_GE(sb.capacity(), 100000u);
}
```

### GREEN — Implement

1. Add declaration in `include/hpactor/adt/stream_buffer.hpp`:
   ```cpp
   /// \brief Create a buffer from raw data with exact-fit capacity.
   static StreamBuffer from_data(const uint8_t* data, size_t len);
   ```

2. Add implementation in `src/adt/stream_buffer.cpp`:
   ```cpp
   StreamBuffer StreamBuffer::from_data(const uint8_t* data, size_t len) {
       StreamBuffer sb = with_capacity(len);
       if (len > 0) sb.append(data, len);
       return sb;
   }
   ```

### REFACTOR — Verify clean

- Run all StreamBuffer tests to confirm no regression
- Verify `ensure_capacity` path isn't triggered for small buffers

### Verification:
```bash
ninja -C build test_unit_adt
./build/tests/unit/adt/test_unit_adt --gtest_filter="*StreamBufferFromData*"
```

---

## Phase 2: Bench Messages Use Exact-Capacity Buffers

**Goal:** Replace all iterator-pair `StreamBuffer` constructors in bench message
encoders with `StreamBuffer::from_data()`.

### Changes to `apps/bench_saturate/messages.hpp`

Replace every pattern:
```cpp
return StreamBuffer(buf, buf + sizeof(buf));
```
with:
```cpp
return StreamBuffer::from_data(buf, sizeof(buf));
```

Affected functions:
- `SaturateStartPayload::encode()` — 40 bytes
- `RateChangePayload::encode()` — 12 bytes
- `LoadMessagePayload::encode_header()` — 20 bytes
- `LoadMessagePayload::encode_with_junk()` — variable (keep as-is; already sized)
- `LatencySamplePayload::encode()` — 16 bytes
- `DropReportPayload::encode()` — 24 bytes
- `ThroughputSamplePayload::encode()` — 20 bytes

### Verification:
```bash
ninja -C build 17_bench_saturate
./build/apps/bench_saturate/17_bench_saturate --headless quick-saturate --format json
```

---

## Phase 3: Collector Lock-Free Ring Buffer

**Goal:** Replace `std::vector<double> latencies_` with a fixed-size array of
atomic doubles plus an atomic write index. Eliminates O(n) erase-from-front.

### RED — Write failing tests

Create `tests/unit/apps/test_saturate_collector.cpp` (or extend existing):

```cpp
TEST(SaturateCollector, RingBufferWrapsCorrectly) {
    // Verify that after 2× reservoir worth of writes, the oldest
    // half is overwritten
}

TEST(SaturateCollector, PercentileOnPartialFill) {
    // Verify percentile computation when fewer than capacity samples exist
}

TEST(SaturateCollector, PercentileOnFullRingBuffer) {
    // Verify P50/P99/P999 with exactly capacity samples
}

TEST(SaturateCollector, ConcurrentWriteReadSafety) {
    // Verify atomic writes don't tear and reads see consistent values
}
```

### GREEN — Implement

In `saturate_collector_actor.hpp`:

```cpp
// Replace:
// std::vector<double> latencies_;
// latencies_.reserve(kReservoirSize);
//
// With:
static constexpr size_t kReservoirSize = 10000;
std::array<std::atomic<double>, kReservoirSize> latencies_{};
std::atomic<size_t> latency_write_idx_{0};

// In handle_latency_sample():
void handle_latency_sample(TypedMessage& msg) {
    const auto& p = msg.payload();
    if (p.size() < 16) return;
    LatencySamplePayload sample = LatencySamplePayload::decode(p);
    size_t idx = latency_write_idx_.fetch_add(1, std::memory_order_relaxed) % kReservoirSize;
    latencies_[idx].store(static_cast<double>(sample.latency_us), std::memory_order_relaxed);
}

// In recompute_percentiles():
void recompute_percentiles() {
    size_t count = std::min(latency_write_idx_.load(std::memory_order_acquire), kReservoirSize);
    if (count > 0) {
        std::vector<double> sorted;
        sorted.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            sorted.push_back(latencies_[i].load(std::memory_order_relaxed));
        }
        std::sort(sorted.begin(), sorted.end());
        p50_us_ = sorted[count / 2];
        p99_us_ = sorted[count * 99 / 100];
        p999_us_ = sorted[count * 999 / 1000];
    }
    // ... remainder unchanged ...
}

// Remove latencies_.clear() from handle_start()
// Remove latencies_ reserve in constructor
```

### REFACTOR

- Ensure `handle_start()` resets `latency_write_idx_` to 0
- Verified no other code accesses `latencies_` directly

### Verification:
```bash
ninja -C build test_unit_apps
./build/tests/unit/apps/test_unit_apps --gtest_filter="*SaturateCollector*"
```

---

## Phase 4: Latency Reporting Downsampling

**Goal:** Report latency for 1% of messages instead of 100%.

### Change to `saturate_receiver_actor.hpp`

```cpp
void handle_load_message(TypedMessage& msg) {
    if (!running_) return;
    received_count_.fetch_add(1, std::memory_order_relaxed);
    uint64_t rcvd = received_count_.load(std::memory_order_relaxed);

    // Sample 1% of messages for latency (statistically sufficient at scale)
    if (rcvd % 100 == 0) {
        const auto& p = msg.payload();
        if (p.size() >= LoadMessagePayload::kHeaderSize) {
            auto decoded = LoadMessagePayload::decode(p);
            auto now = std::chrono::steady_clock::now();
            auto send_time = std::chrono::steady_clock::time_point(
                std::chrono::microseconds(decoded.send_timestamp_us));
            uint32_t latency_us = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - send_time)
                    .count());
            LatencySamplePayload sample;
            sample.sender_id = decoded.sender_id;
            sample.seq_no = decoded.seq_no;
            sample.latency_us = latency_us;
            context()->send(collector_addr_,
                            make_msg(LatencySampleTag, sample.encode()));
        }
    }

    // Drop report at 1/1000 as before
    if (rcvd % 1000 == 0) {
        send_drop_report();
    }
}
```

Key: check `rcvd % 100 == 0` BEFORE decoding and sending, so we skip the
decode + send for 99% of messages.

### Verification:
```bash
ninja -C build 17_bench_saturate
```

---

## Phase 5: Fast Local Delivery Path

**Goal:** Add `ActorSystem::try_deliver_local_fast()` that enqueues directly
to the target mailbox, bypassing the full DeliveryPipeline.

### RED — Write failing tests

Extend `tests/unit/core/test_actor_system.cpp` or `tests/integration/actor/test_delivery.cpp`:

```cpp
TEST(FastDelivery, EnqueuesToMailbox) {
    // Spawn an actor, send via fast path, verify mailbox non-empty
}

TEST(FastDelivery, ActorNotFoundReturnsRejected) {
    // Send to invalid ActorId, verify EnqueueResultCode::ActorNotFound
}

TEST(FastDelivery, MessageReachesActor) {
    // Send via fast path, verify actor processes it correctly
}

TEST(FastDelivery, FastPathMatchesNormalPath) {
    // Verify fast path delivers same messages as normal path for simple case
}

TEST(FastDelivery, SystemMessageLaneRouting) {
    // Verify system messages go to system lane via fast path
}
```

### GREEN — Implement

In `include/hpactor/core/actor_system.hpp`:
```cpp
/// \brief Fast local delivery that bypasses the full DeliveryPipeline.
///
/// Enqueues directly to the target mailbox without circuit breaker,
/// TTL, dedup, or backpressure checks. Suitable for internal benchmarks
/// and hot paths where those checks are known to be unnecessary.
///
/// \pre The target actor exists and has no circuit breaker configured.
/// \pre The message has no deadline requirement (best-effort).
/// \param[in] target Actor ID to deliver to.
/// \param[in] msg    Message to deliver (moved).
/// \return EnqueueResult describing acceptance or rejection.
mailbox::EnqueueResult try_deliver_local_fast(ActorId target, TypedMessage msg);
```

In `src/actor/actor_system.cpp`:
```cpp
mailbox::EnqueueResult
ActorSystem::try_deliver_local_fast(ActorId target, TypedMessage msg) {
    auto* mailbox = get_mailbox(target);
    if (!mailbox) {
        mailbox::EnqueueResult r;
        r.code = mailbox::EnqueueResultCode::ActorNotFound;
        r.target = target;
        return r;
    }
    mailbox::MailboxEnvelopeMeta meta;
    meta.sender = msg.sender_address();
    meta.type_tag = msg.type_id();
    meta.priority = 0;
    meta.deadline_ns = INT64_MAX;
    return mailbox->try_push(std::move(msg), meta);
}
```

### REFACTOR

- Verify `get_mailbox()` navigation is correct (it goes through `actor_directory_.find_mailbox()`)
- Check that `MailboxEnvelopeMeta` default fields are appropriate

### Verification:
```bash
ninja -C build test_unit_core test_integration_actor
ctest -R "FastDelivery" --output-on-failure
```

---

## Phase 6: Cooperative Sender Loop

**Goal:** Eliminate timer-driven self-scheduling in the sender. Instead, use
the scheduler's existing requeue mechanism for continuous operation with
natural interleaving.

### Changes to `saturate_sender_actor.hpp`

Replace `schedule_next()` / `SendTickTag` / timer-based flow with:

```cpp
void do_tick() {
    if (!running_ || receiver_addrs_.empty()) return;

    constexpr uint32_t kBatchSize = 10;
    auto now = std::chrono::steady_clock::now();
    uint64_t now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count());

    // Rate throttling: if we're ahead of target rate, yield
    uint64_t expected_elapsed_us = (sent_count_.load() * 1'000'000ULL) /
                                    current_rate_msgps_;
    uint64_t actual_elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now - start_time_).count());
    if (expected_elapsed_us > actual_elapsed_us + 500) {
        // We're ahead — schedule a short delay and return
        pending_tick_ = context()->schedule(
            std::chrono::microseconds(100), make_msg(SendTickTag));
        return;
    }

    for (uint32_t i = 0; i < kBatchSize; ++i) {
        // ... same message construction as before ...
        // Use fast delivery path instead of try_deliver_local
        auto result = home_system().try_deliver_local_fast(
            target.id, make_msg(LoadMessageTag, std::move(payload)));
        sent_count_.fetch_add(1, std::memory_order_relaxed);
        if (!result.accepted())
            send_dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // Throughput sampling (every 100 messages, as before)
    uint64_t sent = sent_count_.load(std::memory_order_relaxed);
    if (sent % 100 == 0) {
        // ... same ThroughputSample send as before ...
    }

    // Let the scheduler's kRequeueBudget mechanism handle continuous
    // operation. Returning from do_tick() lets the scheduler invoke us
    // again via RequeueReady (if mailbox still has messages and budget
    // not exhausted), avoiding a timer round-trip.
}
```

The key insight: by returning from the handler after each batch, the
`BehaviorActorRunner::run()` will check `!mailbox->empty()` and re-enqueue
the actor via `RequeueReady`, up to `kRequeueBudget=64` times. After that,
the actor is set to idle and the next enqueue triggers `notify_ready()` —
natural interleaving.

For `handle_start()`, remove the `SendTickTag` self-schedule. Instead,
make the first batch fire from a zero-delay self-schedule so the actor
gets its first activation:
```cpp
void handle_start(TypedMessage& msg) {
    // ... existing init code ...
    running_ = true;
    start_time_ = std::chrono::steady_clock::now();
    // Kick off the send loop via zero-delay self-message
    pending_tick_ = context()->schedule(
        std::chrono::microseconds(0), make_msg(SendTickTag));
}
```

The `SendTickTag` handler becomes:
```cpp
} else if (msg.type_id() == SendTickTag) {
    if (running_) do_tick();
}
```

And `do_tick()` schedules future ticks only when rate-throttling; otherwise
the scheduler's requeue budget handles continuous re-invocation.

The `handle_rate_change()` handler no longer needs `schedule_next()`:
```cpp
void handle_rate_change(TypedMessage& msg) {
    auto rc = RateChangePayload::decode(msg.payload());
    current_rate_msgps_ = rc.target_rate_msgps;
    payload_mode_ = rc.payload_mode;
    payload_size_min_ = rc.payload_size_min;
    payload_size_max_ = rc.payload_size_max;
    step_interval_ms_ = rc.step_interval_ms;
    // No schedule_next() needed — do_tick() handles its own pacing
}
```

### Verification:
```bash
ninja -C build 17_bench_saturate
./build/apps/bench_saturate/17_bench_saturate --headless quick-saturate --format json
```

---

## Phase 7: End-to-End Validation

### Build verification:
```bash
ninja -C build
```

### Unit tests:
```bash
ctest --output-on-failure --parallel 8
```

### Bench-specific validation:
```bash
# Quick saturate (should complete faster with higher throughput)
./build/apps/bench_saturate/17_bench_saturate --headless quick-saturate --format json

# Deep saturate (larger scale)
./build/apps/bench_saturate/17_bench_saturate --headless deep-saturate --format json

# Fan-in extreme (contention test)
./build/apps/bench_saturate/17_bench_saturate --headless fan-in-extreme --format json
```

### Expected results:
- All 1411+ existing GTest cases pass
- `quick-saturate` throughput significantly higher than baseline
- No drops at low rates (regression check)
- Latency percentiles still accurate with 1% sampling
