# Bench Saturate Perf Optimization Round 2 — Design Spec

**Date:** 2026-06-19
**Branch:** `feature/bench-saturate-perf-optimize`
**Issue:** #337 (remaining optimizations)

## 1. Motivation

Round 1 implemented 5 optimizations (StreamBuffer from_data, ring buffer collector,
latency downsampling, fast delivery path, cooperative sender loop). Round 2
addresses the remaining high-impact recommendations from #337:

- **Opt-5**: Fast-path receive() — skip pipeline gates for known bench messages
- **Opt-6**: Pre-allocated message object pool — eliminate per-message allocation
- **Opt-10**: Dedicated spin-worker — bypass scheduler for bench receivers

## 2. Design Goals

- **2–4× additional throughput** on top of Round 1 gains
- **No behavioral change** to production actor pipeline
- **All existing 2089 tests pass**
- **Opt-in** — fast paths are used only by bench actors

## 3. Optimizations

### 3.1 Fast-Tag Set: Skip receive() Pipeline Gates (Opt-5)

**Problem:** Every `EventBasedActor::receive()` call runs through ~8 pipeline stages
before reaching the user handler: FAULT_INJECT, trace span setup, drain gate,
system message dispatch, lifecycle gate, handler init, CLI dispatch, drain
completion, mailbox pressure check. For bench load messages (LoadMessageTag,
SendTickTag), these are pure overhead — ~150–200ns per message.

**Solution:** Add an opt-in `fast_tag_set_` (bitmask of TypeTag values in a small
range) to EventBasedActor. When `receive()` sees a message whose tag is in the
set, it skips directly to `dispatch_user_message()`. System messages and
control messages not in the set still go through the full pipeline.

The fast tag set is declared at actor construction time via a protected method
`add_fast_tag(TypeTag)`. Bench actors register their hot-path tags. This is
an additive API — actors that don't call `add_fast_tag()` have zero overhead.

**Files changed:**
- `include/hpactor/actor/event_based_actor.hpp` — add `fast_tag_set_`, `add_fast_tag()`
- `src/actor/event_based_actor.cpp` — check fast_tag_set_ in receive()
- `apps/bench_saturate/actors/saturate_sender_actor.hpp` — register SendTickTag
- `apps/bench_saturate/actors/saturate_receiver_actor.hpp` — register LoadMessageTag

### 3.2 Lock-Free Message Object Pool (Opt-6)

**Problem:** Every enqueue via `try_deliver_local_fast()` calls `mem::allocate()`
(25–50ns) + placement new. For 1M msg/s, that's 25–50ms of CPU per second
just in allocation.

**Solution:** Add a thread-local, bounded MPSC object pool for TypedMessage.
The sender allocates from its own pool (lock-free, thread-local). The receiver
deallocates back to its own pool after processing. Since each actor runs on a
single scheduler thread at a time, the pool is single-threaded per actor.

Key design:
- Fixed-size ring buffer of pre-allocated TypedMessage objects
- `try_acquire()` — CAS-pop from thread-local freelist
- `release(TypedMessage*)` — CAS-push to thread-local freelist
- Pre-allocation on first use (lazy, N objects at a time)
- Fallback to `mem::allocate()` when pool is exhausted
- Pool is owned by the actor, not globally shared

**Files changed:**
- `include/hpactor/mem/object_pool.hpp` — new lock-free object pool template
- `apps/bench_saturate/actors/saturate_sender_actor.hpp` — use pool for load messages
- `apps/bench_saturate/actors/saturate_receiver_actor.hpp` — release messages back to pool

### 3.3 Dedicated Spin-Worker for Bench Receivers (Opt-10)

**Problem:** Every message delivery goes through:
  sender enqueue → CAS wakeup → scheduler notify_ready → placement →
  worker pick up → execute_actor → CAS state → dequeue → receive

For bench receivers at high throughput, this scheduler round-trip adds
~200–400ns per message.

**Solution:** Add a `PollingActor` mode where the actor's mailbox is continuously
polled by a dedicated thread, bypassing the scheduler entirely. The actor's
`receive()` is called directly in a tight loop.

Key design:
- Actor opts in via `set_polling_mode(true)` at construction
- A dedicated `std::thread` runs a poll loop: dequeue → receive → repeat
- The thread uses `std::this_thread::yield()` or short sleeps when idle
- The existing `DaemonActor`/`PollingActor` base class already has the dedicated
  thread infrastructure — we adapt it for high-throughput message polling
- Control messages (stop, inspect) still work via the normal mailbox path
- Rate limiting via `ActorRateLimiter` integration

**Alternative:** Instead of a full dedicated thread, use the existing scheduler's
`RequeueReady` budget (64 consecutive cycles) which already provides continuous
execution for hot actors. The sender already benefits from this. The receiver
automatically benefits when messages arrive continuously. The dedicated thread
is only needed if we want to eliminate the CAS state transitions entirely.

**Decision:** For Round 2, we implement Opt-5 and Opt-6 first. Opt-10 is deferred
pending profiling to determine if scheduler dispatch is still a bottleneck after
Opt-5 and Opt-6.

## 4. Non-Goals for Round 2

- **Opt-4: Batch-enqueue API** — Changes MPSCMailbox internals; needs careful
  CAS ordering verification. Deferred.
- **Opt-9: Same-worker direct dispatch** — High architectural risk; needs
  reentrancy analysis. Deferred.
- **Opt-10: Dedicated spin-worker** — Deferred until profiling confirms need.
- **Opt-11: StreamBuffer SBO** — Affects entire codebase; deferred.

## 5. Testing Strategy

1. **Fast-tag set**: Unit test verifying gates are skipped for fast tags;
   integration test verifying control messages still go through full pipeline
2. **Object pool**: Unit test for acquire/release, exhaustion fallback,
   concurrent single-producer safety
3. **Integration**: Full bench saturate run with both optimizations enabled

## 6. Files Changed Summary

| File | Change |
|------|--------|
| `include/hpactor/actor/event_based_actor.hpp` | Add fast_tag_set_, add_fast_tag(), fast receive path |
| `src/actor/event_based_actor.cpp` | Check fast_tag_set_ in receive() |
| `include/hpactor/mem/object_pool.hpp` | New: lock-free object pool template |
| `apps/bench_saturate/actors/saturate_sender_actor.hpp` | Use object pool + fast tags |
| `apps/bench_saturate/actors/saturate_receiver_actor.hpp` | Use fast tags + release to pool |
