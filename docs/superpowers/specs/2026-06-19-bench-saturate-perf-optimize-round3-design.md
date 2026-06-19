# Bench Saturate Perf Optimization Round 3 — Design Spec

## 1. Opt-11: StreamBuffer Small-Buffer Optimization (SBO)

**Problem:** Every StreamBuffer payload, even 16-20 byte bench messages, allocates
heap memory (via `ensure_capacity()` which enforces a minimum). The `from_data()`
factory helps by sizing the allocation to match the data, but a heap allocation
still occurs. For 1M+ msg/s, this means 1M+ allocations/s.

**Solution:** Add inline storage (128 bytes) to StreamBuffer. Payloads ≤ 128
bytes use the inline buffer with zero heap allocation. Larger payloads use the
existing `std::vector<uint8_t>` heap-backed storage. The transition is
transparent to all callers.

**Key design:**
- `uint8_t sbo_[128]` inline storage
- `uint8_t* data_` points to either `sbo_` or `buf_.data()`
- `size_t capacity_` tracks current capacity
- `bool is_sbo_` flag
- Transition to heap when size exceeds 128 bytes
- `from_data()` uses SBO directly for ≤128 byte payloads

**Files:**
- `include/hpactor/adt/stream_buffer.hpp` — complete rework of internals
- `src/adt/stream_buffer.cpp` — updated implementations

## 2. Opt-4: Batch-Enqueue for Mailbox

**Problem:** Each `try_push()` call does: reservation CAS, allocation, MPSC
enqueue CAS, edge-triggered CAS. At scale, the repeated CAS operations add up.

**Solution:** Add `try_push_batch()` that reserves once, allocates N nodes,
links them into a chain, and enqueues the chain with one CAS. Edge-triggered
wakeup fires once for the batch.

**Key design:**
- Link N nodes into a singly-linked list: node[i]->mpsc_next = node[i+1]
- Enqueue head node into MPSC queue (standard CAS)
- The consumer traverses the chain transparently via mpsc_next
- Wakeup fires once for the batch head

**Files:**
- `include/hpactor/mailbox/mpsc_actor_mailbox.hpp` — add `try_push_batch()`

## 3. Opt-9: Same-Worker Direct Dispatch (Deferred — High Risk)

Deferred due to reentrancy concerns. Direct dispatch requires proving that
receive()→send()→receive() chains don't overflow the stack or corrupt actor
state. This needs a formal reentrancy analysis.

## 4. Opt-10: Dedicated Spin-Worker (Deferred)

Deferred. The Round 2 fast-tag optimization already delivers most of the
scheduler dispatch savings. A dedicated spin-worker requires scheduler
integration that's better done as a separate feature.
