# HPActor Memory Management — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a high-performance multi-thread safe memory management subsystem with observability, debugging, hibernation, and memory compression for the HPActor actor framework.

**Architecture:** Two-tier slab allocator (Tier 0: mmap-backed global SegmentProvider, Tier 1: per-thread bump+freelist slab caches), typed memory regions with per-region back-pressure, generation-based slabs with compaction, lock-free telemetry ring buffer, hibernation via serialization + madvise(MADV_PAGEOUT) to ZRAM.

**Tech Stack:** C++20, no exceptions, no RTTI, LLVM coding standards, mmap/madvise (Linux), mach_vm (macOS fallback), Ninja/CMake build.

**Spec:** `docs/architecture/memory/memory-management-architecture-design.md`

---

## File Structure

### New Files (create)

```
include/hpactor/mem/
    alloc_header.hpp          — AllocHeader (32B), CanaryFooter (8B), block layout math
    size_class.hpp            — SizeClass enum, kSizeClasses table, size-to-class mapping
    freelist.hpp              — Lock-free CAS freelist (reusable by slabs)
    segment_provider.hpp      — Tier 0: mmap segment acquisition, carving, address→segment lookup
    slab_cache.hpp            — Tier 1: per-size-class slab with bump allocator + freelist
    thread_local_allocator.hpp — Per-thread allocator owning slab caches for all size classes
    memory_region.hpp         — TypedRegion (kActor/kMessage/kCoroutine/kNetwork/kInternal/kHibernate)
    memory_tracker.hpp        — Per-actor shadow counters (alignas(64) atomic array)
    telemetry_ring_buffer.hpp — MPSC lock-free ring buffer for allocation events
    hibernation_registry.hpp  — Concurrent hash map: ActorId → HibernationBuffer
    hibernatable.hpp          — Interface for hibernatable actors
    guard_page.hpp            — Guard page placement + signal handler
    memory_config.hpp         — Compile-time + runtime configuration constants

src/mem/
    segment_provider.cpp
    slab_cache.cpp
    memory_tracker.cpp
    telemetry_ring_buffer.cpp
    hibernation_manager.cpp

tests/mem/
    test_size_class.cpp
    test_freelist.cpp
    test_alloc_header.cpp
    test_segment_provider.cpp
    test_slab_cache.cpp
    test_thread_local_allocator.cpp
    test_memory_tracker.cpp
    test_telemetry_ring_buffer.cpp
    test_memory_poisoning.cpp
    test_guard_page.cpp
    test_hibernation.cpp
    test_memory_stress.cpp
```

### Modified Files

```
include/hpactor/net/platform.hpp       → Move to include/hpactor/platform.hpp (elevate)
include/hpactor/actor/actor_state.hpp  → Add kHibernating state (0x20)
include/hpactor/sched/worker_thread.hpp → Add ThreadLocalAllocator* member
include/hpactor/sched/coroutine_frame_pool.hpp → Refactor backing store to use mem::
include/hpactor/mailbox/mpsc_actor_mailbox.hpp → Replace new/delete with mem::
include/hpactor/core/actor_system.hpp  → Integrate actor allocation from kActor region
include/hpactor/actor_context.hpp → Add hibernate() method (note: top-level, not in actor/ subdir)
include/hpactor/actor/event_based_actor.hpp → Inherit Hibernatable (optional)
include/hpactor/sched/scheduler.hpp    → Skip kHibernating actors, compaction background task
src/sched/scheduler.cpp               → Wire ThreadLocalAllocator init, compaction
src/sched/coroutine_frame_pool.cpp    → Refactor to use mem::allocate/mem::deallocate
src/sched/worker_thread.cpp           → Init/destroy ThreadLocalAllocator
src/actor/actor_system.cpp            → Integrate custom allocator for spawn/destroy
CMakeLists.txt                         → New options, source files, test targets
include/hpactor/hpactor_config.hpp.in → New defines
```

---

## Phase M1: Platform Foundation and Core Types (Tasks 1-4)

### Task 1: Elevate platform.hpp and add memory CMake options

**Files:**
- Create: `include/hpactor/platform.hpp`
- Modify: `include/hpactor/net/platform.hpp`
- Modify: `include/hpactor/hpactor_config.hpp.in`
- Modify: `CMakeLists.txt:22-25`

- [ ] **Step 1: Create the elevated platform header**

```cpp
// include/hpactor/platform.hpp
#pragma once

#include <cstddef>

#ifdef __linux__
#    define HPACTOR_PLATFORM_LINUX 1
#elif defined(__APPLE__)
#    define HPACTOR_PLATFORM_MACOS 1
#else
#    define HPACTOR_PLATFORM_UNKNOWN 1
#endif

namespace hpactor {
using byte_t = unsigned char;

inline constexpr size_t default_mailbox_capacity = 1024;
} // namespace hpactor
```

- [ ] **Step 2: Update net/platform.hpp to include the new header**

```cpp
// include/hpactor/net/platform.hpp
#pragma once

#include <hpactor/platform.hpp>
// Backwards compatibility: all symbols available via hpactor/platform.hpp
```

- [ ] **Step 3: Add memory options to CMakeLists.txt**

Insert after line 25 (`option(ENABLE_PROACTOR ...)`):
```cmake
option(ENABLE_MEMORY_TRACKING "Enable per-actor memory tracking and telemetry" ON)
option(ENABLE_MEMORY_DEBUG "Enable memory poisoning, canaries, and guard pages" OFF)
```

- [ ] **Step 4: Add config defines to hpactor_config.hpp.in**

Append after line 40:
```cpp
#cmakedefine01 HPACTOR_ENABLE_MEMORY_TRACKING
#cmakedefine01 HPACTOR_ENABLE_MEMORY_DEBUG
```

- [ ] **Step 5: Build and verify**

Run: `cmake -S . -B build -GNinja && ninja -C build`
Expected: Build passes with new options visible in cmake output.

- [ ] **Step 6: Run existing tests**

Run: `ctest --output-on-failure`
Expected: All existing tests pass (no regressions).

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/platform.hpp include/hpactor/net/platform.hpp \
        include/hpactor/hpactor_config.hpp.in CMakeLists.txt
git commit -m "feat(mem): elevate platform.hpp, add ENABLE_MEMORY_TRACKING and ENABLE_MEMORY_DEBUG CMake options"
```

---

### Task 2: Define SizeClass infrastructure

**Files:**
- Create: `include/hpactor/mem/size_class.hpp`
- Create: `tests/mem/test_size_class.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/mem/test_size_class.cpp
#include <hpactor/mem/size_class.hpp>
#include <cassert>
#include <iostream>

int main() {
    using namespace hpactor::mem;

    // Verify 8 size classes
    assert(kNumSizeClasses == 8);

    // Verify size classes are power-of-two multiples of 32
    assert(SizeClass::k32B == 0);
    assert(SizeClass::k64B == 1);
    assert(SizeClass::k128B == 2);
    assert(SizeClass::k256B == 3);
    assert(SizeClass::k512B == 4);
    assert(SizeClass::k1KB == 5);
    assert(SizeClass::k2KB == 6);
    assert(SizeClass::k4KB == 7);

    // Verify size_for_class()
    assert(size_for_class(SizeClass::k32B) == 32);
    assert(size_for_class(SizeClass::k64B) == 64);
    assert(size_for_class(SizeClass::k128B) == 128);
    assert(size_for_class(SizeClass::k256B) == 256);
    assert(size_for_class(SizeClass::k512B) == 512);
    assert(size_for_class(SizeClass::k1KB) == 1024);
    assert(size_for_class(SizeClass::k2KB) == 2048);
    assert(size_for_class(SizeClass::k4KB) == 4096);

    // Verify class_for_size() rounds up correctly
    assert(class_for_size(1) == SizeClass::k32B);
    assert(class_for_size(32) == SizeClass::k32B);
    assert(class_for_size(33) == SizeClass::k64B);
    assert(class_for_size(64) == SizeClass::k64B);
    assert(class_for_size(65) == SizeClass::k128B);
    assert(class_for_size(128) == SizeClass::k128B);
    assert(class_for_size(400) == SizeClass::k512B);
    assert(class_for_size(500) == SizeClass::k512B);
    assert(class_for_size(513) == SizeClass::k1KB);
    assert(class_for_size(1024) == SizeClass::k1KB);
    assert(class_for_size(2000) == SizeClass::k2KB);
    assert(class_for_size(3000) == SizeClass::k4KB);
    assert(class_for_size(4096) == SizeClass::k4KB);

    // Verify block_size() includes header + footer overhead
    // Overhead: 32B header + 8B footer = 40B
    assert(block_size(SizeClass::k32B) == 32 + 40);   // 72
    assert(block_size(SizeClass::k64B) == 64 + 40);   // 104
    assert(block_size(SizeClass::k4KB) == 4096 + 40); // 4136

    // Verify user_size() subtracts overhead
    assert(user_size(block_size(SizeClass::k128B)) == 128);

    std::cout << "test_size_class: PASS\n";
    return 0;
}
```

- [ ] **Step 2: Add test target to CMakeLists.txt**

In `tests/CMakeLists.txt`, add after the existing test entries:
```cmake
# =============================================================================
# Memory tests
# =============================================================================
add_executable(test_size_class mem/test_size_class.cpp)
target_link_libraries(test_size_class hpactor)
add_test(NAME test_size_class COMMAND test_size_class)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cmake -S . -B build -GNinja && ninja -C build && ctest -R test_size_class --output-on-failure`
Expected: BUILD FAIL — `hpactor/mem/size_class.hpp` not found.

- [ ] **Step 4: Write the minimal implementation**

```cpp
// include/hpactor/mem/size_class.hpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace hpactor::mem {

enum class SizeClass : uint8_t {
    k32B  = 0,
    k64B  = 1,
    k128B = 2,
    k256B = 3,
    k512B = 4,
    k1KB  = 5,
    k2KB  = 6,
    k4KB  = 7,
};

inline constexpr uint8_t kNumSizeClasses = 8;

inline constexpr size_t kSizeClassTable[kNumSizeClasses] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096
};

// AllocHeader (32 bytes) + CanaryFooter (8 bytes) = 40 bytes overhead
inline constexpr size_t kAllocOverhead = 40;

inline constexpr size_t size_for_class(SizeClass sc) noexcept {
    return kSizeClassTable[static_cast<uint8_t>(sc)];
}

inline constexpr size_t block_size(SizeClass sc) noexcept {
    return size_for_class(sc) + kAllocOverhead;
}

inline constexpr size_t user_size(size_t block_sz) noexcept {
    return block_sz - kAllocOverhead;
}

inline SizeClass class_for_size(size_t user_bytes) noexcept {
    for (uint8_t i = 0; i < kNumSizeClasses; ++i) {
        if (user_bytes <= kSizeClassTable[i]) {
            return static_cast<SizeClass>(i);
        }
    }
    return SizeClass::k4KB;
}

} // namespace hpactor::mem
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build && ctest -R test_size_class --output-on-failure`
Expected: test_size_class PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/mem/size_class.hpp tests/mem/test_size_class.cpp tests/CMakeLists.txt
git commit -m "feat(mem): add SizeClass infrastructure with power-of-two sizing"
```

---

### Task 3: Define AllocHeader and CanaryFooter

**Files:**
- Create: `include/hpactor/mem/alloc_header.hpp`
- Create: `tests/mem/test_alloc_header.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/mem/test_alloc_header.cpp
#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/size_class.hpp>
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    using namespace hpactor::mem;

    // Verify sizes
    assert(sizeof(AllocHeader) == 32);
    assert(sizeof(CanaryFooter) == 8);
    static_assert(sizeof(AllocHeader) == 32, "AllocHeader must be 32 bytes");
    static_assert(sizeof(CanaryFooter) == 8, "CanaryFooter must be 8 bytes");

    // Allocate a raw buffer and stamp it
    constexpr size_t bs = block_size(SizeClass::k128B); // 128 + 40 = 168
    alignas(alignof(AllocHeader)) std::byte buffer[bs];

    // Stamp header
    AllocHeader* hdr = AllocHeader::stamp(buffer, SizeClass::k128B, ActorId{42});
    assert(hdr != nullptr);
    assert(hdr->owner_id == 42);
    assert(hdr->magic == kAllocMagic);
    assert(hdr->size_class == static_cast<uint8_t>(SizeClass::k128B));
    assert(hdr->generation == 0);
    assert(hdr->flags == 0);

    // Verify user_data() returns pointer after header
    void* user = hdr->user_data();
    assert(static_cast<std::byte*>(user) == buffer + sizeof(AllocHeader));

    // Stamp footer
    CanaryFooter::stamp(hdr);
    CanaryFooter* ftr = CanaryFooter::from_header(hdr);
    assert(ftr->magic == kAllocMagic);

    // Verify footer is at the end of the block
    assert(reinterpret_cast<std::byte*>(ftr) == buffer + bs - sizeof(CanaryFooter));

    // Verify canary
    assert(CanaryFooter::verify(hdr) == true);

    // Corrupt the canary and verify detection
    ftr->magic = 0xDEAD;
    assert(CanaryFooter::verify(hdr) == false);

    // Test from_user_data() round-trip
    AllocHeader* hdr2 = AllocHeader::from_user_data(user);
    assert(hdr2 == hdr);
    assert(hdr2->owner_id == 42);

    // Test freed magic
    hdr->magic = kFreedMagic;
    assert(hdr->is_freed() == true);
    assert(hdr->is_live() == false);

    // Test pointer arithmetic helpers
    std::byte* raw = reinterpret_cast<std::byte*>(hdr);
    assert(AllocHeader::footer_ptr(raw, bs) == reinterpret_cast<std::byte*>(ftr));
    assert(AllocHeader::user_ptr(raw) == static_cast<std::byte*>(user));

    std::cout << "test_alloc_header: PASS\n";
    return 0;
}
```

- [ ] **Step 2: Add test target**

In `tests/CMakeLists.txt` after the test_size_class entry:
```cmake
add_executable(test_alloc_header mem/test_alloc_header.cpp)
target_link_libraries(test_alloc_header hpactor)
add_test(NAME test_alloc_header COMMAND test_alloc_header)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `ninja -C build 2>&1 | head -20`
Expected: BUILD FAIL — `hpactor/mem/alloc_header.hpp` not found.

- [ ] **Step 4: Write the implementation**

```cpp
// include/hpactor/mem/alloc_header.hpp
#pragma once

#include <hpactor/mem/size_class.hpp>
#include <hpactor/types/types_fwd.hpp>

#include <cstddef>
#include <cstdint>

namespace hpactor::mem {

// Magic values for corruption detection
inline constexpr uint32_t kAllocMagic  = 0xAC70AC70;  // "ACTO ACTO"
inline constexpr uint32_t kFreedMagic  = 0xDEADDEAD;

// ---------------------------------------------------------------------------
// AllocHeader: 32-byte metadata prefix on every block
// ---------------------------------------------------------------------------
struct alignas(32) AllocHeader {
    uint64_t owner_id;       // ActorId that owns this block (full 64-bit, no truncation)
    uint32_t incarnation;    // actor incarnation at allocation time
    uint32_t magic;          // kAllocMagic or kFreedMagic
    uint8_t  size_class;     // SizeClass index
    uint8_t  generation;     // slab generation at allocation time
    uint16_t flags;          // region type, poison flag, etc.
    uint64_t timestamp;      // rdtsc at allocation

    // Freelist linkage (reuses first 8 bytes when freed via magic == kFreedMagic)
    AllocHeader* next;       // valid only when is_freed()

    // Stamp a header at the given buffer address
    static AllocHeader* stamp(void* block, SizeClass sc, ActorId owner) noexcept {
        auto* h = reinterpret_cast<AllocHeader*>(block);
        h->owner_id = owner.value();
        h->incarnation = 0;
        h->magic = kAllocMagic;
        h->size_class = static_cast<uint8_t>(sc);
        h->generation = 0;
        h->flags = 0;
        h->timestamp = 0;
        return h;
    }

    void* user_data() noexcept {
        return reinterpret_cast<std::byte*>(this) + sizeof(AllocHeader);
    }

    static AllocHeader* from_user_data(void* user_ptr) noexcept {
        return reinterpret_cast<AllocHeader*>(
            static_cast<std::byte*>(user_ptr) - sizeof(AllocHeader));
    }

    static std::byte* user_ptr(std::byte* block) noexcept {
        return block + sizeof(AllocHeader);
    }

    static std::byte* footer_ptr(std::byte* block, size_t block_sz) noexcept {
        return block + block_sz - sizeof(CanaryFooter);
    }

    bool is_live() const noexcept { return magic == kAllocMagic; }
    bool is_freed() const noexcept { return magic == kFreedMagic; }

    size_t block_size() const noexcept {
        return mem::block_size(static_cast<SizeClass>(size_class));
    }

    size_t user_size() const noexcept {
        return mem::size_for_class(static_cast<SizeClass>(size_class));
    }
};

// ---------------------------------------------------------------------------
// CanaryFooter: 8-byte canary at the end of every block
// ---------------------------------------------------------------------------
struct alignas(8) CanaryFooter {
    uint32_t magic;    // kAllocMagic
    uint32_t checksum; // reserved for future use

    static void stamp(AllocHeader* header) noexcept {
        auto* f = from_header(header);
        f->magic = kAllocMagic;
        f->checksum = 0;
    }

    static CanaryFooter* from_header(AllocHeader* header) noexcept {
        return reinterpret_cast<CanaryFooter*>(
            reinterpret_cast<std::byte*>(header) + header->block_size() - sizeof(CanaryFooter));
    }

    static bool verify(const AllocHeader* header) noexcept {
        auto* f = from_header(const_cast<AllocHeader*>(header));
        return f->magic == kAllocMagic;
    }
};

} // namespace hpactor::mem
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build && ctest -R test_alloc_header --output-on-failure`
Expected: test_alloc_header PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/mem/alloc_header.hpp tests/mem/test_alloc_header.cpp tests/CMakeLists.txt
git commit -m "feat(mem): add AllocHeader (32B) and CanaryFooter (8B) block metadata"
```

---

### Task 4: Implement lock-free Freelist

**Files:**
- Create: `include/hpactor/mem/freelist.hpp`
- Create: `tests/mem/test_freelist.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/mem/test_freelist.cpp
#include <hpactor/mem/freelist.hpp>
#include <cassert>
#include <cstring>
#include <thread>
#include <vector>
#include <iostream>

struct TestNode {
    TestNode* next;
    uint32_t value;
};

int main() {
    using namespace hpactor::mem;

    // Test basic push/pop
    {
        FreeList<TestNode> fl;
        assert(fl.empty());

        TestNode a{.next = nullptr, .value = 1};
        TestNode b{.next = nullptr, .value = 2};
        TestNode c{.next = nullptr, .value = 3};

        fl.push(&a);
        assert(!fl.empty());
        fl.push(&b);
        fl.push(&c);

        TestNode* n = fl.pop();
        assert(n != nullptr);
        assert(n->value == 3); // LIFO order: last pushed = first popped
        n = fl.pop();
        assert(n != nullptr);
        assert(n->value == 2);
        n = fl.pop();
        assert(n != nullptr);
        assert(n->value == 1);
        assert(fl.empty());
        assert(fl.pop() == nullptr);
    }

    // Test concurrent push/pop from multiple threads
    {
        FreeList<TestNode> fl;
        constexpr int kNumItems = 10000;
        constexpr int kNumThreads = 4;

        std::atomic<int> push_count{0};
        std::atomic<int> pop_count{0};

        std::vector<std::thread> threads;
        for (int t = 0; t < kNumThreads; ++t) {
            threads.emplace_back([&]() {
                // Push
                for (int i = 0; i < kNumItems / kNumThreads; ++i) {
                    auto* node = new TestNode{nullptr, static_cast<uint32_t>(i)};
                    fl.push(node);
                    push_count.fetch_add(1);
                }
                // Pop
                while (pop_count.load() < kNumItems) {
                    TestNode* n = fl.pop();
                    if (n) {
                        pop_count.fetch_add(1);
                        delete n;
                    }
                }
            });
        }
        for (auto& t : threads) t.join();

        assert(pop_count.load() == kNumItems);
        assert(fl.empty());
    }

    std::cout << "test_freelist: PASS\n";
    return 0;
}
```

- [ ] **Step 2: Add test target**

```cmake
add_executable(test_freelist mem/test_freelist.cpp)
target_link_libraries(test_freelist hpactor pthread)
add_test(NAME test_freelist COMMAND test_freelist)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `ninja -C build 2>&1 | head -20`
Expected: BUILD FAIL.

- [ ] **Step 4: Write the implementation**

```cpp
// include/hpactor/mem/freelist.hpp
#pragma once

#include <atomic>
#include <cstddef>

namespace hpactor::mem {

// Lock-free LIFO freelist. Each node must have a `T* next` field.
// Thread-safe for any number of concurrent push/pop operations.
template <typename T>
class FreeList {
public:
    FreeList() : top_(nullptr) {}

    void push(T* node) noexcept {
        node->next = top_.load(std::memory_order_acquire);
        while (!top_.compare_exchange_weak(
            node->next, node,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
            // CAS failed, node->next was reloaded with current top_
        }
    }

    T* pop() noexcept {
        T* node = top_.load(std::memory_order_acquire);
        while (node != nullptr) {
            T* next = node->next;
            if (top_.compare_exchange_weak(
                node, next,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
                return node;
            }
            // CAS failed, node reloaded
        }
        return nullptr;
    }

    bool empty() const noexcept {
        return top_.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<T*> top_;
};

} // namespace hpactor::mem
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build && ctest -R test_freelist --output-on-failure`
Expected: test_freelist PASS.

- [ ] **Step 6: Run all tests to verify no regressions**

Run: `ctest --output-on-failure`
Expected: 67/67 tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/mem/freelist.hpp tests/mem/test_freelist.cpp tests/CMakeLists.txt
git commit -m "feat(mem): add lock-free CAS freelist for slab recycling"
```

---

## Phase M2: Two-Tier Slab Allocator (Tasks 5-9)

### Task 5: Implement SegmentProvider (Tier 0)

**Files:**
- Create: `include/hpactor/mem/segment_provider.hpp`
- Create: `src/mem/segment_provider.cpp`
- Create: `tests/mem/test_segment_provider.cpp`
- Modify: `CMakeLists.txt` (add src/mem/segment_provider.cpp to hpactor_lib)
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/mem/test_segment_provider.cpp
#include <hpactor/mem/segment_provider.hpp>
#include <hpactor/mem/size_class.hpp>
#include <cassert>
#include <cstring>
#include <iostream>
#include <set>

int main() {
    using namespace hpactor::mem;

    SegmentProvider& sp = SegmentProvider::instance();

    // Acquire slabs of different size classes
    void* slab32  = sp.acquire_slab(SizeClass::k32B);
    void* slab64  = sp.acquire_slab(SizeClass::k64B);
    void* slab128 = sp.acquire_slab(SizeClass::k128B);
    void* slab4k  = sp.acquire_slab(SizeClass::k4KB);

    assert(slab32 != nullptr);
    assert(slab64 != nullptr);
    assert(slab128 != nullptr);
    assert(slab4k != nullptr);

    // All pointers should be distinct
    std::set<void*> ptrs = {slab32, slab64, slab128, slab4k};
    assert(ptrs.size() == 4);

    // Each slab should be usable memory (writeable)
    size_t sz32 = sp.slab_size(SizeClass::k32B);
    size_t sz4k = sp.slab_size(SizeClass::k4KB);
    assert(sz32 > 0);
    assert(sz4k > 0);
    assert(sz32 < sz4k);  // smaller size class = more blocks = potentially smaller slab

    std::memset(slab32, 0xAB, sz32);
    std::memset(slab4k, 0xCD, sz4k);

    // Verify address-to-segment lookup
    SegmentProvider::SegmentInfo info32 = sp.lookup(slab32);
    assert(info32.base != nullptr);
    assert(info32.size >= sz32);

    // Release slabs back
    sp.release_slab(slab32, SizeClass::k32B);
    sp.release_slab(slab64, SizeClass::k64B);
    sp.release_slab(slab128, SizeClass::k128B);
    sp.release_slab(slab4k, SizeClass::k4KB);

    // Stats should reflect release
    auto stats = sp.stats();
    assert(stats.total_allocated > 0);

    std::cout << "test_segment_provider: PASS\n";
    return 0;
}
```

- [ ] **Step 2: Add test target and library source**

In `tests/CMakeLists.txt`:
```cmake
add_executable(test_segment_provider mem/test_segment_provider.cpp)
target_link_libraries(test_segment_provider hpactor pthread)
add_test(NAME test_segment_provider COMMAND test_segment_provider)
```

In `CMakeLists.txt`, add `src/mem/segment_provider.cpp` to the hpactor_lib sources (after the existing source entries, before `src/sched/scheduler.cpp`).

- [ ] **Step 3: Run test to verify it fails**

Run: `ninja -C build 2>&1 | tail -5`
Expected: BUILD FAIL.

- [ ] **Step 4: Write the header**

```cpp
// include/hpactor/mem/segment_provider.hpp
#pragma once

#include <hpactor/mem/size_class.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hpactor::mem {

class SegmentProvider {
public:
    struct SegmentInfo {
        void* base;
        size_t size;
    };

    struct Stats {
        size_t total_allocated{0};
        size_t total_freed{0};
        size_t active_segments{0};
    };

    static SegmentProvider& instance();

    // Acquire a slab carved from a segment for the given size class.
    void* acquire_slab(SizeClass sc);

    // Release a slab back. When all slabs in a segment are released, munmap.
    void release_slab(void* slab, SizeClass sc);

    // Look up which segment a pointer belongs to (for free() path).
    SegmentInfo lookup(void* ptr) const;

    // Size of a slab for a given size class.
    size_t slab_size(SizeClass sc) const;

    Stats stats() const;

private:
    SegmentProvider() = default;

    struct Segment {
        void* base;
        size_t size;
        size_t offset;           // next carve position
        std::atomic<uint32_t> ref_count;
    };

    void* carve_from_segment(SizeClass sc);
    void* allocate_new_segment(size_t size);

    mutable std::mutex mutex_;
    std::vector<Segment> segments_;    // active segments
    std::unordered_map<void*, Segment*> addr_to_segment_;

    static constexpr size_t kSegmentSize = 2 * 1024 * 1024; // 2MB
    static constexpr size_t kSlabSize = 64 * 1024;          // 64KB default slab
};
} // namespace hpactor::mem
```

- [ ] **Step 5: Write the implementation**

```cpp
// src/mem/segment_provider.cpp
#include <hpactor/mem/segment_provider.hpp>
#include <hpactor/platform.hpp>

#include <cstring>
#include <stdexcept>

#if HPACTOR_PLATFORM_LINUX
#    include <sys/mman.h>
#elif HPACTOR_PLATFORM_MACOS
#    include <sys/mman.h>
#endif

namespace hpactor::mem {

SegmentProvider& SegmentProvider::instance() {
    static SegmentProvider sp;
    return sp;
}

void* SegmentProvider::acquire_slab(SizeClass sc) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Try to carve from existing segments first
    void* slab = carve_from_segment(sc);
    if (slab) return slab;

    // Allocate a new segment
    return allocate_new_segment(slab_size(sc));
}

void SegmentProvider::release_slab(void* slab, SizeClass sc) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = addr_to_segment_.find(slab);
    if (it == addr_to_segment_.end()) return;

    Segment* seg = it->second;
    addr_to_segment_.erase(it);

    if (seg->ref_count.fetch_sub(1) == 1) {
        // Last slab in segment — munmap the entire segment
        munmap(seg->base, seg->size);
        // Remove from segments_ vector
        auto seg_it = std::find_if(segments_.begin(), segments_.end(),
            [seg](const Segment& s) { return &s == seg; });
        if (seg_it != segments_.end()) {
            segments_.erase(seg_it);
        }
    }
}

SegmentProvider::SegmentInfo SegmentProvider::lookup(void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = addr_to_segment_.find(ptr);
    if (it != addr_to_segment_.end()) {
        return {it->second->base, it->second->size};
    }
    // Linear scan for pointers inside segments (not at slab start)
    for (const auto& seg : segments_) {
        if (ptr >= seg.base &&
            ptr < static_cast<std::byte*>(seg.base) + seg.size) {
            return {seg.base, seg.size};
        }
    }
    return {nullptr, 0};
}

size_t SegmentProvider::slab_size(SizeClass sc) const {
    // Larger size classes get larger slabs
    switch (sc) {
        case SizeClass::k32B:  return kSlabSize;       // 64KB → 2048 blocks
        case SizeClass::k64B:  return kSlabSize;       // 64KB → 1024 blocks
        case SizeClass::k128B: return kSlabSize;       // 64KB → 512 blocks
        case SizeClass::k256B: return kSlabSize * 2;   // 128KB
        case SizeClass::k512B: return kSlabSize * 4;   // 256KB
        case SizeClass::k1KB:  return kSlabSize * 4;   // 256KB
        case SizeClass::k2KB:  return kSlabSize * 8;   // 512KB
        case SizeClass::k4KB:  return kSlabSize * 8;   // 512KB
    }
    return kSlabSize;
}

SegmentProvider::Stats SegmentProvider::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s;
    s.active_segments = segments_.size();
    for (const auto& seg : segments_) {
        s.total_allocated += seg.size;
    }
    return s;
}

void* SegmentProvider::carve_from_segment(SizeClass sc) {
    size_t needed = slab_size(sc);
    for (auto& seg : segments_) {
        if (seg.size - seg.offset >= needed) {
            void* slab = static_cast<std::byte*>(seg.base) + seg.offset;
            seg.offset += needed;
            seg.ref_count.fetch_add(1);
            addr_to_segment_[slab] = &seg;
            return slab;
        }
    }
    return nullptr;
}

void* SegmentProvider::allocate_new_segment(size_t size) {
    size_t alloc_size = (size > kSegmentSize) ? size : kSegmentSize;

    void* base = mmap(nullptr, alloc_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1, 0);

    if (base == MAP_FAILED) return nullptr;

    Segment seg;
    seg.base = base;
    seg.size = alloc_size;
    seg.offset = size;  // first `size` bytes are this slab
    seg.ref_count.store(1);

    segments_.push_back(seg);
    Segment* seg_ptr = &segments_.back();
    addr_to_segment_[base] = seg_ptr;

    return base;
}

} // namespace hpactor::mem
```

- [ ] **Step 6: Run test to verify it passes**

Run: `ninja -C build && ctest -R test_segment_provider --output-on-failure`
Expected: test_segment_provider PASS.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/mem/segment_provider.hpp src/mem/segment_provider.cpp \
        tests/mem/test_segment_provider.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(mem): add SegmentProvider — Tier 0 mmap-based segment acquisition"
```

---

### Task 6: Implement SlabCache (Tier 1 — per-size-class slab)

**Files:**
- Create: `include/hpactor/mem/slab_cache.hpp`
- Create: `tests/mem/test_slab_cache.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/mem/test_slab_cache.cpp
#include <hpactor/mem/slab_cache.hpp>
#include <hpactor/mem/size_class.hpp>
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    using namespace hpactor::mem;

    // Create a slab cache for 128B size class
    SlabCache cache(SizeClass::k128B);

    assert(cache.size_class() == SizeClass::k128B);
    assert(cache.live_count() == 0);

    // Allocate several blocks
    void* b1 = cache.allocate(ActorId{1});
    void* b2 = cache.allocate(ActorId{2});
    void* b3 = cache.allocate(ActorId{3});

    assert(b1 != nullptr);
    assert(b2 != nullptr);
    assert(b3 != nullptr);
    assert(b1 != b2);
    assert(b2 != b3);
    assert(cache.live_count() == 3);

    // Each block should be independently writeable
    std::memset(b1, 0x11, 128);
    std::memset(b2, 0x22, 128);
    std::memset(b3, 0x33, 128);
    // Verify no cross-block corruption
    assert(*static_cast<uint8_t*>(b1) == 0x11);
    assert(*static_cast<uint8_t*>(b2) == 0x22);
    assert(*static_cast<uint8_t*>(b3) == 0x33);

    // Free one block back
    cache.deallocate(b1);
    assert(cache.live_count() == 2);

    // Allocate again — should get recycled block
    void* b4 = cache.allocate(ActorId{4});
    assert(b4 != nullptr);
    assert(b4 == b1); // recycled from freelist
    assert(cache.live_count() == 3);

    // Free all
    cache.deallocate(b2);
    cache.deallocate(b3);
    cache.deallocate(b4);
    assert(cache.live_count() == 0);

    // Allocate many blocks to force multiple slab acquisitions
    std::vector<void*> blocks;
    for (int i = 0; i < 1000; ++i) {
        void* b = cache.allocate(ActorId{static_cast<uint32_t>(i)});
        assert(b != nullptr);
        std::memset(b, static_cast<uint8_t>(i & 0xFF), 128);
        blocks.push_back(b);
    }
    assert(cache.live_count() == 1000);

    // Free all
    for (auto* b : blocks) {
        cache.deallocate(b);
    }
    assert(cache.live_count() == 0);

    // Verify stats
    auto stats = cache.stats();
    assert(stats.alloc_count == 1003);  // 3 + 1000
    assert(stats.free_count == 1003);

    std::cout << "test_slab_cache: PASS\n";
    return 0;
}
```

- [ ] **Step 2: Add test target**

```cmake
add_executable(test_slab_cache mem/test_slab_cache.cpp)
target_link_libraries(test_slab_cache hpactor pthread)
add_test(NAME test_slab_cache COMMAND test_slab_cache)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `ninja -C build 2>&1 | tail -5`
Expected: BUILD FAIL.

- [ ] **Step 4: Write the implementation**

```cpp
// include/hpactor/mem/slab_cache.hpp
#pragma once

#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/freelist.hpp>
#include <hpactor/mem/segment_provider.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hpactor::mem {

// Per-size-class slab cache. Manages one or more slabs of the same size class.
// Uses bump allocation for virgin memory + lock-free freelist for recycled blocks.
class SlabCache {
public:
    struct Stats {
        std::atomic<uint64_t> alloc_count{0};
        std::atomic<uint64_t> free_count{0};
        std::atomic<uint64_t> slab_acquire_count{0};
    };

    explicit SlabCache(SizeClass sc) : size_class_(sc) {
        // Pre-acquire first slab
        refill();
    }

    ~SlabCache() {
        // Release all slabs back to SegmentProvider
        for (auto* slab : slabs_) {
            SegmentProvider::instance().release_slab(slab, size_class_);
        }
    }

    SlabCache(const SlabCache&) = delete;
    SlabCache& operator=(const SlabCache&) = delete;
    SlabCache(SlabCache&&) = delete;
    SlabCache& operator=(SlabCache&&) = delete;

    void* allocate(ActorId owner) noexcept {
        // 1. Try freelist first
        void* block = freelist_.pop();
        if (block) {
            auto* hdr = static_cast<AllocHeader*>(block);
            hdr->owner_id = owner.value();
            hdr->magic = kAllocMagic;
            hdr->generation = current_generation_;
            hdr->timestamp = 0;
            stats_.alloc_count.fetch_add(1);
            live_count_.fetch_add(1);
            return hdr->user_data();
        }

        // 2. Bump allocate from current slab
        if (current_slab_) {
            size_t bs = block_size(size_class_);
            if (bump_offset_ + bs <= slab_size_) {
                auto* raw = current_slab_ + bump_offset_;
                bump_offset_ += bs;
                auto* hdr = AllocHeader::stamp(raw, size_class_, owner);
                hdr->generation = current_generation_;
                CanaryFooter::stamp(hdr);
                stats_.alloc_count.fetch_add(1);
                live_count_.fetch_add(1);
                return hdr->user_data();
            }
        }

        // 3. Need a new slab
        refill();
        if (current_slab_) {
            return allocate(owner); // retry with fresh slab
        }
        return nullptr; // OOM
    }

    void deallocate(void* user_ptr) noexcept {
        auto* hdr = AllocHeader::from_user_data(user_ptr);
        hdr->magic = kFreedMagic;
        stats_.free_count.fetch_add(1);
        live_count_.fetch_sub(1);
        freelist_.push(reinterpret_cast<typename FreeList<AllocHeader>::node_type*>(hdr));
    }

    SizeClass size_class() const noexcept { return size_class_; }
    uint32_t live_count() const noexcept { return live_count_.load(); }
    const Stats& stats() const noexcept { return stats_; }

private:
    void refill() {
        current_slab_ = static_cast<std::byte*>(
            SegmentProvider::instance().acquire_slab(size_class_));
        if (current_slab_) {
            slab_size_ = SegmentProvider::instance().slab_size(size_class_);
            bump_offset_ = 0;
            slabs_.push_back(current_slab_);
            stats_.slab_acquire_count.fetch_add(1);
        }
    }

    SizeClass size_class_;
    uint8_t current_generation_{0};

    std::byte* current_slab_{nullptr};
    size_t slab_size_{0};
    size_t bump_offset_{0};

    FreeList<AllocHeader> freelist_;
    std::atomic<uint32_t> live_count_{0};
    Stats stats_;

    std::vector<std::byte*> slabs_; // all slabs owned by this cache
};

} // namespace hpactor::mem
```

**Note:** The `FreeList` template uses `node->next` for linkage. With the `next` field now added directly to `AllocHeader`, the `FreeList<AllocHeader>` works without any adapter — `next` is valid when the block is freed (magic == kFreedMagic). This avoids the union trick originally proposed and keeps the FreeList template clean.

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build && ctest -R test_slab_cache --output-on-failure`
Expected: test_slab_cache PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/mem/slab_cache.hpp tests/mem/test_slab_cache.cpp tests/CMakeLists.txt
git commit -m "feat(mem): add SlabCache — per-size-class slab with bump+freelist allocation"
```

---

### Task 7: Implement ThreadLocalAllocator

**Files:**
- Create: `include/hpactor/mem/thread_local_allocator.hpp`
- Create: `tests/mem/test_thread_local_allocator.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/mem/test_thread_local_allocator.cpp
#include <hpactor/mem/thread_local_allocator.hpp>
#include <hpactor/mem/size_class.hpp>
#include <cassert>
#include <cstring>
#include <thread>
#include <vector>
#include <iostream>

int main() {
    using namespace hpactor::mem;

    {
        ThreadLocalAllocator tla;

        // Allocate from different size classes
        void* p32  = tla.allocate(SizeClass::k32B,  ActorId{1});
        void* p256 = tla.allocate(SizeClass::k256B, ActorId{2});
        void* p1k  = tla.allocate(SizeClass::k1KB,  ActorId{3});

        assert(p32 != nullptr);
        assert(p256 != nullptr);
        assert(p1k != nullptr);

        // Write to each
        std::memset(p32,  0xAA, 32);
        std::memset(p256, 0xBB, 256);
        std::memset(p1k,  0xCC, 1024);

        // Free all
        tla.deallocate(p32);
        tla.deallocate(p256);
        tla.deallocate(p1k);
    }

    // Concurrent allocation from multiple threads (each with own TLA)
    {
        constexpr int kThreads = 4;
        constexpr int kAllocsPerThread = 1000;

        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([t]() {
                ThreadLocalAllocator tla;
                std::vector<void*> ptrs;
                for (int i = 0; i < kAllocsPerThread; ++i) {
                    void* p = tla.allocate(SizeClass::k64B,
                        ActorId{static_cast<uint32_t>(t * kAllocsPerThread + i)});
                    assert(p != nullptr);
                    std::memset(p, static_cast<uint8_t>(t), 64);
                    ptrs.push_back(p);
                }
                for (auto* p : ptrs) {
                    tla.deallocate(p);
                }
            });
        }
        for (auto& t : threads) t.join();
    }

    std::cout << "test_thread_local_allocator: PASS\n";
    return 0;
}
```

- [ ] **Step 2: Add test target**

```cmake
add_executable(test_thread_local_allocator mem/test_thread_local_allocator.cpp)
target_link_libraries(test_thread_local_allocator hpactor pthread)
add_test(NAME test_thread_local_allocator COMMAND test_thread_local_allocator)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `ninja -C build 2>&1 | tail -5`
Expected: BUILD FAIL.

- [ ] **Step 4: Write the implementation**

```cpp
// include/hpactor/mem/thread_local_allocator.hpp
#pragma once

#include <hpactor/mem/slab_cache.hpp>
#include <hpactor/mem/size_class.hpp>

#include <array>
#include <cstddef>

namespace hpactor::mem {

// Per-thread allocator. Owns a SlabCache for each size class.
// The hot allocation path is bump-pointer or freelist pop — no locks.
class ThreadLocalAllocator {
public:
    ThreadLocalAllocator() {
        for (uint8_t i = 0; i < kNumSizeClasses; ++i) {
            caches_[i] = new SlabCache(static_cast<SizeClass>(i));
        }
    }

    ~ThreadLocalAllocator() {
        for (uint8_t i = 0; i < kNumSizeClasses; ++i) {
            delete caches_[i];
        }
    }

    ThreadLocalAllocator(const ThreadLocalAllocator&) = delete;
    ThreadLocalAllocator& operator=(const ThreadLocalAllocator&) = delete;
    ThreadLocalAllocator(ThreadLocalAllocator&&) = delete;
    ThreadLocalAllocator& operator=(ThreadLocalAllocator&&) = delete;

    // Allocate from the appropriate size class. Returns user-data pointer.
    void* allocate(SizeClass sc, ActorId owner) noexcept {
        return caches_[static_cast<uint8_t>(sc)]->allocate(owner);
    }

    // Allocate by user-requested byte size (auto-selects size class).
    void* allocate_bytes(size_t user_bytes, ActorId owner) noexcept {
        SizeClass sc = class_for_size(user_bytes);
        return allocate(sc, owner);
    }

    // Deallocate a block (must have been allocated by this or another TLA).
    void deallocate(void* user_ptr) noexcept {
        auto* hdr = AllocHeader::from_user_data(user_ptr);
        SizeClass sc = static_cast<SizeClass>(hdr->size_class);
        caches_[static_cast<uint8_t>(sc)]->deallocate(user_ptr);
    }

    // Stats for a specific size class
    const SlabCache::Stats& stats(SizeClass sc) const noexcept {
        return caches_[static_cast<uint8_t>(sc)]->stats();
    }

private:
    std::array<SlabCache*, kNumSizeClasses> caches_;
};

} // namespace hpactor::mem
```

- [ ] **Step 5: Run test to verify it passes**

Run: `ninja -C build && ctest -R test_thread_local_allocator --output-on-failure`
Expected: test_thread_local_allocator PASS.

- [ ] **Step 6: Run all tests to verify no regressions**

Run: `ctest --output-on-failure`
Expected: All tests pass (70 total now).

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/mem/thread_local_allocator.hpp tests/mem/test_thread_local_allocator.cpp tests/CMakeLists.txt
git commit -m "feat(mem): add ThreadLocalAllocator — per-thread slab cache array"
```

---

### Task 8: Wire ThreadLocalAllocator into WorkerThread

**Files:**
- Modify: `include/hpactor/sched/worker_thread.hpp`
- Modify: `src/sched/worker_thread.cpp`

- [ ] **Step 1: Read the existing worker_thread.cpp**

Run: `cat src/sched/worker_thread.cpp` to understand the constructor/destructor.

- [ ] **Step 2: Modify worker_thread.hpp**

Add include and member:
```cpp
#include <hpactor/mem/thread_local_allocator.hpp>

// In class WorkerThread, add public accessor:
mem::ThreadLocalAllocator* allocator() { return allocator_.get(); }

// In private section, add member:
std::unique_ptr<mem::ThreadLocalAllocator> allocator_;
```

- [ ] **Step 3: Modify worker_thread.cpp**

In the constructor, add after other initializations:
```cpp
allocator_ = std::make_unique<mem::ThreadLocalAllocator>();
```

- [ ] **Step 4: Build and verify**

Run: `ninja -C build`
Expected: Build passes.

- [ ] **Step 5: Run all tests**

Run: `ctest --output-on-failure`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/sched/worker_thread.hpp src/sched/worker_thread.cpp
git commit -m "feat(mem): wire ThreadLocalAllocator into WorkerThread"
```

---

### Task 9: Add Typed Memory Regions

**Files:**
- Create: `include/hpactor/mem/memory_region.hpp`
- Modify: `tests/CMakeLists.txt` (add to existing test, or note that region is tested via integration)

- [ ] **Step 1: Write the implementation (no separate test — tested via allocator)**

```cpp
// include/hpactor/mem/memory_region.hpp
#pragma once

#include <cstdint>
#include <atomic>

namespace hpactor::mem {

enum class RegionType : uint8_t {
    kActor     = 0,
    kMessage   = 1,
    kCoroutine = 2,
    kNetwork   = 3,
    kInternal  = 4,
    kHibernate = 5,
};

inline constexpr uint8_t kNumRegionTypes = 6;

// Per-region statistics (cache-line aligned to avoid false sharing)
struct alignas(64) RegionStats {
    std::atomic<uint64_t> total_allocated{0};
    std::atomic<uint64_t> total_freed{0};
    std::atomic<uint64_t> active_bytes{0};
    std::atomic<uint64_t> high_water_mark{0};
    std::atomic<uint64_t> alloc_count{0};
    std::atomic<uint64_t> free_count{0};
};

} // namespace hpactor::mem
```

- [ ] **Step 2: Build and verify**

Run: `ninja -C build && ctest --output-on-failure`
Expected: Build + all tests pass.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/mem/memory_region.hpp
git commit -m "feat(mem): add Typed Memory Region enum and per-region stats"
```

---

## Phase M3: Integration with Existing Code (Tasks 10-14)

### Task 10: Add global mem::allocate/mem::deallocate API

**Files:**
- Create: `include/hpactor/mem/memory_config.hpp`
- Modify: (integration into actor_system later)

- [ ] **Step 1: Write the config + convenience API header**

```cpp
// include/hpactor/mem/memory_config.hpp
#pragma once

#include <hpactor/mem/thread_local_allocator.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/hpactor_config.hpp>

#include <cstddef>

namespace hpactor::mem {

// =========================================================================
// Compile-time configuration
// =========================================================================

#if HPACTOR_ENABLE_MEMORY_TRACKING
inline constexpr bool kMemoryTrackingEnabled = true;
#else
inline constexpr bool kMemoryTrackingEnabled = false;
#endif

#if HPACTOR_ENABLE_MEMORY_DEBUG
inline constexpr bool kMemoryDebugEnabled = true;
#else
inline constexpr bool kMemoryDebugEnabled = false;
#endif

// Default sampling rate: 1 in N allocations logged to telemetry
inline constexpr uint32_t kDefaultSampleRate = 128;

// =========================================================================
// Global convenience API (routes to thread-local allocator)
// =========================================================================

// These are the primary API entry points used throughout the codebase.
// They require a ThreadLocalAllocator to be set up on the calling thread.

// Allocate memory for a typed region, auto-selecting size class
void* allocate(RegionType region, size_t user_bytes, ActorId owner);

// Allocate from a specific size class
void* allocate_class(SizeClass sc, ActorId owner);

// Deallocate memory (extracts size class from AllocHeader)
void deallocate(void* user_ptr);

} // namespace hpactor::mem
```

- [ ] **Step 2: Create the implementation file**

```cpp
// src/mem/memory_config.cpp (or inline in the header since it needs TLA access)

// For now, keep as inline functions that access a thread_local:
namespace hpactor::mem {

// Thread-local pointer set by WorkerThread/Scheduler during init
inline thread_local ThreadLocalAllocator* t_tla = nullptr;

inline void set_thread_allocator(ThreadLocalAllocator* tla) {
    t_tla = tla;
}

inline ThreadLocalAllocator* thread_allocator() {
    return t_tla;
}

inline void* allocate(RegionType /*region*/, size_t user_bytes, ActorId owner) {
    if (t_tla) {
        return t_tla->allocate_bytes(user_bytes, owner);
    }
    // Fallback to malloc when no TLA is set (e.g., before scheduler init)
    return std::malloc(user_bytes);
}

inline void* allocate_class(SizeClass sc, ActorId owner) {
    if (t_tla) {
        return t_tla->allocate(sc, owner);
    }
    return std::malloc(size_for_class(sc));
}

inline void deallocate(void* user_ptr) {
    if (!user_ptr) return;
    // Check if this is a TLA-managed block
    auto* hdr = static_cast<AllocHeader*>(
        static_cast<std::byte*>(user_ptr) - sizeof(AllocHeader));
    if (hdr->is_live() || hdr->is_freed()) {
        // It's ours, route through TLA
        if (t_tla) {
            t_tla->deallocate(user_ptr);
            return;
        }
    }
    // Fallback
    std::free(user_ptr);
}

} // namespace hpactor::mem
```

- [ ] **Step 3: Build and verify**

Run: `ninja -C build && ctest --output-on-failure`
Expected: Build + all tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mem/memory_config.hpp CMakeLists.txt
git commit -m "feat(mem): add global allocate/deallocate API with thread_local TLA routing"
```

---

### Task 11: Refactor CoroutineFramePool to use custom allocator

**Files:**
- Modify: `include/hpactor/sched/coroutine_frame_pool.hpp`
- Modify: `src/sched/coroutine_frame_pool.cpp`

- [ ] **Step 1: Update the header — add allocator-aware constructor overload**

In `coroutine_frame_pool.hpp`, add a new private method and modify the constructor to accept an optional `ThreadLocalAllocator*`. The key change: replace `new std::byte[stack_size]` with `mem::allocate(kCoroutine, stack_size, ...)`. For backward compatibility, keep the existing constructor and add an overload.

- [ ] **Step 2: Update coroutine_frame_pool.cpp**

The existing code uses `new std::byte[stack_size]` stored in `std::unique_ptr<std::byte[]>` (which calls `delete[]` on destruction). When using the custom allocator:

- **Allocation:** If TLA is available, use `mem::allocate(RegionType::kCoroutine, stack_size, system_actor_id)`. Store returned pointer in a `std::vector<void*>` with a custom cleanup function (not `unique_ptr<std::byte[]>` since that would call `delete[]`).
- **Deallocation:** The destructor iterates over stored pointers and calls `mem::deallocate(ptr)` for each.
- **FreeNode aliasing:** The existing code (line 35 of `coroutine_frame_pool.cpp`) reinterprets the stack pointer as `FreeNode*` and writes a `next` pointer at offset 0. Since the memory is now from our allocator (not `new[]`), this is safe — the allocator's metadata is at a fixed negative offset (AllocHeader), not at offset 0 of the user data.

For backward compatibility when no TLA is set (e.g., tests that don't initialize the scheduler), keep the `new[]` fallback path with a boolean flag tracking which allocator was used per frame:

- [ ] **Step 3: Build and run existing tests**

Run: `ninja -C build && ctest --output-on-failure`
Expected: All tests pass — CoroutineFramePool behavior is unchanged.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/sched/coroutine_frame_pool.hpp src/sched/coroutine_frame_pool.cpp
git commit -m "refactor(mem): add allocator-aware path to CoroutineFramePool"
```

---

### Task 12: Refactor MPSCActorMailbox to use custom allocator

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

- [ ] **Step 1: Replace new/delete in push() and try_pop()**

In `push()`:
```cpp
// Before:
auto* node = new T(std::move(msg));

// After:
void* raw = mem::allocate(RegionType::kMessage, sizeof(T), actor_id_);
auto* node = new (raw) T(std::move(msg));
```

In `try_pop()`:
```cpp
// Before:
delete node;

// After:
node->~T();
mem::deallocate(node);
```

- [ ] **Step 2: Build and run mailbox tests**

Run: `ninja -C build && ctest -R "mailbox" --output-on-failure`
Expected: All mailbox tests pass.

- [ ] **Step 3: Run all tests**

Run: `ctest --output-on-failure`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp
git commit -m "refactor(mem): route MPSCActorMailbox allocations through custom allocator"
```

---

### Task 13: Integrate with ActorSystem spawn/destroy

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Update spawn to use kActor region with custom shared_ptr deleter**

The existing `spawn()` stores actors in `std::unordered_map<ActorId, std::shared_ptr<AbstractActor>>` and returns an `Actor` which wraps a `std::shared_ptr<AbstractActor>`. To route deallocation through our custom allocator, use a custom deleter:

```cpp
// Custom deleter for shared_ptr that routes through our allocator
struct ActorDeleter {
    void operator()(AbstractActor* actor) const {
        actor->~AbstractActor();
        mem::deallocate(actor);
    }
};

// In spawn():
auto* raw = static_cast<AbstractActor*>(
    mem::allocate(RegionType::kActor, sizeof(T), id));
auto* actor = new (raw) T(std::forward<Args>(args)...);
std::shared_ptr<AbstractActor> ptr(actor, ActorDeleter{});
actors_[id] = ptr;
return Actor{ptr};
```

This ensures that when the last `shared_ptr` reference is dropped, `mem::deallocate()` is called rather than `operator delete`.

- [ ] **Step 2: Update destroy to use deallocate**

In the actor destruction path, call `actor->~T()` followed by `mem::deallocate(actor)`.

- [ ] **Step 3: Build and run actor system tests**

Run: `ninja -C build && ctest -R "actor_system" --output-on-failure`
Expected: test_actor_system PASS.

- [ ] **Step 4: Run all tests**

Run: `ctest --output-on-failure`
Expected: All 70+ tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "refactor(mem): integrate ActorSystem spawn/destroy with custom allocator"
```

---

### Task 14: Memory stress test

**Files:**
- Create: `tests/mem/test_memory_stress.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the stress test**

```cpp
// tests/mem/test_memory_stress.cpp
// 1M alloc/free operations across 8 threads, verifying no leaks, no corruption
// TSan-clean, runs in < 5 seconds
```

- [ ] **Step 2: Add test target and run**

Run: `ninja -C build && ./build/tests/test_memory_stress`
Expected: PASS with "1M operations in <X> ms".

- [ ] **Step 3: Run with TSan**

Run: `cmake -DENABLE_TSAN=ON -S . -B build-tsan && ninja -C build-tsan && ./build-tsan/tests/test_memory_stress`
Expected: PASS, TSan-clean.

- [ ] **Step 4: Commit**

```bash
git add tests/mem/test_memory_stress.cpp tests/CMakeLists.txt
git commit -m "test(mem): add 1M-operation concurrent stress test (TSan-clean)"
```

---

## Phase M4: Observability (Tasks 15-18)

### Task 15: Implement MemoryTracker per-actor shadow counters

### Task 16: Implement TelemetryRingBuffer

### Task 17: Add sampling infrastructure

### Task 18: Wire telemetry background thread

---

## Phase M5: Debugging Features (Tasks 19-23)

### Task 19: Implement memory poisoning on free (0xAA pattern)

### Task 20: Implement canary verification on deallocate

### Task 21: Implement guard pages for large blocks (>4KB fat-block path)

Note: The fat-block path for allocations > 4KB (spec Section 5.3) uses direct mmap with
guard pages at both ends. This task covers both the direct-mmap allocation path AND the
mprotect(PROT_NONE) guard pages.

### Task 22: Implement SIGSEGV handler for guard page violations
- Signal handler catches SIGSEGV, looks up faulting address via SegmentProvider::lookup(),
  terminates the owning actor with ExitReason::kMemoryCorruption instead of crashing the process.

### Task 23: Add tiny-block optimization for 32B size class
Per spec Section 5.2: the 40B header+footer overhead on 32B blocks causes 125% waste.
Implement the packed out-of-band metadata approach using a bitmap for 32B slabs.

---

## Phase M6: Hibernation (Tasks 24-29)

### Task 24: Add kHibernating state to ActorState

**CRITICAL:** When adding `kHibernating = 0x20`, also update `kMask` from `0x1F` to `0x3F` in
`include/hpactor/actor/actor_state.hpp:31`. Any code that validates state values via `kMask`
would reject `kHibernating` (0x20 & 0x1F = 0, not matching any known state).

### Task 25: Implement Hibernatable interface

### Task 26: Implement HibernationRegistry

### Task 27: Implement hibernation protocol (serialize → madvise)

### Task 28: Implement reactivation protocol (madvise → deserialize)

### Task 29: Implement idle-timeout and memory-pressure hibernation triggers

---

## Phase M7: Compaction (Tasks 30-33)

### Task 30: Implement generation tracking and live block counting

### Task 31: Implement actor relocation and slab compaction

### Task 32: Integrate compaction into scheduler background task

### Task 33: Implement fragmentation budget enforcement (5% target from spec Section 10.4)

---

## Phase M8: ZRAM Integration and Performance Validation (Tasks 34-37)

### Task 34: Implement ZRAM detection and MADV_PAGEOUT

### Task 35: End-to-end hibernation density test (100K actors, 80% hibernated)

### Task 36: Performance benchmarks against spec Appendix A targets
- allocate() hot path (bump): < 10ns p50
- allocate() hot path (freelist pop): < 25ns p50
- deallocate() hot path: < 20ns p50
- free() canary verification: < 5ns
- Hibernate actor (2KB state): < 50μs
- Reactivate actor (2KB state): < 500μs

### Task 37: Runtime configuration knobs from spec Appendix B
- Environment variable support: HPACTOR_MEMORY_ACTOR_REGION_LIMIT_MB, etc.
- Config file integration with existing Config system

---

## Appendix: Build Verification After Each Task

After every task, run these commands and verify:

```bash
# Build
cmake -S . -B build -GNinja && ninja -C build

# Run all tests
ctest --output-on-failure

# Verify no regressions
# Expected: test count increases monotonically, never decreases
```

## Appendix: Commit Cadence

- One commit per task (after "Run test to verify it passes" step)
- Commit messages follow format: `<type>(mem): <description>`
- Types: `feat` (new capability), `refactor` (change existing code), `test` (test-only), `fix` (bug fix)
