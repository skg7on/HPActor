# HPActor Memory Management — Erlang BEAM Gap Analysis & Evolution Roadmap

**Date:** 2026-06-20
**Status:** Design Reference
**Scope:** Gap analysis between HPActor current implementation and Erlang BEAM `erts_alloc`, prioritized optimization roadmap
**Issue:** #339
**Parent Doc:** `docs/architecture/memory/memory-management-architecture-design.md`

## Table of Contents

1. [Purpose and Scope](#1-purpose-and-scope)
2. [Architecture Comparison](#2-architecture-comparison)
3. [Key Architectural Gaps](#3-key-architectural-gaps)
4. [HPActor Strengths](#4-hpactor-strengths)
5. [Prioritized Optimization Roadmap](#5-prioritized-optimization-roadmap)
6. [Implementation Sequence](#6-implementation-sequence)
7. [Metric Targets](#7-metric-targets)
8. [Risks and Mitigations](#8-risks-and-mitigations)
9. [Reference Documents](#9-reference-documents)

---

## 1. Purpose and Scope

### 1.1 Purpose

This document is the **whole-picture reference** comparing HPActor's current
memory management implementation against Erlang/BEAM's production-hardened
`erts_alloc` architecture. It captures the full analysis, identifies
architectural gaps, acknowledges existing strengths, and provides a prioritized
optimization roadmap for the next evolution of HPActor's memory subsystem.

Each P0/P1 gap identified here has a corresponding detailed design spec in
`docs/superpowers/specs/`. This document provides the architectural context
those specs depend on.

### 1.2 Scope

- **In scope:** Slab allocator hot path, free-list strategy, fragmentation
  control, carrier/virtual-memory management, per-region allocation policy,
  NUMA/huge-page awareness, message-path allocation overhead.
- **Out of scope:** GC/semi-space copying (HPActor is explicitly manual-memory),
  hibernation improvements (already production-grade), memory debugging/poisoning
  (already production-grade), per-actor tracking (already exceeds Erlang).

### 1.3 Current State Summary

HPActor's memory subsystem (M1–M8, completed 2026-05-03) provides:

| Layer | Component | Status |
|-------|-----------|--------|
| Foundation | `AllocHeader` (32B) + `CanaryFooter` (8B), 8 size classes (32B–4KB), 6 region types | ✅ Production |
| Tier 0 | `SegmentProvider` — 2MB `mmap` on demand, mutex-protected, per-slab ref-counting | ✅ Production |
| Tier 1 | `ThreadLocalAllocator` — per-thread `SlabCache` matrix (6×8), bump + CAS LIFO freelist | ✅ Production |
| Accounting | `MemoryRegionRegistry` — lock-free per-region counters, `try_reserve()` admission | ✅ Production |
| Tracking | `MemoryTracker` — 1M-actor flat atomic array, per-actor peak/current tracking | ✅ Production |
| Telemetry | `MemoryTelemetry` — sampled (1/128) MPSC ring buffer, allocation event streaming | ✅ Production |
| Debug | Memory poisoning (0xAA), canary verification, guard pages, SIGSEGV handler | ✅ Production |
| Hibernation | `HibernationRegistry`, `Hibernatable` interface, `ZramManager` (MADV_PAGEOUT) | ✅ Production |
| Compaction | `CompactionManager` — 25% threshold, 5% fragmentation budget, actor relocation | ✅ Production |
| Fault injection | 6 FAULT_INJECT sites in allocator paths, seed-replayable | ✅ Production |

**Total:** 16 public headers, 11 source files, 19 unit test files.

---

## 2. Architecture Comparison

### 2.1 Side-by-Side

| Dimension | Erlang BEAM (`erts_alloc`) | HPActor (current) |
|-----------|---------------------------|-------------------|
| **Top-level allocator** | Super Carrier — single huge `mmap` reservation at VM startup (e.g., 64GB contiguous) | `SegmentProvider` — individual 2MB `mmap` on demand, mutex-protected |
| **Mid-level structure** | Multi-block carriers; progressive sizing from `smbcs` → `lmbcs` over `mbcgs` stages | Fixed-size slabs per size class (64KB–512KB) |
| **Allocator instances** | Per-scheduler instances sharing carrier pools; instances can have different strategies | Per-thread `ThreadLocalAllocator`; no sharing/migration between threads |
| **Hot-path allocation strategy** | **Good Fit (`gf`)** — segregated free lists, bounded search depth=3, O(1) | Bump allocator (virgin) → CAS LIFO freelist (recycled) |
| **Free block coalescing** | ✅ Yes — boundary tags (header + footer on all free blocks), immediate coalescing of adjacent frees | ❌ No — freed blocks pushed independently onto CAS freelist; adjacent frees remain separate |
| **Carrier migration** | ✅ Yes — abandoned carriers migrate between allocator instances via lock-free circular pool; employment tracking prevents use-after-free races | ❌ No — slabs permanently bound to creating `ThreadLocalAllocator` |
| **Typed allocation** | 12 allocator types (`eheap_alloc`, `binary_alloc`, `ets_alloc`, `sl_alloc`, `ll_alloc`, `fix_alloc`, `temp_alloc`, `std_alloc`, `driver_alloc`, `literal_alloc`, `sys_alloc`, `mseg_alloc`) | 6 region types (`kActor`, `kMessage`, `kCoroutine`, `kNetwork`, `kInternal`, `kHibernate`) |
| **Allocation strategy per type** | ✅ Per-allocator strategy selection: `bf`, `aobf`, `aoff`, `aoffcaobf`, `aoffcbf`, `gf`, `af` | ❌ Single strategy (bump + CAS freelist) for all regions |
| **GC / lifecycle model** | Per-process generational semi-space copying GC (Cheney's algorithm); per-process private heap; no stop-the-world | Manual explicit alloc/free; hibernation for cold storage; ActorId-based relocation for compaction |
| **Memory safety** | Immutable data → no write barriers needed for generational GC | Canary footers (8B), poison patterns (0xAA), guard pages (PROT_NONE), SIGSEGV→actor-termination signal handler |
| **Observability** | `erlang:memory()` + `instrument` module; per-process `process_info(Pid, memory)` | Per-actor atomic shadow counters (flat array, O(1)); MPSC telemetry ring buffer with configurable sampling |
| **NUMA awareness** | ✅ Yes — address-order strategies (`aoffcaobf`, `aoffcbf`) allocate from carriers at lowest address for NUMA locality | ❌ None |
| **Huge page support** | ✅ THP / explicit huge pages for super carrier (`+MMlp on`) | Design mentions `MAP_HUGETLB`; **not in current implementation** |
| **Fragmentation control** | Good Fit approximates best-fit + free block coalescing + carrier migration + abandonment | Slab compaction via actor relocation (leverages ActorId indirection); fragmentation budget tracking (5% target) |
| **Constants / literals** | ✅ `literal_alloc` — immutable terms never participate in GC, 1GB virtual region on 64-bit | ❌ No read-only region |
| **Message placement** | `message_queue_data` flag: `on_heap` (receiver's heap) vs `off_heap` (heap fragments) | Messages always independently allocated from `kMessage` region |
| **Large object path** | Single-block carriers for blocks > `sbct` threshold; refc binaries in shared binary heap | Fallback to `std::aligned_alloc` / `std::free` for blocks > 4KB |
| **Tiny object optimization** | `fix_alloc` — fast allocator for fixed-size data types | Design doc describes **tiny-block packed metadata** for 32B blocks (bitmap + owner array, reducing overhead from 125% → 25%); **not yet implemented** |

### 2.2 Architecture Diagrams

#### Erlang BEAM Allocator Hierarchy

```
┌──────────────────────────────────────────────────────────────────┐
│                        ERTS Memory Subsystem                      │
├──────────────────────────────────────────────────────────────────┤
│  eheap_alloc  binary_alloc  ets_alloc  sl_alloc  ll_alloc  ...  │
│       │            │            │          │         │            │
│       └────────────┴────────────┴──────────┴─────────┘            │
│                          │                                       │
│                   alloc_util Framework                           │
│            (carriers, blocks, boundary tags,                     │
│             segregated free lists, carrier pools)                │
│                          │                                       │
│              ┌───────────┴───────────┐                           │
│           mseg_alloc             sys_alloc                       │
│         (mmap segments)         (OS malloc)                      │
│               │                      │                           │
│    ┌──────────┴──────────┐    sbrk / heap                        │
│  Super Carrier    Regular mmap                                   │
│  (huge, contiguous virtual address space)                        │
│  [+MMsc <size>] [+MMlp on]                                      │
└──────────────────────────────────────────────────────────────────┘
```

#### HPActor Current Allocator Hierarchy

```
┌──────────────────────────────────────────────────────────────────┐
│                     HPActor Memory Subsystem                      │
├──────────────────────────────────────────────────────────────────┤
│  ThreadLocalAllocator (per WorkerThread)                         │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  RegionType × SizeClass matrix (6 × 8 = 48 SlabCache)      │  │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐      ┌──────────┐  │  │
│  │  │ kActor   │ │ kMessage │ │ kCoroutine│ ...  │kHibernate│  │  │
│  │  │  Slab[0] │ │  Slab[0] │ │  Slab[0]  │      │  Slab[0] │  │  │
│  │  │  Slab[1] │ │  Slab[1] │ │  Slab[1]  │      │  Slab[1] │  │  │
│  │  │  ...     │ │  ...     │ │  ...      │      │  ...     │  │  │
│  │  │  Slab[7] │ │  Slab[7] │ │  Slab[7]  │      │  Slab[7] │  │  │
│  │  └──────────┘ └──────────┘ └──────────┘      └──────────┘  │  │
│  │  Each Slab: bump ptr │ CAS LIFO freelist                    │  │
│  └────────────────────────────────────────────────────────────┘  │
│                          │                                       │
│                    SegmentProvider (Tier 0)                       │
│              2MB mmap regions, mutex-protected                    │
│              slab_records_ map for cross-thread free routing      │
│                          │                                       │
│                    mmap(MAP_PRIVATE | MAP_ANONYMOUS)              │
└──────────────────────────────────────────────────────────────────┘
```

#### HPActor Target Evolution (Post Phase 1–4)

```
┌──────────────────────────────────────────────────────────────────┐
│                HPActor Memory Subsystem (Target)                   │
├──────────────────────────────────────────────────────────────────┤
│  ThreadLocalAllocator (per WorkerThread)                         │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  Per-Region Strategy Selection                              │  │
│  │  ┌────────────┐ ┌────────────┐ ┌────────────┐              │  │
│  │  │ kMessage   │ │ kActor     │ │ kNetwork   │ ...           │  │
│  │  │ Bump-only  │ │ Good Fit   │ │ Bump-only  │              │  │
│  │  │ (no free-  │ │ + Coalesce │ │ (no free-  │              │  │
│  │  │  list)     │ │ + Seg List │ │  list)     │              │  │
│  │  └────────────┘ └────────────┘ └────────────┘              │  │
│  └────────────────────────────────────────────────────────────┘  │
│                          │                                       │
│              ┌───────────┴───────────┐                           │
│         Carrier Migration Pool       SegmentProvider             │
│    (lock-free circular pool,         (carves from super carrier) │
│     under-utilized slabs migrate)    │                           │
│                          │           │                           │
│                    Super Carrier                                 │
│         (huge contiguous virtual reservation, 8–64GB)            │
│         mmap(PROT_NONE, MAP_NORESERVE) at startup                │
│         mprotect(PROT_READ|PROT_WRITE) on carve                  │
│         madvise(MADV_FREE) + mprotect(PROT_NONE) on release      │
│         MAP_HUGETLB preferred, THP fallback                      │
└──────────────────────────────────────────────────────────────────┘
```

---

## 3. Key Architectural Gaps

### Gap 1 — No Super Carrier (P1, Phase 2.1)

Erlang reserves a single contiguous virtual address region (configurable via
`+MMscs`, e.g., 64GB) at VM startup. All carrier allocations carve from this
region:

- **TLB pressure:** A single hugepage-backed contiguous range → few TLB entries
  for allocator metadata.
- **Zero syscall overhead:** Slab acquire/release is just pointer arithmetic
  within the super carrier; no `mmap`/`munmap` per slab.
- **Virtual address stability:** Pointers remain within a predictable range,
  enabling pointer compression optimizations in the future.
- **NUMA-ready:** Can pre-partition the super carrier into per-NUMA-node
  sub-regions.

HPActor's `SegmentProvider` calls `mmap` for each 2MB segment individually — at
million-actor scale, this means potentially thousands of discontinuous mappings,
each consuming TLB entries.

### Gap 2 — No Free Block Coalescing (P0, Phase 1.2)

Erlang's boundary tags (header + footer on every free block) enable immediate
coalescing: when freeing block B, if neighbor A is free they merge into AB, and
if C is also free they merge into ABC. This is constant-time and prevents the
checkerboard fragmentation pattern.

HPActor pushes each freed block onto a CAS LIFO freelist. Adjacent freed blocks
remain separate. The consequence:

- Internal fragmentation within slabs grows monotonically between compaction
  cycles.
- Compaction (stop-the-world per slab, copy live blocks, update actor registry
  pointers) is the **sole** mechanism for recovering contiguous free space.
- Under steady-state churn, fragmentation oscillates rather than converging.

### Gap 3 — No Carrier Migration (P3, Phase 3.1)

Erlang's abandoned carrier pool allows under-utilized carriers to move between
scheduler-specific allocator instances via a lock-free circular doubly-linked
list. After a load spike subsides, carriers with ≤25% utilization are offered
to the shared pool where memory-starved allocator instances can claim them.

HPActor's slabs are permanently bound to their creating
`ThreadLocalAllocator`. Under skewed workloads:

- Thread 1 spawns 100K actors → its allocator holds many slabs.
- Thread 2 spawns none → its allocator holds minimal memory.
- When thread 1's actors terminate, those slabs go cold but cannot be
  reassigned.
- Thread 2 cannot benefit from thread 1's unused capacity.

### Gap 4 — Single Allocation Strategy for All Regions (P1, Phase 1.3)

All HPActor regions use identical bump + CAS freelist. Erlang allows
per-allocator strategy selection:

- `gf` (Good Fit) for general-purpose — constant-time, bounded search depth.
- `af` (A Fit) for `temp_alloc` — check only the first block, create new
  carrier otherwise (fastest).
- `aoffcaobf` for NUMA-sensitive workloads with carrier migration.
- `bf` (Best Fit) for fragmentation-sensitive workloads.

Different HPActor regions have radically different lifetime patterns:

| Region | Lifetime Pattern | Optimal Strategy | Rationale |
|--------|-----------------|------------------|-----------|
| `kMessage` | Microsecond-scale, high churn | Bump-only, no freelist | Freelist overhead wasted on µs-lived blocks; bump is 2× faster |
| `kActor` | Process-lifetime scale | Good Fit + coalescing | Fragmentation matters over 7-day uptime |
| `kCoroutine` | Mixed lifetimes | Bump + freelist (current) | Balanced approach is reasonable |
| `kNetwork` | I/O request lifetimes | Bump-only | Buffers match request lifetimes, low fragmentation risk |
| `kInternal` | Indefinite, low churn | Current (balanced default) | Low allocation volume |
| `kHibernate` | Infrequent alloc/free | Current strategy | Infrequent operations |

### Gap 5 — No Segregated Free Lists / Good Fit (P0, Phase 1.1)

Erlang's default `gf` strategy places free blocks in segregated size-range
lists. When allocating N bytes, only the corresponding list is searched, bounded
to depth 3. This approximates best-fit quality at constant cost.

HPActor's CAS freelist is pure LIFO — no size awareness. Since each `SlabCache`
handles one size class, there's partial mitigation (all blocks are the same
size), but the LIFO ordering means a block freed from position X will be
reallocated at position X regardless of whether a block at position Y (closer
to other live blocks) would yield better locality.

### Gap 6 — No Huge Page Support in Implementation (P1, Phase 2.2)

The design doc mentions `MAP_HUGETLB` but the current
`SegmentProvider::allocate_new_segment()` does not use it:

```cpp
void* base = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
```

At scale, TLB misses on allocator metadata access become a measurable cost.

### Gap 7 — No NUMA Awareness (P3, Phase 3.2)

All segments are allocated from whatever physical memory the kernel provides,
with no NUMA node preference. On multi-socket systems, a thread on socket 0 may
frequently access slabs backed by socket 1's memory.

### Gap 8 — No Message Inlining for Tiny Payloads (P1, Phase 2.3)

Every message, regardless of size, goes through the full allocation path:
admission check → region reserve → bump/freelist → header stamp → canary stamp.
For messages ≤ 32 bytes (ActorId-sized commands, simple signals), the allocator
overhead (40 bytes header+footer) exceeds the payload.

### Gap 9 — Tiny-Block Optimization from Design Not Implemented (P2, Phase 3.4)

The design doc Section 5.2 describes packing 32B block metadata out-of-band
(bitmap + owner array, reducing overhead from 125% to 25%). This is spec'd but
not built.

---

## 4. HPActor Strengths (Exceeds Erlang)

These areas represent HPActor advantages that should be preserved through all
optimizations:

| Capability | HPActor Advantage | Erlang Equivalent |
|------------|------------------|-------------------|
| **Hibernation + ZRAM** | Idle actors serialized → `MADV_PAGEOUT` → 3–4× memory compression | No equivalent; idle processes consume full heap in DRAM |
| **Per-actor atomic tracking** | Flat atomic array indexed by ActorId, O(1) lock-free | `erlang:memory(per_process)` requires walking process descriptors |
| **Deterministic fault injection** | 80 sites across 14 domains including OOM, freelist corruption, slab refill failure | No equivalent; memory failures are non-deterministic |
| **Canary + poison + guard pages** | Inline verification on alloc/free; use-after-free detection; SIGSEGV → actor termination | Immutability + GC for safety; no use-after-free detection needed |
| **Handle-based relocation** | ActorId indirection enables moving actors in memory without dangling pointers | Copying GC achieves similar safety but at runtime cost |
| **Per-region hard limits** | CAS-based admission control prevents OOM cascades per region type | Global memory limits only; no per-allocator-type hard caps with backpressure |
| **Explicit lifecycle** | No GC pauses — allocation cost is fully deterministic and predictable | GC pauses are per-process (bounded but non-zero); fullsweep can be costly |
| **Zero-copy ownership transfer** | `AllocHeader::owner_id` can be atomically reassigned to transfer message ownership | Deep copies messages between processes (mitigated by shared off-heap binaries) |
| **Compact metadata** | 32B AllocHeader + 8B CanaryFooter = 40B overhead per block | Per-block overhead varies by strategy; boundary tags add per-block cost |

---

## 5. Prioritized Optimization Roadmap

### Phase 1: High-Impact, Low-Risk (Weeks 1–4)

#### 1.1 Segregated Free Lists (Good Fit Strategy) — **P0**

Replace the single CAS LIFO freelist per `SlabCache` with segregated free
lists. Free blocks grouped into ~8 sub-ranges by address within the slab.
Allocation searches at most 3 sub-ranges.

**Expected impact:** 30–50% reduction in internal fragmentation under churn;
fewer compaction cycles.

**Design considerations:**
- Keep single CAS LIFO as compile-time option for latency-sensitive paths.
- Round-robin starting bin to avoid hot spots.
- Each bin is a CAS freelist (reuse existing `FreeList<T>`).

**Detailed spec:** `docs/superpowers/specs/2026-06-20-mem-01-segregated-free-lists-design.md`

#### 1.2 Free Block Coalescing with Boundary Tags — **P0**

Add boundary tag support: when a block is freed, check adjacent blocks and
coalesce if free. Repurpose the `CanaryFooter` slot on freed blocks to store
block size for the boundary tag, achieving zero additional memory overhead.

**Expected impact:** 60–80% reduction in compaction frequency; organic
fragmentation stays under 2%.

**Design constraint:** Must support removing a block from the *middle* of a
segregated free list (not just pop from front). The `FreeList<T>` may need to
be upgraded to a doubly-linked or hybrid structure, or coalescing can be
deferred to compaction rather than done inline.

**Detailed spec:** `docs/superpowers/specs/2026-06-20-mem-02-free-block-coalescing-design.md`

#### 1.3 Per-Region Allocation Strategy Selection — **P1**

Allow different strategies per `RegionType`:

| Region | Strategy | Rationale |
|--------|----------|-----------|
| `kMessage` | Pure bump allocation, no freelist | Messages are µs-lived |
| `kActor` | Good Fit with coalescing | Long-lived, fragmentation matters |
| `kCoroutine` | Bump + freelist (current) | Mixed lifetimes |
| `kInternal` | Current bump + freelist | Balanced default |
| `kNetwork` | Bump only | Buffer lifetimes match request lifetimes |
| `kHibernate` | Current strategy | Infrequent alloc/free |

**Detailed spec:** `docs/superpowers/specs/2026-06-20-mem-03-per-region-strategy-design.md`

### Phase 2: Structural Improvements (Weeks 5–10)

#### 2.1 Super Carrier — Contiguous Virtual Memory Reservation — **P1**

Reserve a large contiguous virtual address range at `ActorSystem` startup
(configurable, e.g., 8–64GB). All `SegmentProvider` slabs are carved from this
range.

```
Startup: mmap(PROT_NONE, MAP_NORESERVE) → huge virtual reservation
Slab carve: mprotect(slab, PROT_READ|PROT_WRITE) → commit pages on demand
Slab release: madvise(MADV_FREE) + mprotect(PROT_NONE) → decommit, keep virtual
```

**Expected impact:** 15–30% TLB miss reduction; elimination of runtime
`mmap`/`munmap` syscalls. Must be `#ifdef __LP64__` guarded with 32-bit
fallback.

**Detailed spec:** `docs/superpowers/specs/2026-06-20-mem-04-super-carrier-design.md`

#### 2.2 Huge Page Support — **P1**

Prefer `MAP_HUGETLB` for the super carrier with fallback to `MADV_HUGEPAGE`
(THP). At million-actor scale, TLB entries for slab memory dominate cache
pressure — a single 2MB huge page entry covers an entire segment.

**Detailed spec:** `docs/superpowers/specs/2026-06-20-mem-05-huge-pages-design.md`

#### 2.3 Message Inlining for Tiny Payloads — **P1**

Messages ≤ 32 bytes are inlined into the mailbox envelope, eliminating a
separate allocation. The `MultiLaneQueue` envelope gains an inline buffer in
a union with the external payload pointer.

**Expected impact:** 20–40% reduction in total allocations for message-heavy
workloads.

**Detailed spec:** `docs/superpowers/specs/2026-06-20-mem-06-message-inlining-design.md`

#### 2.4 Size-Class Rebalancing — **P2**

Background thread computes per-size-class pressure (EWMA of allocation rate)
and shifts capacity from cold to hot size classes within each region.

### Phase 3: Scale & Locality (Weeks 11–16)

#### 3.1 Carrier Migration Between Thread-Local Caches — **P3**

Lock-free circular pool of under-utilized slabs (<25% utilization). Threads offer
slabs to the pool; memory-starved threads fetch from the pool before requesting
from `SegmentProvider`.

#### 3.2 NUMA-Aware Slab Placement — **P3**

On NUMA systems, `SegmentProvider` tracks per-node carve offsets within the
super carrier. Slab requests from thread T on node N are satisfied from node
N's sub-region.

#### 3.3 Zero-Copy Message Ownership Transfer — **P2**

When Actor A sends a message to Actor B on the same node and the message
exceeds a size threshold, atomically update `AllocHeader::owner_id` from A → B
instead of copying.

#### 3.4 Tiny-Block Packed Metadata — **P2**

Implement design doc Section 5.2: for 32B blocks, store metadata out-of-band in
a per-slab bitmap + owner array, reducing overhead from 40B/block (125%) to
~2KB/slab (25%).

### Phase 4: Advanced Optimizations (Weeks 17–24)

#### 4.1 Constant / Read-Only Memory Region — **P4**

Add `kConstant` region for immutable data: TypeTag tables, serialized TOML
config, protobuf descriptor pools. Pages are `mprotect(PROT_READ)` after
initialization. Hardware-enforced immutability; can be shared across processes
via `MAP_SHARED`.

#### 4.2 Slab Prefetch / Speculative Refill — **P3**

When a slab reaches 75% capacity, asynchronously request the next slab from
`SegmentProvider`. The refill is ready before bump pointer exhaustion, moving
the `refill()` mutex acquisition off the critical allocation path.

#### 4.3 Actor-Affined Slab Assignment — **P4**

Assign dedicated slabs to top-N "hot" actors by allocation rate. Improves cache
locality — an actor's state and recent messages live in adjacent cache lines.
Cold actors share pooled slabs.

---

## 6. Implementation Sequence (Dependency-Ordered)

```
Phase 1: Foundation
  ├── 1.1 Segregated Free Lists ─────────────────────┐
  ├── 1.2 Free Block Coalescing ───── (depends: 1.1)  │
  └── 1.3 Per-Region Strategies ───── (depends: 1.2)  │
                                                       │
Phase 2: Scale                                          │
  ├── 2.1 Super Carrier ───────────────────────────────┤
  ├── 2.2 Huge Pages ──────────────── (depends: 2.1)   │
  ├── 2.3 Message Inlining ────────────────────────────┤
  └── 2.4 Size-Class Rebalancing ──── (depends: 1.1)   │
                                                       │
Phase 3: Locality & Efficiency                          │
  ├── 3.1 Carrier Migration ───────── (depends: 2.1)   │
  ├── 3.2 NUMA Placement ──────────── (depends: 2.1)   │
  ├── 3.3 Zero-Copy Transfer ──────────────────────────┤
  └── 3.4 Tiny-Block Metadata ─────────────────────────┤
                                                       │
Phase 4: Polish                                         │
  ├── 4.1 Constant Region ─────────────────────────────┤
  ├── 4.2 Slab Prefetch ───────────────────────────────┤
  └── 4.3 Actor-Affined Slabs ─────── (depends: 1.3)   │
```

---

## 7. Metric Targets

| Metric | Current (est.) | Target (post-optimization) |
|--------|---------------|---------------------------|
| `allocate()` hot path (bump) | <10 ns | <10 ns (preserved) |
| `allocate()` hot path (freelist) | <25 ns | <20 ns (segregated lists, fewer CAS) |
| `deallocate()` hot path | <20 ns | <25 ns (+5ns coalescing check; amortized) |
| Internal fragmentation (7-day uptime) | <5% (design target) | <2% (coalescing eliminates checkerboard) |
| Compaction cycles / day | ~2–4 (design estimate) | 0–1 (coalescing handles most cases) |
| TLB misses / M allocations | ~500 (estimated) | ~50 (super carrier + huge pages) |
| `mmap`/`munmap` syscalls / hour | ~100s–1000s | ~1–2 (super carrier eliminates slab-level mmap) |
| Message throughput (tiny msgs) | baseline | +20–40% (inlining skips allocator) |
| Cross-NUMA allocation penalty | 40–100% | <5% (NUMA-aware placement) |
| 32B block overhead | 125% (40B overhead) | 25% (tiny-block packed metadata) |

---

## 8. Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| Super carrier virtual address exhaustion on 32-bit | Low | Guard with `#ifdef __LP64__`; fall back to current per-segment mmap |
| Coalescing adds deallocation latency | Medium | Make coalescing depth-bounded (max 2 neighbors); benchmark before enabling by default |
| Segregated freelists increase cache miss rate (LIFO locality lost) | Medium | Keep current CAS LIFO as default for `kMessage` region; use segregated only for `kActor` |
| Carrier migration adds complexity with minimal benefit for single-tenant | Low | Make migration opt-in via config; default off until multi-tenant benchmarks prove value |
| Huge page allocation failures under memory pressure | Low | Graceful fallback to 4KB pages with `MADV_HUGEPAGE` hint |
| Coalescing boundary tags require removing from middle of freelist | Medium | Prototype before committing; may limit coalescing to compaction cycle rather than inline |

---

## 9. Reference Documents

### HPActor Internal

- **Parent design doc:** `docs/architecture/memory/memory-management-architecture-design.md`
- **Implementation:** `src/mem/` (11 source files), `include/hpactor/mem/` (16 headers)
- **Tests:** `tests/unit/mem/` (19 test files)
- **CLAUDE_MEMORY.md** — Memory Management section
- **Build config:** `ENABLE_MEMORY_TRACKING`, `ENABLE_MEMORY_DEBUG` CMake options

### Detailed Design Specs (this roadmap)

- `docs/superpowers/specs/2026-06-20-mem-01-segregated-free-lists-design.md`
- `docs/superpowers/specs/2026-06-20-mem-02-free-block-coalescing-design.md`
- `docs/superpowers/specs/2026-06-20-mem-03-per-region-strategy-design.md`
- `docs/superpowers/specs/2026-06-20-mem-04-super-carrier-design.md`
- `docs/superpowers/specs/2026-06-20-mem-05-huge-pages-design.md`
- `docs/superpowers/specs/2026-06-20-mem-06-message-inlining-design.md`

### Erlang / BEAM References

- [erts_alloc — Erlang Runtime System Allocator Documentation](https://www.erlang.org/doc/apps/erts/erts_alloc.html)
- [Erlang Garbage Collector Documentation](https://www.erlang.org/doc/apps/erts/garbagecollection.html)
- [Carrier Migration in Erlang/OTP](https://www.erlang.org/doc/apps/erts/carriermigration.html)

### Key Erlang Allocator Strategies Explained

| Strategy | Description | Complexity | Carrier Migration |
|----------|-------------|------------|-------------------|
| **Good Fit (`gf`)** | Segregated free lists, bounded search depth (default=3) | O(1) constant | No |
| **Best Fit (`bf`)** | Smallest satisfying block via balanced BST | O(log N) | No |
| **Address Order Best Fit (`aobf`)** | Best fit with lowest-address tiebreaker | O(log N) | No |
| **A Fit (`af`)** | Check only first free block; temp_alloc only | O(1) | No |
| **aoffcaobf** | Two-level BST (carriers + per-carrier blocks), address-ordered | O(log N) | Yes |
| **aoffcbf** | Two-level BST, carrier address order, block best fit | O(log N) | Yes |

---

## Next Steps

1. **Review & Prioritize** — Team reviews this roadmap; adjust priorities based
   on production workload profiles.
2. **Deep-Dive Design Specs** — Each P0/P1 item has a detailed design spec in
   `docs/superpowers/specs/`. Review each before implementation.
3. **Benchmark Harness** — Build a memory subsystem micro-benchmark
   (`tests/perf/mem/`) that measures allocation throughput, fragmentation over
   time, and TLB miss rates under simulated workloads.
4. **Incremental Delivery** — Implement Phase 1 items one at a time, each with
   TDDFlow (RED → GREEN → REFACTOR) and the narrowest verification first.
5. **Production Validation** — Run under `ENABLE_MEMORY_DEBUG` + ASAN + TSAN;
   validate with the EdgeOps telemetry app and order platform app as integration
   workloads.
