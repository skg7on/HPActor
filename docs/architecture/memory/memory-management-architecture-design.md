# HPActor Memory Management Architecture Design

**Date:** 2026-05-03 (original), updated 2026-06-20
**Status:** Implemented (M1–M8 complete), Evolution Phase 1–2 implemented (MEM-001 through MEM-007)
**Scope:** Custom allocator, observability, debugging, hibernation, memory compression, fragmentation control, NUMA-aware allocation
**Companion Docs:**
- `docs/architecture/memory/memory-management-erlang-gap-analysis.md` — Erlang BEAM comparison & optimization roadmap
- `docs/superpowers/specs/2026-06-20-mem-01` through `mem-06` — detailed design specs for Phase 1–2 optimizations
- `docs/superpowers/plans/2026-06-20-mem-01` through `mem-06` — implementation plans
- PR #343 — implementation

## Table of Contents

1. [Overview and Design Goals](#1-overview-and-design-goals)
2. [Two-Tier Slab Allocator](#2-two-tier-slab-allocator)
3. [Typed Memory Regions](#3-typed-memory-regions)
4. [Generation-Based Slab Management](#4-generation-based-slab-management)
5. [Allocation Metadata and Header Wrappers](#5-allocation-metadata-and-header-wrappers)
6. [Observability Subsystem](#6-observability-subsystem)
7. [Debugging: Poisoning and Guard Pages](#7-debugging-poisoning-and-guard-pages)
8. [Hibernation and Cold Storage](#8-hibernation-and-cold-storage)
9. [Memory Compression via ZRAM](#9-memory-compression-via-zram)
10. [Determinism and Fragmentation Control](#10-determinism-and-fragmentation-control)
11. [Integration with Existing Code](#11-integration-with-existing-code)
12. [Implementation Phases](#12-implementation-phases)

---

## 1. Overview and Design Goals

### 1.1 Problem Statement

At million-actor scale, general-purpose allocators (`malloc`, `new`) become the dominant bottleneck:

- **Lock contention** on `malloc` arenas under high-concurrency spawn/message/death traffic
- **Metadata overhead** — glibc malloc adds 16-32 bytes per allocation, overwhelming L1/L2 caches
- **External fragmentation** — after weeks of churn, allocation latency degrades from O(1) to O(n) free-list walks
- **No introspection** — `malloc` provides zero visibility into per-actor memory pressure
- **No cold-storage path** — idle actors occupy hot DRAM indistinguishable from active ones

### 1.2 Design Goals

| Goal | Metric | Target |
|------|--------|--------|
| Allocation latency | p99.99 | < 50 ns (thread-local hit) |
| Fragmentation | Internal/external waste | < 5% after 7-day uptime |
| Observability | Per-actor byte tracking | Atomic counter array, no locks on hot path |
| Hibernation density | Actors/GB DRAM | 1M hibernated actors in < 256 MB hot + ZRAM cold |
| Corruption detection | Use-after-free / overflow | Detected at `free()` or hibernation checkpoint |
| Back-pressure | OOM prevention | Watermark-based reclaim before ENOMEM |

### 1.3 Key Design Principles

1. **Thread-local first.** The hot allocation path never crosses a cache line boundary to another thread. Global coordination only happens at segment acquisition (amortized over thousands of allocations).

2. **Actor memory is typed and sized.** Every allocation knows its owner ActorId, its size class, and its type tag — no blind `free(void*)`.

3. **Manual memory, absolute predictability.** No GC pauses. No reference counting cascades. The allocator is a deterministic state machine.

4. **Observability is embedded, not bolted on.** Every allocation event can be sampled into a lock-free ring buffer without extra branches on the hot path.

5. **Hibernation is a first-class lifecycle state.** Actors transition between Hot (slab-backed) and Cold (serialized + madvise-backed) memory transparently.

### 1.4 Current Implementation Status (as of 2026-06-20)

All 8 implementation phases (M1–M8) described in Section 12 are complete.
Phase 1–2 evolution optimizations (MEM-001 through MEM-007) have been
implemented on branch `feature/mem-erlang-gap-optimizations` (PR #343).

The memory subsystem now has:

- **18 public headers** under `include/hpactor/mem/` (added `super_carrier.hpp`)
- **13 source files** under `src/mem/` (added `super_carrier.cpp`)
- **22 unit test files** under `tests/unit/mem/` (added `test_segregated_freelist.cpp`,
  `test_free_block_coalescing.cpp`, `test_super_carrier.cpp`)
- **152 total memory tests passing** (up from 110 baseline)
- **6 fault injection sites** in allocator paths

The current architecture has been extended with:

**Layer 0 (new): Virtual Memory Management**
- `SuperCarrier` — contiguous virtual reservation (8–64GB), mprotect-based slab carving
- NUMA-aware per-node sub-regions via `carve_numa()`
- Huge page support: MAP_HUGETLB with THP fallback for both carrier and legacy segments
- SegmentProvider now prioritizes carrier carve over individual mmap

**Layer 2 (updated): Slab Allocator**
- `AllocationStrategy` enum: `kCasLifo` (default), `kSegregatedFit`, `kBumpOnly`
- `kSegregatedFit`: bump-first allocation + 8-bin address-segregated free lists with
  doubly-linked intrusive list for O(1) middle removal (enables coalescing)
- `kBumpOnly`: bump-only allocation with idle slab recycling for `kMessage`/`kNetwork`
- `MemoryStrategyTable` + `kDefaultStrategies`: per-region compile-time configuration

**Layer 2.5 (new): Fragmentation Control**
- `BoundaryFooter` — zero-overhead coalescing metadata in freed block's CanaryFooter slot
- `try_coalesce()`: immediate left+right neighbor merging on every deallocate
- Doubly-linked free list (`FreeBlockLinkage`) for O(1) block removal from any bin position

**Message Layer (new):**
- `TypedMessage::create_inline()` — zero-allocation message factory for payloads ≤32B
- `kCanInlinePayload<T>` constexpr trait for compile-time inline dispatch

### 1.5 Evolution Status

Phase 1–2 optimizations (MEM-001 through MEM-007) are **implemented** on branch
`feature/mem-erlang-gap-optimizations` (PR #343).

| MEM | Priority | Feature | Status |
|-----|----------|---------|--------|
| 001 | P0 | Segregated Free Lists (8-bin, round-robin, bump-first) | ✅ Implemented |
| 002 | P0 | Free Block Coalescing (boundary tags, doubly-linked freelist) | ✅ Implemented |
| 003 | P1 | Per-Region Strategy Selection (bump-only, segregated+coalescing) | ✅ Implemented |
| 004 | P1 | Super Carrier (contiguous virtual reservation, mprotect carving) | ✅ Implemented |
| 005 | P1 | Huge Page Support (MAP_HUGETLB, THP fallback, legacy segment path) | ✅ Implemented |
| 006 | P1 | Message Inlining (TypedMessage inline storage, constexpr trait) | ✅ Implemented |
| 007 | P3 | NUMA-Aware Memory Manager (per-node carve, topology detection) | ✅ Implemented |

All optimizations preserve backward compatibility with opt-in defaults. The
existing hibernation, tracking, debugging, and compaction subsystems are
unchanged.

---

## 2. Two-Tier Slab Allocator

### 2.1 Architecture Overview

The current architecture uses a **two-tier design** with per-thread region×size-class
caches backed by a global segment provider:

```
┌──────────────────────────────────────────────────────────────────┐
│                     Tier 0: SegmentProvider (global singleton)    │
│                                                                   │
│  mmap(MAP_PRIVATE | MAP_ANONYMOUS, 2MB) per segment              │
│  Mutex-protected carve: segments_ vector + slab_records_ map     │
│  slab_records_: O(1) pointer→slab lookup for cross-thread free   │
│  Ref-counted segments: munmap when all slabs released             │
└──────────────────────┬───────────────────────────────────────────┘
                       │
         ┌─────────────┼─────────────┐
         ▼             ▼             ▼
┌─────────────┐ ┌─────────────┐ ┌─────────────┐
│ ThreadLocal  │ │ ThreadLocal  │ │ ThreadLocal  │
│ Allocator    │ │ Allocator    │ │ Allocator    │
│ (Worker 0)   │ │ (Worker 1)   │ │ (Worker N)   │
│              │ │              │ │              │
│ 6 RegionType × 8 SizeClass   │ │              │
│ = 48 SlabCache instances     │ │              │
│              │ │              │ │              │
│ Each SlabCache:              │ │              │
│  bump ptr (virgin alloc)     │ │              │
│  + CAS LIFO freelist (recycled)│              │
│              │ │              │ │              │
│ Slab sizes:  │ │              │ │              │
│  64KB (32–128B blocks)       │ │              │
│  128KB (256B blocks)         │ │              │
│  256KB (512B–1KB blocks)     │ │              │
│  512KB (2KB–4KB blocks)      │ │              │
└─────────────┘ └─────────────┘ └─────────────┘
```

**Evolution note:** The single CAS LIFO freelist per `SlabCache` is the target
of MEM-001 (segregated free lists) and MEM-002 (free block coalescing). See
`docs/architecture/memory/memory-management-erlang-gap-analysis.md` for details.

### 2.2 Tier 0: Global Segment Provider

The SegmentProvider is a singleton that acquires large contiguous virtual memory regions from the OS and carves them into slabs for thread-local caches.

```cpp
// include/hpactor/mem/segment_provider.hpp

namespace hpactor::mem {

class SegmentProvider {
public:
    struct Segment {
        void* base;            // mmap'd base address
        size_t size;           // total size (e.g., 2MB)
        size_t offset;         // next carve offset
        uint32_t segment_id;
        std::atomic<uint32_t> ref_count; // active slab count
    };

    // Size classes for slab allocation
    static constexpr size_t kNumSizeClasses = 8;
    static constexpr size_t kSizeClasses[kNumSizeClasses] = {
        32, 64, 128, 256, 512, 1024, 2048, 4096
    };

    // Acquire a carved slab for a given size class.
    // Returns nullptr if OOM.
    Slab* acquire_slab(size_t size_class);

    // Release a slab back to the provider (refcount → 0 triggers munmap).
    void release_slab(Slab* slab);

    // Global stats
    size_t total_allocated() const;
    size_t total_waste() const;

private:
    std::mutex mutex_;                    // low-contention: only on slab acquire/release
    std::vector<std::unique_ptr<Segment>> segments_;
    std::unordered_map<void*, Segment*> addr_to_segment_; // for free() lookup
};

} // namespace hpactor::mem
```

**Design decisions:**
- Uses `mmap` with `MAP_PRIVATE | MAP_ANONYMOUS` and optionally `MAP_HUGETLB` (2MB pages) on Linux to reduce TLB pressure.
- Segments are 2MB — large enough to amortize the mmap cost over ~500-64K allocations (depending on size class), small enough to munmap when a size class goes cold.
- Ref-counted segments: when all slabs carved from a segment are released back, the segment is `munmap`'d.
- The address-to-segment map enables O(1) lookup from any allocated pointer back to its owning segment, which is required for `free()` without storing a segment pointer in each block.

### 2.3 Tier 1: Thread-Local Slab Cache

Each worker thread owns a `ThreadLocalSlabCache` — an array of size-class-specific slabs. This is the hot path.

```cpp
// include/hpactor/mem/thread_local_slab_cache.hpp

namespace hpactor::mem {

class ThreadLocalSlabCache {
public:
    ThreadLocalSlabCache();

    // Allocate a block of the given size class.
    // Returns pointer to usable memory (after AllocHeader).
    void* allocate(uint8_t size_class_idx, ActorId owner);

    // Free a block back to its slab.
    void deallocate(void* ptr);

    // Per-size-class stats
    struct SlabStats {
        std::atomic<uint64_t> alloc_count;
        std::atomic<uint64_t> free_count;
        std::atomic<uint64_t> cache_miss_count; // had to go to Tier 0
    };
    const SlabStats& stats(uint8_t size_class_idx) const;

private:
    struct Slab {
        uint8_t* memory;       // start of usable region
        uint8_t* bump;         // next free address (bump allocator)
        uint8_t* end;          // end of usable region
        FreeList* freelist;    // lock-free freelist for freed blocks
        uint32_t size_class;
    };

    Slab slabs_[SegmentProvider::kNumSizeClasses];
    SlabStats stats_[SegmentProvider::kNumSizeClasses];
};

} // namespace hpactor::mem
```

**Slab internal structure (bump + freelist hybrid):**

```
┌──────────────────────────────────────────────────────────────┐
│                    Slab Memory Layout                          │
│                                                               │
│  ┌──────────┬──────────┬──────────┬──────────┬────────────┐  │
│  │ Block 0  │ Block 1  │ Block 2  │  ...free │   bump →    │  │
│  │ (in use) │ (in use) │ (freed)  │  blocks  │  (virgin)   │  │
│  └──────────┴──────────┴──────────┴──────────┴────────────┘  │
│       ▲                        ▲            ▲                 │
│       │                        │            │                 │
│       │              recycled via freelist     │                 │
│       │              (CAS-pop on alloc)       fresh bump      │
│       │                                       allocation      │
│  Each block: AllocHeader | user data | CanaryFooter           │
└──────────────────────────────────────────────────────────────┘
```

**Allocation fast path (hot):**

```
allocate(size_class, owner):
    slab = &slabs_[size_class]

    // 1. Try freelist first (recycled blocks)
    block = slab.freelist.pop()       // lock-free CAS
    if (block) goto stamp_header

    // 2. Bump allocate from virgin region
    block = slab.bump
    next = block + block_size(size_class)
    if (next > slab.end) goto slow_path  // slab exhausted
    slab.bump = next

stamp_header:
    header = AllocHeader::from_block(block)
    header->owner = owner
    header->magic = kAllocMagic
    header->size_class = size_class
    header->timestamp = rdtsc()
    stats_[size_class].alloc_count++
    return header->user_data()

slow_path:
    // Refill slab from SegmentProvider
    // (amortized: once per ~500 allocations for 4KB blocks, ~64K for 32B blocks)
    new_slab = SegmentProvider::acquire_slab(size_class)
    if (!new_slab) return nullptr  // OOM
    replace_exhausted_slab(size_class, new_slab)
    goto retry
```

**Deallocation fast path:**

```
deallocate(ptr):
    header = AllocHeader::from_user_data(ptr)

    // Corruption check
    if (header->magic != kAllocMagic) → corruption_panic()
    footer = CanaryFooter::from_block(header)
    if (footer->magic != kAllocMagic) → overflow_panic()

    // Poison on free (debug builds)
    memset(ptr, kPoisonPattern, header->block_size())

    header->magic = kFreedMagic
    stats_[header->size_class].free_count++

    slab = header->owning_slab()
    slab.freelist.push(header)       // lock-free CAS
```

### 2.4 Size Class Selection

| Size Class | Block Size (32B header + payload + 8B footer) | Slab Size | Blocks/Slab | Use Case |
|------------|-----------------------------------------------|-----------|-------------|----------|
| 32B | 72B total (32B header + 32B user + 8B footer) | 64KB | ~910 | Tiny messages, ActorId payloads |
| 64B | 104B total (32B header + 64B user + 8B footer) | 64KB | ~630 | Small messages, TypedMessage headers |
| 128B | 168B total | 64KB | ~390 | Medium messages, small actor state |
| 256B | 296B total | 128KB | ~443 | Large messages, serialized protobuf chunks |
| 512B | 552B total | 256KB | ~475 | Frame headers, small actor metadata |
| 1KB | 1064B total | 256KB | ~246 | Coroutine frames (small), actor state snapshots |
| 2KB | 2088B total | 512KB | ~251 | Medium coroutine stacks |
| 4KB | 4136B total | 512KB | ~127 | Default coroutine stacks, large actor state |

**Overhead ratio:** At 32B size class, block overhead is 125% (40B overhead / 32B user).
See MEM-006 (message inlining) for the near-term mitigation and Section 5.2 for the
long-term tiny-block packed metadata design.

Blocks larger than 4KB use the fat-block path: direct `mmap` via `guarded_alloc()`
with `PROT_NONE` guard pages at both ends (see Section 7.3).

### 2.5 Lock-Freedom Guarantees

**Hot path (Thread-local, no atomics):**
- Bump allocation: single store to `slab.bump` pointer.
- Deallocation freelist push to owning thread's cache: CAS freelist push.
- Deallocation freelist push from foreign thread: cross-thread free is routed to
  the origin `SlabCache` via `SegmentProvider::lookup_slab()`, then a CAS push.

**Slow path (Global, mutex-protected, amortized):**
- `SegmentProvider::acquire_slab()`: mutex-protected carve from segment or new
  `mmap`. Hit rate < 0.1% (once per hundreds to tens-of-thousands of allocations).
- `MemoryRegionRegistry::try_reserve()`: CAS-based admission, no mutex.
- `MemoryTracker::record_alloc()`: relaxed atomic increments, no mutex.

**Evolution note:** The current CAS LIFO freelist provides O(1) push/pop with
no middle removal. For the segregated free list (MEM-001) and coalescing
(MEM-002) evolution, the freelist is upgraded to a doubly-linked intrusive list
with O(1) middle removal. Cross-thread deallocation is already routed to the
owning thread, so `remove()` is single-threaded — no CAS needed for the upgrade.

---

## 3. Typed Memory Regions

### 3.1 Concept

Instead of a single heap where messages, actors, and coroutine frames are indistinguishable, HPActor uses **Typed Memory Regions** — each allocation type gets its own slab set with type-specific lifetime rules and observability counters.

### 3.2 Region Types

```cpp
// include/hpactor/mem/memory_region.hpp

namespace hpactor::mem {

enum class RegionType : uint8_t {
    kActor     = 0,  // Actor instances (shared_ptr backing, ActorContext)
    kMessage   = 1,  // Message payloads (TypedMessage<T>, protobuf wire bytes)
    kCoroutine = 2,  // Coroutine stack frames
    kNetwork   = 3,  // Network buffers (Frame encode/decode, I/O buffers)
    kInternal  = 4,  // Scheduler internal structures (queues, timer wheel nodes)
    kHibernate = 5,  // Hibernation buffers (serialized actor state)
};

class TypedRegion {
public:
    RegionType type;
    ThreadLocalSlabCache cache;  // per-region slab cache

    // Region-specific stats
    struct alignas(64) RegionStats {
        std::atomic<uint64_t> total_allocated;
        std::atomic<uint64_t> total_freed;
        std::atomic<uint64_t> active_bytes;
        std::atomic<uint64_t> high_water_mark;
        std::atomic<uint64_t> alloc_count;
        std::atomic<uint64_t> free_count;
        std::atomic<uint64_t> corruption_events;
    };
    RegionStats stats;
};

} // namespace hpactor::mem
```

### 3.3 Per-Region Back-Pressure Policies

Each region type has a configurable water mark that triggers reclaim:

| Region | High Water Mark | Reclaim Action |
|--------|-----------------|----------------|
| kActor | 80% of actor slab capacity | Reject spawn, notify supervisor |
| kMessage | 90% of message slab capacity | Drop non-critical messages, apply back-pressure to senders |
| kCoroutine | 95% of frame pool | Stall actor activation until frames are released |
| kNetwork | 75% of network buffer capacity | Close idle connections, shrink connection pool |
| kInternal | Fixed pre-allocated | No reclaim; considered critical infrastructure |
| kHibernate | No hard limit | `madvise(MADV_PAGEOUT)` oldest buffers first |

---

## 4. Generation-Based Slab Management

### 4.1 The Fragmentation Problem

With bump allocation + freelist, freed blocks create holes in slabs. Over time, a slab can become a checkerboard of used/freed blocks — internal fragmentation. Worse, if all blocks in a slab are freed *except one*, that one block pins the entire slab (external fragmentation at the segment level).

### 4.2 Generation Tracking

Each slab carries a **generation number** and a **live block count**:

```cpp
struct SlabGenerationInfo {
    uint64_t generation;             // incremented on each compaction cycle
    std::atomic<uint32_t> live_count; // blocks currently in use
    uint32_t total_blocks;           // max blocks in this slab
    uint32_t compaction_threshold;   // compact when live/total < threshold (e.g., 25%)
};
```

### 4.3 Compaction by Relocation

When a slab falls below its compaction threshold (e.g., < 25% utilized), the allocator triggers **slab compaction**:

```
compact_slab(slab):
    for each live block in slab:
        new_block = allocate_same_size_class(block.owner)
        memcpy(new_block, block, block.size)
        update_actor_pointer(block.owner, block → new_block)
        free(block)
    release_slab(slab)  // return to SegmentProvider, may munmap
```

**Why this works for actors:** Actors are referenced by ActorId, not raw pointers. The `update_actor_pointer()` step updates the actor registry's pointer — no dangling references in the scheduler or message queues. Messages in flight carry the payload inline, so no pointer update is needed.

**For coroutine frames:** Coroutine frames cannot be relocated trivially (the running coroutine holds stack pointers into the frame). The compaction strategy for coroutines is different: idle coroutines are serialized to hibernation buffers and their frames freed; active coroutines are left in place and their slab is excluded from compaction.

### 4.4 Generation-Based Use-After-Free Detection

Every ActorId carries an **incarnation counter** (already present as `incarnation_type`). The allocator extends this by storing the generation in the AllocHeader:

```cpp
struct AllocHeader {
    uint32_t owner_id;       // ActorId that owns this block
    uint32_t incarnation;    // actor incarnation at allocation time
    uint32_t magic;          // kAllocMagic (0xAC70AC70)
    uint8_t  size_class;     // index into size class table
    uint8_t  generation;     // slab generation at allocation time
    uint16_t flags;          // region type, poison flag, etc.
    uint64_t timestamp;      // rdtsc at allocation (for age tracking)
};
```

If a freed block is accessed, the generation in the header will not match the current generation of the slab — the allocator detects this at the next integrity check.

---

## 5. Allocation Metadata and Header Wrappers

### 5.1 Block Layout

Every allocated block has this structure:

```
┌────────────────────────────────────────────────────┐
│                  AllocHeader (32 bytes)              │
│  owner_id (4B) │ incarnation (4B) │ magic (4B)     │
│  size_class (1B) │ generation (1B) │ flags (2B)    │
│  timestamp (8B) │ padding (8B)                     │
├────────────────────────────────────────────────────┤
│                  User Data (variable)               │
│                  ...payload...                      │
├────────────────────────────────────────────────────┤
│                  CanaryFooter (8 bytes)             │
│  magic (4B)     │ checksum (4B)                    │
└────────────────────────────────────────────────────┘
```

- **AllocHeader:** 32 bytes, cache-line aligned when block size ≥ 64B. Contains all metadata needed for `free()` and observability.
- **CanaryFooter:** 8 bytes at the end of the block. Overwritten by buffer overflow → detected at `free()` or periodic scan.
- **Total overhead:** 40 bytes per allocation. At the 32B size class, this is suboptimal — see Section 5.2 for the tiny-block optimization.

### 5.2 Tiny-Block Optimization (≤ 32B)

**Status:** Design spec — not yet implemented. Tracked as Phase 3.4 (P2) in the
evolution roadmap. Near-term mitigation: MEM-006 (message inlining) eliminates
allocations entirely for messages ≤ 32B, bypassing the overhead problem.

For the 32B size class, the full 40-byte overhead is prohibitive. Instead, 32B
blocks will use a **packed header** stored out-of-band in a bitmap:

```
Slab for 32B blocks:
┌──────────────────────────────────────────────┐
│  Metadata Region (bitmap + owner array)       │
│  block_allocated[256] = 32 bytes bitmap       │
│  block_owner[256]    = 1024 bytes (4B each)  │
│  block_magic[256]    = 1024 bytes (4B each)  │
├──────────────────────────────────────────────┤
│  Data Region (256 × 32B = 8KB)               │
│  Block[0] Block[1] ... Block[255]             │
└──────────────────────────────────────────────┘
```

This reduces overhead for 32B blocks from 125% (40/32) to 25% (2KB metadata / 8KB data) at the cost of an indexed lookup on `free()`.

### 5.3 Fat-Block Path (> 4KB)

For allocations larger than 4KB, blocks are allocated directly from the SegmentProvider as individual `mmap` regions — no slab, no bump allocator. The AllocHeader is stored at the start of the mmap'd region, followed by user data, followed by a guard page (see Section 7).

---

## 6. Observability Subsystem

### 6.1 Per-Actor Shadow Counters

A global array of atomic counters, indexed by ActorId, tracks per-actor memory usage:

```cpp
// include/hpactor/mem/memory_tracker.hpp

namespace hpactor::mem {

class MemoryTracker {
public:
    static constexpr size_t kMaxTrackedActors = 1'000'000;

    struct alignas(64) ActorMemoryStats {
        std::atomic<uint64_t> current_bytes;  // current allocated bytes
        std::atomic<uint64_t> peak_bytes;     // high water mark
        std::atomic<uint64_t> alloc_count;    // total allocations
        std::atomic<uint64_t> free_count;     // total frees
        std::atomic<uint32_t> last_alloc_ns;  // timestamp of last alloc
    };

    // Record an allocation for an actor. Returns false if over limit.
    bool record_alloc(ActorId actor, size_t bytes);

    // Record a deallocation.
    void record_free(ActorId actor, size_t bytes);

    // Query stats for an actor (zero-cost when not queried)
    ActorMemoryStats snapshot(ActorId actor) const;

    // Global aggregates
    uint64_t total_active_bytes() const;
    uint64_t total_peak_bytes() const;

private:
    // Array of atomic stats — direct indexed access, O(1), lock-free
    std::vector<ActorMemoryStats> stats_;  // indexed by ActorId::index()
};

} // namespace hpactor::mem
```

**Design notes:**
- The `ActorMemoryStats` struct is `alignas(64)` — exactly one cache line. False sharing between adjacent actor stats is eliminated.
- The index is derived from ActorId directly (no hash table lookup on the hot path).
- `record_alloc` and `record_free` use `fetch_add` on `current_bytes` and a `compare_exchange` loop only on `peak_bytes` (infrequently contested).

### 6.2 Lock-Free Telemetry Ring Buffer

A lock-free multi-producer single-consumer (MPSC) ring buffer streams allocation events to a background telemetry thread:

```cpp
// include/hpactor/mem/telemetry_ring_buffer.hpp

namespace hpactor::mem {

struct AllocationEvent {
    uint64_t timestamp;     // rdtsc
    uint32_t actor_id;
    uint8_t  size_class;
    uint8_t  region_type;   // RegionType enum
    uint8_t  event_type;    // 0=alloc, 1=free, 2=corruption, 3=hibernate_in, 4=hibernate_out
    uint8_t  padding;
    uint32_t block_size;
};

class TelemetryRingBuffer {
public:
    static constexpr size_t kBufferSize = 1 << 16; // 65K events (~2MB at 32B/event)

    // Lock-free push from any thread (allocation hot path)
    bool try_push(const AllocationEvent& event);

    // Called by telemetry thread: drains all events
    template<typename F>
    void drain(F&& callback);

private:
    alignas(64) std::atomic<uint64_t> write_cursor_{0};
    alignas(64) std::atomic<uint64_t> read_cursor_{0};
    AllocationEvent buffer_[kBufferSize];
};

} // namespace hpactor::mem
```

**Telemetry thread responsibilities:**
1. Drain the ring buffer at 10Hz (every 100ms)
2. Compute histograms: allocation size distribution, per-actor memory growth rate
3. Detect leaks: blocks allocated > N seconds ago and still live
4. Export metrics via a Prometheus-compatible endpoint (future) or log to stderr

### 6.3 Sampling for Production

Writing to the ring buffer on every allocation would add ~10-15ns overhead — acceptable in debug builds, but in production, use **sampling**:

```cpp
// Fast path: sample ~1% of allocations
thread_local uint64_t t_alloc_counter = 0;

void* allocate_fast_path(uint8_t size_class, ActorId owner) {
    void* ptr = slab_allocate(size_class);

    if (HPACTOR_UNLIKELY((++t_alloc_counter & 0x7F) == 0)) {  // 1/128 sampling
        AllocationEvent evt = {.timestamp = rdtsc(), .actor_id = owner.id(), ...};
        telemetry_ring_buffer.try_push(evt);
    }

    return ptr;
}
```

The sampling rate is configurable via `HPACTOR_MEMORY_SAMPLE_RATE` (0 = no sampling, 1 = every alloc, N = 1/N).

---

## 7. Debugging: Poisoning and Guard Pages

### 7.1 Memory Poisoning

When a block is freed, its user data region is overwritten with a poison pattern:

```cpp
static constexpr uint8_t kPoisonByte = 0xAA;
static constexpr uint32_t kPoisonPattern32 = 0xAAAAAAAA;
static constexpr uint64_t kPoisonPattern64 = 0xAAAAAAAAAAAAAAAA;
```

**Poisoning on free:**

```
deallocate(ptr):
    header = AllocHeader::from_user_data(ptr)
    user_size = header->block_size() - sizeof(AllocHeader) - sizeof(CanaryFooter)
    memset(ptr, kPoisonByte, user_size)    // poison user data
    header->magic = kFreedMagic             // poison header magic
    footer->magic = kFreedMagic             // poison footer magic
    freelist.push(header)
```

**Detection on allocate:**
Before returning a recycled block from the freelist, in debug builds, verify it still contains the poison pattern:

```cpp
if constexpr (kDebugBuild) {
    if (*reinterpret_cast<uint32_t*>(user_data) != kPoisonPattern32) {
        // Block was written to after free — use-after-free detected
        telemetry_ring_buffer.try_push({.event_type = kCorruptionUseAfterFree, ...});
        HPACTOR_ASSERT(!"use-after-free detected");
    }
}
```

### 7.2 Canary Verification

The `CanaryFooter` at the end of each block contains a magic value. On `free()`, the allocator verifies it:

```cpp
bool verify_canary(const AllocHeader* header) {
    auto* footer = CanaryFooter::from_header(header);
    if (footer->magic != kAllocMagic) {
        // Buffer overflow — user data wrote past the block boundary
        return false;
    }
    // Optional: verify checksum of user data
    return true;
}
```

### 7.3 Guard Pages

For fat blocks (> 4KB) and hibernation buffers, the allocator places **guard pages** (PROT_NONE) at both ends:

```
┌──────────────────────┐
│  Guard Page (4KB)    │  ← mprotect(PROT_NONE)
├──────────────────────┤
│  AllocHeader          │
│  User Data            │
│  ...                  │
│  CanaryFooter         │
├──────────────────────┤
│  Guard Page (4KB)    │  ← mprotect(PROT_NONE)
└──────────────────────┘
```

A buffer overflow or underflow hits the guard page → SIGSEGV → caught by the signal handler → actor terminated with a corruption error, rather than silent data corruption.

Guard pages are allocated by over-allocating the mmap region by 8KB and calling `mprotect` on the first and last pages.

**Signal handler integration:**

```cpp
void memory_corruption_signal_handler(int sig, siginfo_t* info, void* ctx) {
    void* fault_addr = info->si_addr;
    // Look up which actor's block contains this address
    auto* segment = SegmentProvider::instance().find_segment(fault_addr);
    if (segment) {
        auto* header = AllocHeader::find_in_segment(segment, fault_addr);
        if (header) {
            telemetry_ring_buffer.try_push({
                .actor_id = header->owner_id,
                .event_type = kCorruptionGuardPage,
                .block_size = header->block_size()
            });
            // Terminate the actor, not the process
            scheduler().terminate_actor(header->owner_id, ExitReason::kMemoryCorruption);
            return;
        }
    }
    // Not our memory — chain to default handler
    // ...
}
```

---

## 8. Hibernation and Cold Storage

### 8.1 Lifecycle Integration

A new actor state, `kHibernating`, is added to `ActorState`:

```cpp
class ActorState {
public:
    static constexpr uint32_t kIdle        = 0x01;
    static constexpr uint32_t kReady       = 0x02;
    static constexpr uint32_t kRunning     = 0x04;
    static constexpr uint32_t kIOWaiting   = 0x08;
    static constexpr uint32_t kHibernating = 0x20;   // NEW
    static constexpr uint32_t kTerminated  = 0x10;
    // ...
};
```

State transitions for hibernation:

```
Running ──► Hibernating ──► Idle (on reactivation)
  ▲                              │
  └──────────────────────────────┘
        (via hibernation)
```

### 8.2 Hibernation Trigger

An actor enters hibernation when:
1. **Idle timeout:** The actor has been in `kIdle` state for longer than `hibernate_after_idle_ms` (configurable per-actor, default 5 minutes).
2. **Memory pressure:** The kActor region exceeds its high water mark, and the hibernation manager selects the least-recently-active actors to hibernate.
3. **Explicit request:** The actor calls `context()->hibernate()` to serialize itself.

### 8.3 Hibernation Protocol

```
hibernate_actor(actor_id):
    actor = scheduler().find_actor(actor_id)
    if !actor.state.cas(kIdle, kHibernating):
        return BUSY  // actor transitioned, retry later

    // 1. Serialize actor state into hibernation buffer
    buffer = region(kHibernate).allocate(actor.serialized_size())
    actor.serialize_to(buffer)

    // 2. Release actor's hot memory
    actor.release_all_hot_memory()   // frees back to kActor region slabs

    // 3. Register hibernation buffer
    hibernate_registry.store(actor_id, buffer)

    // 4. Hint the kernel: these pages are cold
    madvise(buffer.ptr, buffer.size, MADV_COLD)
    // Optionally: madvise(MADV_PAGEOUT) to immediately push to ZRAM

    // 5. Actor is now hibernated — scheduler skips it
```

### 8.4 Reactivation Protocol

```
activate_actor(actor_id):
    // 1. Retrieve hibernation buffer
    buffer = hibernate_registry.load(actor_id)
    if !buffer: return NOT_FOUND

    // 2. Hint the kernel: we need these pages now
    madvise(buffer.ptr, buffer.size, MADV_WILLNEED)  // prefetch from ZRAM

    // 3. Allocate new hot memory for the actor
    hot_memory = region(kActor).allocate(actor.size_class)

    // 4. Deserialize into hot memory
    actor = Actor::deserialize_from(buffer, hot_memory)

    // 5. Release hibernation buffer
    region(kHibernate).deallocate(buffer)

    // 6. Transition actor state and reschedule
    actor.state.set(kIdle)
    scheduler().notify_ready(actor_id)
```

### 8.5 Hibernation Registry

The hibernation registry maps `ActorId → HibernationBuffer` for all hibernated actors:

```cpp
// include/hpactor/mem/hibernation_registry.hpp

struct HibernationBuffer {
    void* ptr;           // mmap'd buffer (serialized state)
    size_t size;         // serialized size
    uint64_t hibernated_at_ts;  // rdtsc at hibernation
    uint32_t actor_id;
    uint8_t  compression_hint; // 0=none, 1=lz4, 2=zstd
};

class HibernationRegistry {
public:
    // Store a hibernated actor's buffer
    void store(ActorId id, HibernationBuffer buf);

    // Retrieve and remove (on reactivation)
    HibernationBuffer load(ActorId id);

    // Find the least-recently-hibernated actors for eviction under pressure
    std::vector<ActorId> oldest_actors(size_t count) const;

    // Total hibernated bytes
    size_t total_hibernated_bytes() const;

private:
    // Concurrent hash map: ActorId → HibernationBuffer
    // Lock-free reads, fine-grained locking on writes
    ConcurrentHashMap<ActorId, HibernationBuffer> entries_;
};
```

### 8.6 Serialization Contract

Every hibernatable actor must implement the `Hibernatable` interface:

```cpp
// include/hpactor/mem/hibernatable.hpp

class Hibernatable {
public:
    // Return the serialized size of this actor's state.
    // Must be constant for the actor's lifetime.
    virtual size_t serialized_size() const = 0;

    // Serialize actor state into the provided buffer.
    // Buffer is guaranteed to be at least serialized_size() bytes.
    virtual void serialize_to(std::span<std::byte> buffer) const = 0;

    // Deserialize actor state from the provided buffer.
    // Called after the actor's hot memory has been allocated.
    virtual void deserialize_from(std::span<const std::byte> buffer) = 0;
};
```

For `StatefulActor<T>`, `serialized_size()` is `sizeof(T)` and serialization is a `memcpy`. For actors with dynamic state (vectors, strings), the actor must implement custom serialization.

---

## 9. Memory Compression via ZRAM

### 9.1 How ZRAM Works with HPActor

ZRAM is a Linux kernel feature that creates a compressed block device in RAM. When the kernel swaps pages to ZRAM, they are compressed inline (using lz4, lzo, or zstd) and stored in a portion of DRAM — no actual disk I/O occurs.

```
Conventional swap:   DRAM → Disk (ms latency)
ZRAM swap:           DRAM → Compress → DRAM (μs latency, 2-4× compression ratio)
```

### 9.2 The ZRAM Pipeline

```
Actor transitions to Hibernating:
  1. Serialize state → HibernationBuffer (mmap'd, MAP_ANONYMOUS)
  2. madvise(buffer, MADV_COLD)       ← hint: "these pages are cold"
  3. madvise(buffer, MADV_PAGEOUT)    ← force: "reclaim these pages NOW"
  4. Linux page reclaim scans cold anonymous pages
  5. Pages sent to ZRAM swap device
  6. ZRAM compresses pages inline (lz4/zstd)
  7. Physical DRAM freed for active actors

Actor reactivated:
  1. madvise(buffer, MADV_WILLNEED)   ← hint: "prefetch these pages"
  2. Linux faults pages back from ZRAM → decompress → DRAM
  3. Deserialize actor state → hot memory
  4. Hibernation buffer freed
```

### 9.3 Expected Compression Ratios

Actor state tends to be highly compressible — many repeated patterns, zero-initialized structs, and protobuf wire format overhead:

| State Type | Expected Ratio | Rationale |
|------------|---------------|-----------|
| Default-constructed state | 10:1+ | Mostly zeros |
| Protobuf messages | 3:1 — 5:1 | Tag + length prefixes, repeated fields |
| String-heavy state | 2:1 — 4:1 | lz4/zstd handles text well |
| Numeric state (arrays of ints) | 1.5:1 — 2:1 | Less compressible |
| Average | 3:1 — 4:1 | Conservatively: 3:1 |

At 3:1 compression, 1M hibernated actors each with 2KB state occupy:
- Raw: 2 GB
- After ZRAM: ~670 MB compressed + ~100 MB metadata → ~800 MB total

### 9.4 System Configuration

Recommended ZRAM setup for HPActor deployments:

```bash
# Create a zram device with zstd compression
echo zstd > /sys/block/zram0/comp_algorithm
echo 4G > /sys/block/zram0/disksize

# Enable as swap
mkswap /dev/zram0
swapon /dev/zram0 --priority 100  # higher priority than disk swap
```

The allocator detects ZRAM availability at startup:

```cpp
bool HibernationManager::is_zram_available() {
    // Check /sys/block/zram0/comp_algorithm for supported algorithms
    // Prefer zstd, fall back to lz4, then lzo
    // Returns false on non-Linux or if ZRAM is not configured
}
```

On macOS (where ZRAM is not available in the same way), the hibernation system uses `MADV_FREE` or simple `madvise(MADV_COLD)` (10.15+) and accepts the lower compression ratio.

---

## 10. Determinism and Fragmentation Control

**Evolution note:** Compaction is currently the sole fragmentation recovery
mechanism. MEM-002 (free block coalescing) adds immediate adjacent-free merging
on every `deallocate()`, reducing compaction frequency by 60–80%. The two
mechanisms are complementary: coalescing handles steady-state churn; compaction
handles the remaining long-tail fragmentation. See
`docs/superpowers/specs/2026-06-20-mem-02-free-block-coalescing-design.md`.

### 10.1 The Relocation Advantage

Because actors are referenced by **ActorId** (an index), not raw pointers, the allocator can move an actor's hot memory without updating every reference:

```cpp
// Wrong (vulnerable to fragmentation):
Actor* actor = new Actor(...);  // everyone holds raw Actor*

// Right (relocatable):
ActorId id = system.spawn<MyActor>(...);  // everyone holds an ID
// The actor registry maps id → current memory location
```

### 10.2 Handle-Based Addressing

The existing `ActorRef` (variant of `Actor` / `ActorProxy`) already provides indirection. The memory system extends this by ensuring every reference to actor-owned memory goes through the actor registry indirection:

```
User code          → ActorRef → ActorRegistry → Actor* (current location)
                                                      │
                                        relocatable: update this pointer
                                        when slab is compacted
```

### 10.3 Compaction Scheduling

Compaction runs as a low-priority background task on the scheduler:

```cpp
// In HybridScheduler::worker_loop():
if (should_compact()) {
    compact_fragmented_slabs();
}

bool should_compact() {
    // Run compaction when:
    // 1. No ready actors in the local queue (idle moment)
    // 2. Last compaction was > 60 seconds ago
    // 3. At least one slab is below 25% utilization
    return idle && (now - last_compaction > 60s) && fragmented_slabs_exist();
}
```

### 10.4 Fragmentation Budget

The allocator tracks a "fragmentation budget" — the total bytes wasted due to internal fragmentation (partial slab utilization):

```cpp
size_t FragmentationTracker::total_waste_bytes() const {
    size_t waste = 0;
    for (auto& slab : all_slabs()) {
        float utilization = slab.live_count / slab.total_blocks;
        waste += slab.size * slab.total_blocks * (1.0f - utilization);
    }
    return waste;
}
```

If fragmentation exceeds 5% of total allocated memory, compaction is triggered aggressively (even during non-idle periods, but limited to one slab per scheduling quantum to avoid latency spikes).

---

## 11. Integration with Existing Code

### 11.1 Integration Status — ✅ Complete

All integration points described below are implemented and deployed. The table
shows the design intent and the realized implementation:

| Existing Component | Integration | Status |
|--------------------|------------|--------|
| `CoroutineFramePool` | Uses `mem::allocate()`/`mem::deallocate()` from `kCoroutine` region via `SlabAllocated<Derived>` CRTP | ✅ |
| `MPSCActorMailbox` / `MultiLaneQueue` | Mailbox envelopes use `ObjectPool<Envelope>` backed by slab allocator; messages use `kMessage` region | ✅ |
| `ActorSystem::spawn()` | Actor instances allocated from `kActor` region via `SlabAllocated<Derived>`; hibernation supported | ✅ |
| `ActorState` | `kHibernating` state (0x20) integrated into state machine | ✅ |
| `HybridScheduler::execute_actor()` | Skips hibernated actors; handles reactivation via `HibernationRegistry` | ✅ |
| `ActorContext` | `hibernate()` method; memory pressure callbacks via `MemoryRegionRegistry` | ✅ |
| `EventBasedActor` | `Hibernatable` interface support via `StatefulActor<T>` serialization | ✅ |
| `CMakeLists.txt` | `ENABLE_MEMORY_TRACKING` (default ON), `ENABLE_MEMORY_DEBUG` (default OFF) | ✅ |
| `config.hpp.in` | `HPACTOR_ENABLE_MEMORY_TRACKING`, `HPACTOR_ENABLE_MEMORY_DEBUG` defines | ✅ |
| `FaultController` | 6 FAULT_INJECT sites in allocator paths: oom, freelist.pop.corrupt, region.try_reserve.fail, region.record_free.skip, slab_cache.refill_fail, segment.mmap_fail | ✅ |
| `MemoryTracker` | Per-actor atomic shadow counters (1M capacity, flat array, O(1) index) | ✅ |
| `MemoryTelemetry` | Sampled (1/128) MPSC ring buffer, `AllocationEvent` streaming | ✅ |

### 11.2 What Stays the Same

- `ActorId`, `ActorRef`, `ActorAddress` — no changes to addressing scheme
- `MessageVariant`, `TypedMessage<T>` — allocations routed through custom allocator but interface unchanged
- `Behavior`, `TypedBehavior` — no changes
- `Supervision` — allocator failures become supervision events (spawn rejection → supervisor handles it)
- All network code — network buffers use kNetwork region but interface is identical

### 11.3 Directory Structure (Current)

```
include/hpactor/mem/           (16 public headers)
    alloc_header.hpp               # AllocHeader (32B), CanaryFooter (8B), SizeClass
    compaction.hpp                 # CompactionManager, fragmentation tracking
    freelist.hpp                   # Re-export of adt::FreeList
    guard_page.hpp                 # Guarded allocation, SIGSEGV handler
    hibernatable.hpp               # Hibernatable interface
    hibernation_registry.hpp       # HibernationRegistry (ActorId → buffer)
    memory_config.hpp              # Compile-time flags, global allocate()/deallocate()
    memory_region.hpp              # RegionType enum, MemoryRegionRegistry
    memory_telemetry.hpp           # Sampled MPSC telemetry ring buffer
    memory_tracker.hpp             # Per-actor atomic shadow counters (1M capacity)
    object_pool.hpp                # ObjectPool<T,N> for envelope reuse
    segment_provider.hpp           # Tier 0: mmap segment carving, slab records
    size_class.hpp                 # SizeClass constants, block_size()/user_size()
    slab_cache.hpp                 # Tier 1: SlabCache (bump + freelist)
    std_allocator.hpp              # MemStdAllocator, MemUniquePtr, SlabAllocated<>
    thread_local_allocator.hpp     # ThreadLocalAllocator (6×8 SlabCache matrix)
    zram.hpp                       # ZramManager (platform madvise hints)

src/mem/                        (11 source files)
    compaction.cpp                 # CompactionManager implementation
    guard_page.cpp                 # Guard page allocation + SIGSEGV handler
    hibernation_manager.cpp        # HibernationRegistry store/load
    memory_config.cpp              # Global allocate()/deallocate() admission
    memory_region.cpp              # MemoryRegionRegistry CAS-based reservation
    memory_telemetry.cpp           # Telemetry ring buffer drain
    memory_tracker.cpp             # Per-actor tracking
    segment_provider.cpp           # SegmentProvider mmap/munmap
    slab_cache.cpp                 # SlabCache allocate/deallocate/refill
    thread_local_allocator.cpp     # TLA cache matrix init/routing
    zram.cpp                       # Platform madvise dispatch

tests/unit/mem/                 (19 test files)
    test_alloc_header.cpp, test_allocator_benchmark.cpp,
    test_compaction.cpp, test_freelist.cpp, test_guard_page.cpp,
    test_hibernation.cpp, test_mem_branches.cpp,
    test_memory_poisoning.cpp, test_memory_region_accounting.cpp,
    test_memory_stress.cpp, test_memory_tracker.cpp,
    test_object_pool.cpp, test_segment_provider.cpp,
    test_size_class.cpp, test_slab_cache.cpp,
    test_std_allocator.cpp, test_telemetry_ring_buffer.cpp,
    test_thread_local_allocator.cpp
```

### 11.4 API Summary (Current)

```cpp
// Allocate typed memory for an actor (via thread-local allocator)
void* ptr = hpactor::mem::allocate(
    hpactor::mem::RegionType::kMessage,
    size,
    actor_id
);

// Free (routes to origin SlabCache via SegmentProvider::lookup_slab)
hpactor::mem::deallocate(ptr);

// Convenience: allocate by size class index
void* ptr = hpactor::mem::allocate_class(
    hpactor::mem::RegionType::kCoroutine,
    size_class_idx,
    actor_id
);

// Hibernate an actor from within
context()->hibernate();

// Query per-actor memory
auto stats = hpactor::mem::MemoryTracker::instance().snapshot(actor_id);
std::cout << "Actor " << actor_id
          << " current: " << stats.current_bytes
          << " peak: " << stats.peak_bytes << " bytes\n";

// CRTP integration: any class can inherit SlabAllocated<Derived>
// to automatically route operator new/delete through the slab allocator
class MyActor : public EventBasedActor,
                public hpactor::mem::SlabAllocated<MyActor> {
    // operator new/delete automatically use mem::allocate()/deallocate()
};

// STL container with slab allocator
std::vector<int, hpactor::mem::MemStdAllocator<int>> vec;
```

---

## 12. Implementation Phases — ✅ All Complete

All 8 implementation phases (M1–M8) are complete as of 2026-05-31. The table
below captures the original plan with actual implementation notes.

### Phase M1: Platform and Config Foundation ✅
- **Original plan:** Platform header, CMake options, AllocHeader/CanaryFooter, size class table, RegionType enum.
- **Implemented:** `AllocHeader` (32B, 8 fields), `CanaryFooter` (8B), 8 size classes (32B–4KB), 6 region types (`kActor`, `kMessage`, `kCoroutine`, `kNetwork`, `kInternal`, `kHibernate`), `ENABLE_MEMORY_TRACKING` + `ENABLE_MEMORY_DEBUG` CMake options.
- **Key files:** `alloc_header.hpp`, `size_class.hpp`, `memory_region.hpp`, `memory_config.hpp`.

### Phase M2: Two-Tier Slab Allocator ✅
- **Original plan:** SegmentProvider, ThreadLocalSlabCache, TypedRegion, WorkerThread wiring.
- **Implemented:** `SegmentProvider` (2MB mmap, mutex-protected, ref-counted, slab_records_ map for cross-thread free routing), `SlabCache` (bump + CAS LIFO freelist, canary verification, poison detection), `ThreadLocalAllocator` (6×8 matrix of `SlabCache` instances), `WorkerThread` integration.
- **Key files:** `segment_provider.cpp`, `slab_cache.cpp`, `thread_local_allocator.cpp`.
- **Tests:** 19 unit test files, including `test_memory_stress` (8-thread concurrent), `test_slab_cache` (1000-block recyclability).

### Phase M3: Integrate with Existing Code ✅
- **Original plan:** Refactor CoroutineFramePool, MPSCActorMailbox, ActorSystem::spawn().
- **Implemented:** `SlabAllocated<Derived>` CRTP base for automatic `operator new`/`delete` routing; `MemStdAllocator<T>` STL adapter; `MemUniquePtr<T>`; `ObjectPool<T,N>` for mailbox envelope reuse; `MultiLaneQueue` integration.
- **Key files:** `std_allocator.hpp`, `object_pool.hpp`.
- **Tests:** All 65 original framework tests pass with custom allocator enabled.

### Phase M4: Observability ✅
- **Original plan:** MemoryTracker, TelemetryRingBuffer, background thread, sampling.
- **Implemented:** `MemoryTracker` (flat atomic array, 1M actor capacity, O(1) index, per-actor peak/current/allocs/frees), `MemoryTelemetry` (1/128 sampling, MPSC ring buffer, 65536 event capacity, single-consumer drain), allocation events with timestamp/actor_id/size_class/region_type.
- **Key files:** `memory_tracker.cpp`, `memory_telemetry.cpp`.
- **CLI:** `/memory regions` command for per-region snapshot access.

### Phase M5: Debugging Features ✅
- **Original plan:** Poisoning, canary verification, guard pages, signal handler.
- **Implemented:** Memory poisoning (0xAA on free, verified on recycle), canary verification (8B footer, magic check on free), guard pages for blocks >4KB (PROT_NONE at both ends), SIGSEGV/SIGBUS handler with `write()`-only signal-safe logging, SegmentProvider lookup for fault address identification.
- **Key files:** `guard_page.cpp` (signal handler), `slab_cache.cpp` (poison + canary checks).
- **Tests:** `test_memory_poisoning` (canary overflow detection, 100-cycle recyclability), `test_guard_page`.

### Phase M6: Hibernation ✅
- **Original plan:** kHibernating state, Hibernatable interface, serialization protocol, registry, triggers.
- **Implemented:** `kHibernating` state in `ActorState`, `Hibernatable` interface (`serialized_size()`, `serialize_to()`, `deserialize_from()`), `HibernationRegistry` (mutex-guarded unordered_map, store/load/remove), idle-timeout and memory-pressure triggers, `MADV_COLD`/`MADV_PAGEOUT` hints.
- **Key files:** `hibernation_registry.hpp`, `hibernatable.hpp`, `hibernation_manager.cpp`.
- **Tests:** `test_hibernation` (store/load cycle, serialization round-trip).

### Phase M7: Compaction ✅
- **Original plan:** Slab generation tracking, live block counting, actor relocation, background scheduling.
- **Implemented:** `CompactionManager` (25% utilization threshold, 5% fragmentation budget, 60s interval), `SlabCompactionInfo` (live_count, total_blocks, generation), `WasteReport` (wasted_bytes, total_bytes, waste_ratio), background compaction via scheduler idle moments.
- **Key files:** `compaction.hpp`, `compaction.cpp`.
- **Tests:** `test_compaction` (threshold logic, waste computation, timing interval).

### Phase M8: ZRAM Integration and Tuning ✅
- **Original plan:** ZRAM detection, MADV_PAGEOUT, compression ratio tracking, performance tuning.
- **Implemented:** `ZramManager` (Linux: `MADV_PAGEOUT`/`MADV_COLD`/`MADV_WILLNEED`; macOS: `MADV_FREE` fallback), ZRAM detection via `/sys/block/zram0/comp_algorithm`.
- **Key files:** `zram.hpp`, `zram.cpp`.

---

## 13. Evolution Roadmap

An Erlang BEAM `erts_alloc` gap analysis (issue #339, June 2026) identified
9 architectural gaps between HPActor's current implementation and
production-hardened memory systems. The prioritized optimization roadmap is
organized in 4 phases.

### 13.1 Companion Documents

| Document | Purpose |
|----------|---------|
| `docs/architecture/memory/memory-management-erlang-gap-analysis.md` | Full Erlang comparison, 9 gaps, 4-phase roadmap, metric targets, risks |
| `docs/superpowers/specs/2026-06-20-mem-01-segregated-free-lists-design.md` | Phase 1.1 (P0): Segregated free lists for better locality |
| `docs/superpowers/specs/2026-06-20-mem-02-free-block-coalescing-design.md` | Phase 1.2 (P0): Boundary tags + immediate coalescing |
| `docs/superpowers/specs/2026-06-20-mem-03-per-region-strategy-design.md` | Phase 1.3 (P1): Per-RegionType allocation strategy selection |
| `docs/superpowers/specs/2026-06-20-mem-04-super-carrier-design.md` | Phase 2.1 (P1): Contiguous virtual memory reservation |
| `docs/superpowers/specs/2026-06-20-mem-05-huge-pages-design.md` | Phase 2.2 (P1): MAP_HUGETLB with THP fallback |
| `docs/superpowers/specs/2026-06-20-mem-06-message-inlining-design.md` | Phase 2.3 (P1): Inline payloads ≤32B in mailbox envelopes |

### 13.2 Implementation Dependency Tree

```
Phase 1: Foundation (P0/P1)
  MEM-001 Segregated Free Lists ─────────────────────┐
  MEM-002 Free Block Coalescing ──── (depends: 001)   │
  MEM-003 Per-Region Strategies ──── (depends: 002)   │
                                                       │
Phase 2: Scale (P1)                                     │
  MEM-004 Super Carrier ───────────────────────────────┤
  MEM-005 Huge Pages ─────────────── (depends: 004)    │
  MEM-006 Message Inlining ────────────────────────────┘
```

### 13.3 Key Metric Targets

| Metric | Current | Target (post Phase 1–2) |
|--------|---------|--------------------------|
| `allocate()` hot path (freelist) | <25 ns | <20 ns (segregated) |
| `deallocate()` hot path | <20 ns | <25 ns (+coalescing check) |
| Internal fragmentation (7-day uptime) | <5% (compaction) | <2% (coalescing) |
| Compaction cycles / day | ~2–4 | 0–1 |
| TLB misses / M allocations | ~500 | ~50 (super carrier + huge pages) |
| `mmap`/`munmap` syscalls / hour | 100s–1000s | ~1–2 |
| Message throughput (tiny msgs) | baseline | +20–40% (inlining) |

---

## Appendix A: Performance Targets

| Metric | Target | Measurement | Status |
|--------|--------|-------------|--------|
| `allocate()` hot path (thread-local bump) | < 10 ns | `rdtsc` delta, p50 | ✅ Verified |
| `allocate()` hot path (freelist pop) | < 25 ns | `rdtsc` delta, p50 | ✅ Verified |
| `deallocate()` hot path | < 20 ns | `rdtsc` delta, p50 | ✅ Verified |
| SegmentProvider::acquire_slab() | < 10 μs | Amortized, < 0.1% hit rate | ✅ Design target |
| `free()` canary verification | < 5 ns | Memcmp 4 bytes | ✅ Verified |
| Hibernate actor (2KB state) | < 50 μs | Serialize + madvise | ✅ Design target |
| Reactivate actor (2KB state, ZRAM hit) | < 500 μs | madvise + deserialize | ✅ Design target |
| Reactivate actor (2KB state, ZRAM miss) | < 2 ms | Page fault from disk swap | ✅ Design target |
| Telemetry ring buffer push | < 15 ns | When sampled (1/128 rate) | ✅ Design target |
| Compaction per-slab (50% utilized) | < 1 ms | Stop-the-world per slab | ✅ Design target |

**Evolution targets (post Phase 1–2):** See `docs/architecture/memory/memory-management-erlang-gap-analysis.md` Section 7 for Phase 1–2 metric targets including reduced fragmentation (<2%), reduced TLB misses (-90%), and elimination of per-segment mmap/munmap syscalls (-99%).

## Appendix B: Configuration Knobs

```cpp
// Compile-time (CMake)
#define HPACTOR_ENABLE_MEMORY_TRACKING   // enable shadow counters + telemetry (default ON)
#define HPACTOR_ENABLE_MEMORY_DEBUG      // enable poisoning + canaries + guard pages (default OFF)
#define HPACTOR_ENABLE_FAULT_INJECTION   // enable allocator fault injection sites (default ON)
#define HPACTOR_MEMORY_SAMPLE_RATE 128   // 1/N allocations logged to telemetry (compile-time default)
#define HPACTOR_MEMORY_SEGMENT_SIZE (2 * 1024 * 1024)  // 2MB segments
#define HPACTOR_SUPER_CARRIER_ENABLED    // enable super carrier (__LP64__ only, future: MEM-004)

// Run-time (TOML — via self-registering parsers in src/config/parsers/)
// [system.memory]
// tracking_enabled = true
// debug_enabled = false
// sample_rate = 128

// [system.memory.regions.actor]
// hard_limit_mb = 2048
//
// [system.memory.regions.message]
// hard_limit_mb = 4096
//
// [system.memory.hibernation]
// hibernate_after_idle_ms = 300000
//
// [system.memory.compaction]
// threshold = 0.25
// fragmentation_budget = 0.05
// interval_seconds = 60
//
// [system.memory.zram]
// enabled = true
// algorithm = "zstd"
//
// Future (MEM-003, MEM-004):
// [system.memory.regions.actor]
// strategy = "segregated_fit"     # "cas_lifo" | "bump_only" | "segregated_fit"
// coalescing = true
//
// [system.memory.carrier]
// enabled = true
// size_gb = 8
// max_size_gb = 64
```
