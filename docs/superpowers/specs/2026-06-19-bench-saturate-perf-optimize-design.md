# Bench Saturate Performance Optimization — Design Spec

**Date:** 2026-06-19
**Branch:** `feature/bench-saturate-perf-optimize`
**Issue:** #337

## 1. Motivation

The `quick-saturate` benchmark measures actor system message throughput but is
bottlenecked by overhead in the general-purpose production delivery pipeline,
StreamBuffer over-allocation, per-message latency reporting fan-out, and
timer-driven self-scheduling. The measured throughput reflects accumulated
framework overhead rather than fundamental actor-model limits.

This design addresses the five highest-ROI bottlenecks identified in issue #337,
targeting a 5–10× throughput improvement with low-risk, localized changes.

## 2. Design Goals

- **5–10× throughput increase** for `quick-saturate` (100 senders → 10 receivers, 16B)
- **No behavioral change** to production delivery pipeline
- **All existing tests pass** — no regression in actor system correctness
- **Changes are opt-in** — fast paths are used only by the bench app
- **Minimal API surface change** — new methods are additive, not breaking

## 3. Optimizations

### 3.1 StreamBuffer Small-Message Constructor

**Problem:** `StreamBuffer(buf, buf+len)` uses the iterator constructor, which
calls `ensure_capacity()` that enforces `kDefaultInitialCapacity = 65536`.
A 16-byte bench message payload allocates a 64KB backing vector.

**Solution:** Add `StreamBuffer::from_data(const uint8_t*, size_t)` — a named
static factory that creates a buffer with capacity exactly matching the data
size. The existing `with_capacity()` + `append()` pattern works but requires
two calls; the factory combines them.

**Files changed:**
- `include/hpactor/adt/stream_buffer.hpp` — add `static StreamBuffer from_data(const uint8_t* data, size_t len);` declaration
- `src/adt/stream_buffer.cpp` — implement

**Usage in bench:**
```cpp
// Before: StreamBuffer(buf, buf + sizeof(buf))  → 64KB heap alloc
// After:  StreamBuffer::from_data(buf, sizeof(buf)) → exact-capacity alloc
```

### 3.2 Latency Reporting Downsampling

**Problem:** The receiver sends one `LatencySample` to the collector for every
load message received. At 1M msg/s throughput, this doubles message volume
through the system and overwhelms the collector.

**Solution:** Sample latency at 1% (every 100th message). At 1M msg/s, this
produces 10K samples/s — statistically sufficient for accurate P50/P99/P999
with a 10K-sample reservoir.

**Files changed:**
- `apps/bench_saturate/actors/saturate_receiver_actor.hpp` — change sampling condition

### 3.3 Collector Lock-Free Ring Buffer

**Problem:** The collector uses `std::vector<double> latencies_` with an
O(n) erase-from-front pattern to maintain a 10K-sample reservoir. At 1M
insertions/s, this causes ~79 GB/s of memmove.

**Solution:** Replace with a fixed-size array + atomic write index. The
percentile recomputation sorts only the valid samples (up to 10K), which
is O(n log n) but infrequent (called on StatsPoll and Stop).

**Alternative considered:** Using the existing `MpscRingBuffer`. Rejected
because it's template-parameterized on event size and doesn't support
random-access reads needed for percentile sorting. A plain array with
atomic index is simpler and sufficient.

**Files changed:**
- `apps/bench_saturate/actors/saturate_collector_actor.hpp` — replace `std::vector<double>` with `std::array<std::atomic<double>, 10000>` + atomic write index

### 3.4 Fast Local Delivery Path

**Problem:** Every `try_deliver_local()` call traverses the full
`DeliveryPipeline`: circuit breaker check, TTL application, dedup check,
expiry check, and backpressure signaling. For bench messages with no TTL,
no dedup, and no circuit breaker configured, these are pure overhead.

**Solution:** Add `ActorSystem::try_deliver_local_fast(ActorId, TypedMessage)`
that bypasses the pipeline and enqueues directly to the mailbox. This is
semantically equivalent to `try_deliver_local()` with default options when:
- No circuit breaker is configured on the target actor
- No TTL is set (deadline_ns = INT64_MAX)
- No dedup is needed (best-effort delivery mode)
- No backpressure signaling is required

The caller (bench sender) is responsible for ensuring these preconditions.

**Files changed:**
- `include/hpactor/core/actor_system.hpp` — declare `try_deliver_local_fast()`
- `src/actor/actor_system.cpp` — implement

### 3.5 Cooperative Loop Instead of Timer Self-Scheduling

**Problem:** The sender uses `context()->schedule(1ms, ...)` → timer fire →
`deliver_local()` → `receive()` → `do_tick()` for each batch. This requires:
- `shared_ptr<TypedMessage>` allocation per schedule
- `std::function` allocation per schedule
- Full DeliveryPipeline for the tick message
- Scheduler round-trip for every batch
- 1ms minimum timer granularity caps at 10K msg/s per sender

**Solution:** Replace with a tight send loop that uses the existing scheduler
requeue budget (`kRequeueBudget = 64`) for natural interleaving with other
actors. The sender processes as many batches as the scheduler's requeue
budget allows before yielding.

The new flow:
1. `SaturateStart` handler: set running_=true, directly enter the send loop
2. Send loop: for each batch, directly call `try_deliver_local_fast()` for
   each message, then check if the requeue budget is exhausted
3. If budget not exhausted: return from handler, scheduler re-invokes via
   `RequeueReady` path (no timer, no pipeline, no CAS wakeup)
4. Rate throttling: track elapsed time between batches; if ahead of target
   rate, use a zero-delay self-schedule to yield

**Files changed:**
- `apps/bench_saturate/actors/saturate_sender_actor.hpp` — rewrite scheduling logic

## 4. Non-Goals

- **Batched mailbox enqueue API** (Opt-4 in analysis) — deferred; requires
  mailbox API changes and careful CAS ordering verification
- **Same-worker direct dispatch** (Opt-9) — deferred; high architectural risk
- **Dedicated spin-worker** (Opt-10) — deferred; needs scheduler integration
- **StreamBuffer SBO** (Opt-11) — deferred; affects entire codebase
- **Message object pool** (Opt-6) — deferred; adds complexity
- **`receive()` fast path override** (Opt-5) — deferred; the other changes
  provide sufficient gain without modifying the core actor pipeline

## 5. Risk Assessment

| Change | Risk | Mitigation |
|--------|------|------------|
| StreamBuffer factory | Low | Additive API; all existing call sites unaffected |
| Latency downsampling | Low | 1% of 1M = 10K samples; statistically sufficient |
| Ring buffer collector | Low | Write index wraps; percentile sort handles partial fills |
| Fast delivery path | Low | Opt-in; only bench sender uses it; mailbox invariants preserved |
| Cooperative loop | Medium | Must respect requeue budget to avoid monopolizing worker; rate throttling preserves benchmark accuracy |

## 6. Testing Strategy

Each optimization gets a focused unit test before implementation:

1. **StreamBuffer factory**: `test_stream_buffer` — verify `from_data()` produces
   exact-capacity buffer, verify data round-trips correctly
2. **Latency downsampling**: existing bench tests already validate the
   saturation pipeline; no new test needed (behavioral change to sampling rate)
3. **Ring buffer collector**: `test_saturate_collector` — verify wrapped writes
   overwrite oldest samples, percentile computation on partial fills
4. **Fast delivery path**: `test_delivery` — verify fast path enqueues correctly,
   verify fast path rejects missing actors
5. **Cooperative loop**: existing bench tests validate sender behavior;
   manual verification via `/saturate start quick-saturate`

## 7. Files Changed Summary

| File | Change |
|------|--------|
| `include/hpactor/adt/stream_buffer.hpp` | Add `from_data()` static factory declaration |
| `src/adt/stream_buffer.cpp` | Implement `from_data()` |
| `include/hpactor/core/actor_system.hpp` | Add `try_deliver_local_fast()` declaration |
| `src/actor/actor_system.cpp` | Implement `try_deliver_local_fast()` |
| `apps/bench_saturate/messages.hpp` | Use `StreamBuffer::from_data()` in all bench message encoders |
| `apps/bench_saturate/actors/saturate_sender_actor.hpp` | Replace timer ticks with cooperative loop; use fast delivery path |
| `apps/bench_saturate/actors/saturate_receiver_actor.hpp` | Downsample latency reporting to 1% |
| `apps/bench_saturate/actors/saturate_collector_actor.hpp` | Replace vector reservoir with ring buffer |
