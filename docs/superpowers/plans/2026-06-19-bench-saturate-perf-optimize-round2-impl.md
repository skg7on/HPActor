# Bench Saturate Perf Optimization Round 2 — Implementation Plan

## Phase Overview

| Phase | Name | Tests | Files |
|-------|------|-------|-------|
| 1 | Fast-tag set: skip receive() pipeline gates | 5 | 2 |
| 2 | Lock-free message object pool | 5 | 2 |
| 3 | Wire optimizations into bench actors | 0 | 2 |
| 4 | End-to-end validation | — | — |

---

## Phase 1: Fast-Tag Set — Skip receive() Pipeline Gates

### RED — Write failing tests

Add to `tests/unit/core/test_event_based_actor.cpp` (or new file):

```cpp
TEST(FastTagReceive, FastTagSkipsGates) {
    // Create an actor with a fast tag, send a message with that tag,
    // verify the handler is invoked (and no gates block it).
}

TEST(FastTagReceive, NonFastTagGoesThroughFullPipeline) {
    // Verify messages NOT in the fast tag set still go through all gates.
}

TEST(FastTagReceive, SystemMessagesBypassFastTagCheck) {
    // System messages (Link, Unlink, Down) must always go through full pipeline
    // regardless of fast_tag_set_.
}

TEST(FastTagReceive, FastTagStillHandlesControlMessages) {
    // CLI InspectStateRequest still works on actors with fast tags enabled.
}

TEST(FastTagReceive, AddFastTagMultipleTags) {
    // Register multiple tags and verify all get fast dispatch.
}
```

### GREEN — Implement

1. Add to `EventBasedActor` (event_based_actor.hpp):
   ```cpp
   // In protected section:
   /// Register a TypeTag for fast-path dispatch (skips pipeline gates).
   void add_fast_tag(TypeTag tag) noexcept;
   
   // In private section:
   uint32_t fast_tag_mask_[8] = {}; // 256-bit bitmap for fast tags
   ```

2. In `receive()` (event_based_actor.cpp), after the FAULT_INJECT checks:
   ```cpp
   if (is_fast_tag(msg.type_id())) {
       dispatch_user_message(msg);
       return;
   }
   ```

3. `is_fast_tag()` checks the bitmap inline.
4. `add_fast_tag()` sets the corresponding bit.

### Verification:
```bash
ninja -C build test_unit_core
./build/tests/unit/core/test_unit_core --gtest_filter="*FastTag*"
```

---

## Phase 2: Lock-Free Message Object Pool

### RED — Write failing tests

Create `tests/unit/mem/test_object_pool.cpp`:

```cpp
TEST(ObjectPool, AcquireReleaseRoundTrip) {
    // Acquire, use, release, re-acquire same object.
}

TEST(ObjectPool, ExhaustionReturnsNullptr) {
    // Exhaust the pool, verify nullptr on next acquire.
}

TEST(ObjectPool, ReleaseToFullPool) {
    // Release when pool is full, verify object is deallocated.
}

TEST(ObjectPool, ObjectsAreReusable) {
    // Acquire, modify, release, re-acquire, verify state is reset.
}

TEST(ObjectPool, ConcurrentSingleProducer) {
    // Single producer (the owning actor thread) doing acquire/release.
}
```

### GREEN — Implement

New file `include/hpactor/mem/object_pool.hpp`:
```cpp
template <typename T, size_t N = 256>
class ObjectPool {
    std::array<T*, N> slots_;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> count_{0};
    
public:
    T* try_acquire();
    void release(T* obj);
    size_t available() const;
    void prefill(size_t n);
};
```

- `try_acquire()`: CAS-decrement count_, return slots_[head_ - count_ - 1]
- `release()`: CAS-increment count_, store at slots_[head_++ % N]
- On release when count_ == N: deallocate the object
- `prefill()`: pre-allocate n objects via mem::allocate()

### Verification:
```bash
ninja -C build test_unit_mem
./build/tests/unit/mem/test_unit_mem --gtest_filter="*ObjectPool*"
```

---

## Phase 3: Wire Optimizations into Bench Actors

### Sender changes (`saturate_sender_actor.hpp`):
1. Call `add_fast_tag(SendTickTag)` in constructor
2. Create an ObjectPool<TypedMessage> member
3. In `do_tick()`: use pool->try_acquire() instead of make_msg()+move for load messages

### Receiver changes (`saturate_receiver_actor.hpp`):
1. Call `add_fast_tag(LoadMessageTag)` in constructor  
2. After processing a LoadMessage, release the TypedMessage back to the sender's pool
   - Note: this requires the sender to pass its pool pointer or use a shared pool

### Verification:
```bash
ninja -C build 17_bench_saturate
./build/apps/bench_saturate/17_bench_saturate --headless quick-saturate --format json
```

---

## Phase 4: End-to-End Validation

```bash
ninja -C build
ctest --output-on-failure --parallel 8
./build/apps/bench_saturate/17_bench_saturate --headless deep-saturate --format json
```

### Expected results:
- All 2089+ existing tests pass
- New tests pass (5 fast-tag + 5 object pool = 10 new tests)
- Benchmark throughput improved vs Round 1
