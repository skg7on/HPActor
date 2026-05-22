# Test Reorganization: Unit/Integration/System Split with Google Test — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reorganize all 152 test files into three tiers (unit/integration/system), vendor Google Test, convert all tests from raw `assert()` to GTest macros, and restructure the CMake build from a 665-line monolithic file to a hierarchical tree.

**Architecture:** A three-tier test hierarchy under `tests/{unit,integration,system}/` with one test binary per subsystem per tier. Google Test is vendored in `third_party/googletest/` and exposed via `cmake/gtest.cmake`. Each subsystem gets a ~15-line `CMakeLists.txt` linking `GTest::gtest_main`. CTest runs individual `TEST()` cases via `gtest_discover_tests()`.

**Tech Stack:** C++20, CMake 3.20+, Ninja, Google Test (vendored), CTest

---

## Phase 1: Infrastructure Setup

### Task 1: Vendor Google Test

**Files:**
- Create: `third_party/googletest/` (vendored source)

- [ ] **Step 1: Clone Google Test at a specific release tag**

```bash
git clone --depth 1 --branch v1.14.0 \
    https://github.com/google/googletest.git third_party/googletest
```

- [ ] **Step 2: Remove unnecessary files to keep vendored footprint small**

```bash
rm -rf third_party/googletest/.git
rm -rf third_party/googletest/.github
rm -rf third_party/googletest/docs
rm -rf third_party/googletest/ci
```

- [ ] **Step 3: Verify the vendored source has the expected structure**

```bash
ls third_party/googletest/CMakeLists.txt && echo "OK: googletest CMakeLists.txt present"
ls third_party/googletest/googletest/CMakeLists.txt && echo "OK: gtest CMakeLists.txt present"
```

- [ ] **Step 4: Commit**

```bash
git add third_party/googletest/
git commit -m "$(cat <<'EOF'
build: vendor Google Test v1.14.0 in third_party/googletest

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

### Task 2: Create cmake/gtest.cmake module

**Files:**
- Create: `cmake/gtest.cmake`

- [ ] **Step 1: Create the CMake module**

```cmake
# Google Test — vendored in third_party/googletest
# Exposes: GTest::gtest (library), GTest::gtest_main (library + main())
#
# Add to a test CMakeLists.txt with:
#   target_link_libraries(my_test GTest::gtest_main)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)

add_subdirectory(
    ${CMAKE_SOURCE_DIR}/third_party/googletest
    ${CMAKE_BINARY_DIR}/googletest
    EXCLUDE_FROM_ALL
)
```

- [ ] **Step 2: Commit**

```bash
git add cmake/gtest.cmake
git commit -m "build: add cmake/gtest.cmake module for vendored Google Test"
```

### Task 3: Create shared test support target and root CMakeLists

**Files:**
- Create: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the new root tests/CMakeLists.txt**

```cmake
# Tests — three-tier: unit, integration, system
add_compile_options(-UNDEBUG)

# Shared test support headers (scheduler_test_driver.hpp)
add_library(hpactor_test_support INTERFACE)
target_include_directories(hpactor_test_support INTERFACE
    ${CMAKE_SOURCE_DIR}/tests/support)

# Vendored Google Test
include(${CMAKE_SOURCE_DIR}/cmake/gtest.cmake)

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(system)
```

- [ ] **Step 2: Rename old tests/CMakeLists.txt and write the new root**

```bash
mv tests/CMakeLists.txt tests/CMakeLists.txt.old
```

Then write `tests/CMakeLists.txt` with:

```cmake
# Tests — three-tier: unit, integration, system
add_compile_options(-UNDEBUG)

# Shared test support headers (scheduler_test_driver.hpp)
add_library(hpactor_test_support INTERFACE)
target_include_directories(hpactor_test_support INTERFACE
    ${CMAKE_SOURCE_DIR}/tests/support)

# Vendored Google Test
include(${CMAKE_SOURCE_DIR}/cmake/gtest.cmake)

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(system)
```

- [ ] **Step 3: Commit**

```bash
git add tests/CMakeLists.txt tests/CMakeLists.txt.old
git commit -m "build: replace monolithic tests/CMakeLists.txt with tier-based root"
```

### Task 4: Create tier-level CMakeLists.txt files

**Files:**
- Create: `tests/unit/CMakeLists.txt`
- Create: `tests/integration/CMakeLists.txt`
- Create: `tests/system/CMakeLists.txt`

- [ ] **Step 1: Create tests/unit/CMakeLists.txt**

```cmake
add_subdirectory(core)
add_subdirectory(adt)
add_subdirectory(mailbox)
add_subdirectory(sched)
add_subdirectory(mem)
if(ENABLE_CLI)
    add_subdirectory(cli)
endif()
add_subdirectory(config)
add_subdirectory(ref)
add_subdirectory(log)
if(ENABLE_ACTOR_TRACING)
    add_subdirectory(tracing)
endif()
add_subdirectory(net)
add_subdirectory(supervision)
add_subdirectory(actor)
add_subdirectory(spawn)
```

- [ ] **Step 2: Create tests/integration/CMakeLists.txt**

```cmake
add_subdirectory(actor)
add_subdirectory(sched)
add_subdirectory(mailbox)
add_subdirectory(supervision)
add_subdirectory(metrics)
if(ENABLE_CLI)
    add_subdirectory(cli)
endif()
add_subdirectory(config)
add_subdirectory(log)
if(ENABLE_ACTOR_TRACING)
    add_subdirectory(tracing)
endif()
add_subdirectory(spawn)
add_subdirectory(rpc)
add_subdirectory(ref)
```

- [ ] **Step 3: Create tests/system/CMakeLists.txt**

```cmake
add_subdirectory(net)
if(ENABLE_EXAMPLES)
    add_subdirectory(examples)
endif()
```

- [ ] **Step 5: Create all empty subsystem directories**

```bash
for dir in \
    tests/unit/core tests/unit/adt tests/unit/mailbox tests/unit/sched \
    tests/unit/mem tests/unit/cli tests/unit/config tests/unit/ref \
    tests/unit/log tests/unit/tracing tests/unit/net tests/unit/supervision \
    tests/unit/actor tests/unit/spawn \
    tests/integration/actor tests/integration/sched tests/integration/mailbox \
    tests/integration/supervision tests/integration/metrics tests/integration/cli \
    tests/integration/config tests/integration/log tests/integration/tracing \
    tests/integration/spawn tests/integration/rpc tests/integration/ref \
    tests/system/net tests/system/examples; do
    mkdir -p "$dir"
    touch "$dir/.gitkeep"
done
```

- [ ] **Step 5: Stage and commit the tier scaffolding**

```bash
git add tests/unit/CMakeLists.txt tests/integration/CMakeLists.txt \
        tests/system/CMakeLists.txt tests/unit/ tests/integration/ tests/system/
git commit -m "build: add tier-level CMakeLists.txt and subsystem directory scaffolding"
```

### Task 5: Build smoke test — verify infrastructure compiles

- [ ] **Step 1: Create a minimal gtest smoke test**

Write to `tests/unit/core/test_smoke.cpp`:
```cpp
#include <gtest/gtest.h>

TEST(SmokeTest, GTestWorks) {
    EXPECT_EQ(1, 1);
}

TEST(SmokeTest, CTestDiscoversThis) {
    EXPECT_STRNE("hello", "world");
}
```

- [ ] **Step 2: Create tests/unit/core/CMakeLists.txt**

```cmake
add_executable(test_unit_core
    test_smoke.cpp
)
target_link_libraries(test_unit_core hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_core)
```

- [ ] **Step 3: Configure and build**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build test_unit_core
```

Expected: Build succeeds, `test_unit_core` binary exists at `build/tests/unit/test_unit_core`.

- [ ] **Step 4: Run the smoke test through CTest**

```bash
ctest --test-dir build -R test_unit_core --output-on-failure
```

Expected: 2 tests pass (SmokeTest.GTestWorks, SmokeTest.CTestDiscoversThis).

- [ ] **Step 5: Commit the smoke test (this file is superseded by real tests in Task 6)**

```bash
git add tests/unit/core/
git commit -m "test: add gtest smoke test to verify infrastructure"
```

---

## Phase 2: Unit Tier Migration

Each subsystem task follows this pattern:
1. Create `tests/unit/<subsys>/CMakeLists.txt` (one binary listing all sources)
2. Move test source files from old `tests/<subsys>/` to `tests/unit/<subsys>/`
3. Convert each file: remove `main()`, replace `assert()`/`CHECK()` with GTest macros, wrap tests in `TEST()` or `TEST_F()`
4. Build the binary via `ninja -C build test_unit_<subsys>`
5. Run via `ctest --test-dir build -R test_unit_<subsys> --output-on-failure`
6. Commit

### Task 6: Migrate unit/core and unit/adt

**Files:**
- Create: `tests/unit/core/CMakeLists.txt`
- Create: `tests/unit/core/test_types.cpp` (converted from `tests/core/test_types.cpp`)
- Create: `tests/unit/core/test_result.cpp` (converted from `tests/core/test_result.cpp`)
- Create: `tests/unit/adt/CMakeLists.txt`
- Create: `tests/unit/adt/test_adt_mpsc_ring_buffer.cpp` (converted)
- Remove: `tests/unit/core/test_smoke.cpp` (smoke test already verified)

- [ ] **Step 1: Convert test_types.cpp**

Read the existing file at `tests/core/test_types.cpp`, then write `tests/unit/core/test_types.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/types/types.hpp>
#include <gtest/gtest.h>

TEST(TypesTest, ActorIdDefaultConstruction) {
    hpactor::ActorId default_actor_id;
    EXPECT_EQ(default_actor_id.value(), 0);
}

TEST(TypesTest, ActorIdExplicitConstruction) {
    hpactor::ActorId explicit_actor_id(42);
    EXPECT_EQ(explicit_actor_id.value(), 42);
}

TEST(TypesTest, ActorIdEquality) {
    hpactor::ActorId actor_id1(100);
    hpactor::ActorId actor_id2(100);
    hpactor::ActorId actor_id3(200);
    EXPECT_EQ(actor_id1, actor_id2);
    EXPECT_NE(actor_id1, actor_id3);
}

TEST(TypesTest, ActorIdValueAccessor) {
    hpactor::ActorId actor_id1(100);
    EXPECT_EQ(actor_id1.value(), 100);
}

TEST(TypesTest, LocalEndpoint) {
    EXPECT_TRUE(hpactor::LocalEndpoint.is_loopback());
    EXPECT_EQ(hpactor::LocalEndpoint.port(), 0);
}

TEST(TypesTest, ActorTypeInvalid) {
    hpactor::ActorType actor_type = hpactor::InvalidActorType;
    EXPECT_EQ(actor_type, hpactor::InvalidActorType);
}

TEST(TypesTest, ErrorOk) {
    hpactor::error ok_err;
    EXPECT_TRUE(ok_err.ok());
    EXPECT_FALSE(ok_err);
}

TEST(TypesTest, ErrorWithCode) {
    hpactor::error err(42, "test error");
    EXPECT_FALSE(err.ok());
    EXPECT_TRUE(err);
    EXPECT_EQ(err.code(), 42);
    EXPECT_EQ(err.message(), "test error");
}

TEST(TypesTest, ErrorCodes) {
    EXPECT_EQ(hpactor::errors::unknown, 1);
    EXPECT_EQ(hpactor::errors::actor_down, 2);
    EXPECT_EQ(hpactor::errors::actor_not_found, 3);
    EXPECT_EQ(hpactor::errors::mailbox_full, 4);
    EXPECT_EQ(hpactor::errors::timeout, 5);
    EXPECT_EQ(hpactor::errors::user, 1000);
}

TEST(TypesTest, MessageIdUnique) {
    hpactor::MessageId id1 = hpactor::generate_message_id();
    hpactor::MessageId id2 = hpactor::generate_message_id();
    EXPECT_NE(id1, id2);
}

TEST(TypesTest, ClockOperations) {
    hpactor::Clock clock;
    hpactor::Clock::time_point tp = clock.now();
    hpactor::Clock::duration dur = hpactor::Clock::duration(100);
    hpactor::Clock::time_point tp2 = tp + dur;
    EXPECT_GT(tp2, tp);
}

TEST(TypesTest, AlarmHandle) {
    hpactor::AlarmHandle handle1;
    hpactor::AlarmHandle handle2(42);
    EXPECT_EQ(handle1.value(), 0);
    EXPECT_EQ(handle2.value(), 42);
}

TEST(TypesTest, TraceContext) {
    hpactor::TraceContext ctx;
    ctx.trace_id.bytes[15] = 1;
    ctx.span_id.bytes[7] = 2;
    ctx.flags.value = 3;
    EXPECT_TRUE(ctx.trace_id.valid());
    EXPECT_TRUE(ctx.span_id.valid());
    EXPECT_EQ(ctx.flags.value, 3);
}

TEST(TypesTest, StreamBuffer) {
    hpactor::StreamBuffer data = {1, 2, 3, 4, 5};
    EXPECT_EQ(data.size(), 5);
    EXPECT_EQ(data[0], 1);
    EXPECT_EQ(data[4], 5);
}
```

- [ ] **Step 2: Convert test_result.cpp**

Read `tests/core/test_result.cpp`, then write `tests/unit/core/test_result.cpp` using the same pattern: replace `CHECK()` with `EXPECT_*`, wrap each logical test group in a `TEST(ResultTest, ...)` block, remove `main()`.

- [ ] **Step 3: Convert test_adt_mpsc_ring_buffer.cpp**

Read `tests/adt/test_adt_mpsc_ring_buffer.cpp`, then write `tests/unit/adt/test_adt_mpsc_ring_buffer.cpp` with the same conversion pattern.

- [ ] **Step 4: Create CMakeLists.txt for core and adt**

Write `tests/unit/core/CMakeLists.txt`:
```cmake
add_executable(test_unit_core
    test_types.cpp
    test_result.cpp
)
target_link_libraries(test_unit_core hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_core)
```

Write `tests/unit/adt/CMakeLists.txt`:
```cmake
add_executable(test_unit_adt
    test_adt_mpsc_ring_buffer.cpp
)
target_link_libraries(test_unit_adt hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_adt)
```

- [ ] **Step 5: Build and run**

```bash
ninja -C build test_unit_core test_unit_adt
ctest --test-dir build -R "test_unit_core|test_unit_adt" --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add tests/unit/core/ tests/unit/adt/
git commit -m "test: migrate unit/core and unit/adt to GTest"
```

### Task 7: Migrate unit/mailbox (9 files)

**Files:**
- Create: `tests/unit/mailbox/CMakeLists.txt`
- Move + convert: 9 files from `tests/mailbox/` to `tests/unit/mailbox/`

Files to convert:
- `test_message.cpp` → `tests/unit/mailbox/test_message.cpp`
- `test_message_advanced.cpp` → `tests/unit/mailbox/test_message_advanced.cpp`
- `test_mailbox_interface.cpp` → `tests/unit/mailbox/test_mailbox_interface.cpp`
- `test_mailbox_factory.cpp` → `tests/unit/mailbox/test_mailbox_factory.cpp`
- `test_mutex_mailbox.cpp` → `tests/unit/mailbox/test_mutex_mailbox.cpp`
- `test_mailbox_stress.cpp` → `tests/unit/mailbox/test_mailbox_stress.cpp`
- `test_bounded_mailbox.cpp` → `tests/unit/mailbox/test_bounded_mailbox.cpp`
- `test_mailbox_overflow_policies.cpp` → `tests/unit/mailbox/test_mailbox_overflow_policies.cpp`
- `test_mailbox_policy.cpp` → `tests/unit/mailbox/test_mailbox_policy.cpp`

- [ ] **Step 1: Convert test_bounded_mailbox.cpp (Pattern C — mock fixture)**

Read `tests/mailbox/test_bounded_mailbox.cpp`. It defines `MockScheduler` and tests `MPSCActorMailbox` directly. Convert to a fixture:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <gtest/gtest.h>
#include <atomic>

namespace {

struct MockScheduler : public hpactor::sched::IScheduler {
    void start() override {}
    void stop() override {}
    void notify_ready(hpactor::ActorId actor, uint8_t priority,
                      int64_t deadline) override {
        last_actor = actor;
        last_priority = priority;
        last_deadline = deadline;
        notify_ready_count.fetch_add(1, std::memory_order_relaxed);
    }
    void notify_idle(hpactor::ActorId) override {}
    void yield(hpactor::ActorId actor, uint8_t priority) override {
        notify_ready(actor, priority, INT64_MAX);
    }
    hpactor::sched::TimerHandle
    schedule_after(hpactor::sched::timer_callback, int64_t) override {
        return {};
    }
    hpactor::sched::TimerHandle
    schedule_every(hpactor::sched::timer_callback, int64_t) override {
        return {};
    }
    void cancel_timer(hpactor::sched::TimerHandle) override {}
    size_t worker_count() const override { return 1; }
    bool is_running() const override { return true; }
    void register_dedicated_thread(hpactor::ActorId, int) override {}
    void register_dedicated_pool(hpactor::ActorId, uint32_t) override {}
    void unregister_dedicated(hpactor::ActorId) override {}

    std::atomic<int> notify_ready_count{0};
    hpactor::ActorId last_actor{};
    uint8_t last_priority = 255;
    int64_t last_deadline = 0;
};

class BoundedMailboxTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_.capacity.max_messages = 2;
        cfg_.high_watermark = 0.50;
        cfg_.low_watermark = 0.25;
        mb_ = std::make_unique<hpactor::mailbox::MPSCActorMailbox<hpactor::TypedMessage>>(
            hpactor::ActorId{77}, &scheduler_, cfg_);
    }

    MockScheduler scheduler_;
    hpactor::mailbox::MailboxConfig cfg_;
    std::unique_ptr<hpactor::mailbox::MPSCActorMailbox<hpactor::TypedMessage>> mb_;
};

} // namespace

TEST_F(BoundedMailboxTest, AcceptsMessageWithinCapacity) {
    hpactor::mailbox::MailboxEnvelopeMeta meta;
    meta.type_tag = hpactor::TypeTag::User;
    meta.priority = 2;
    meta.deadline_ns = 1234;

    auto r1 = mb_->try_push(
        hpactor::TypedMessage(hpactor::TypeTag::User, hpactor::StreamBuffer{1}), meta);
    EXPECT_TRUE(r1.accepted());
    // Continue testing the bounded mailbox behavior as in the original file
}
```

- [ ] **Step 2: Convert the remaining 8 mailbox test files**

For each file:
1. Read the original at `tests/mailbox/<name>.cpp`
2. Write the converted file at `tests/unit/mailbox/<name>.cpp`
3. Remove `main()`, use `TEST()`/`TEST_F()` macros
4. Replace `assert(x)` → `EXPECT_TRUE(x)`, `assert(a == b)` → `EXPECT_EQ(a, b)`, etc.
5. Replace any `#define CHECK` macros with direct `EXPECT_*` calls

- [ ] **Step 3: Create tests/unit/mailbox/CMakeLists.txt**

```cmake
add_executable(test_unit_mailbox
    test_message.cpp
    test_message_advanced.cpp
    test_mailbox_interface.cpp
    test_mailbox_factory.cpp
    test_mutex_mailbox.cpp
    test_mailbox_stress.cpp
    test_bounded_mailbox.cpp
    test_mailbox_overflow_policies.cpp
    test_mailbox_policy.cpp
)
target_link_libraries(test_unit_mailbox hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_mailbox)
```

- [ ] **Step 4: Build and run**

```bash
ninja -C build test_unit_mailbox
ctest --test-dir build -R test_unit_mailbox --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add tests/unit/mailbox/
git commit -m "test: migrate unit/mailbox (9 files) to GTest"
```

### Task 8: Migrate unit/sched (14 files)

**Files:**
- Create: `tests/unit/sched/CMakeLists.txt`
- Move + convert: 14 files from `tests/sched/` to `tests/unit/sched/`

Files to convert: `test_chaselev_deque.cpp`, `test_multi_priority_work_queue.cpp`, `test_edf_queue.cpp`, `test_a2ws.cpp`, `test_calendar_queue.cpp`, `test_coroutine_task.cpp`, `test_coroutine_frame_pool.cpp`, `test_actor_state.cpp`, `test_actor_state_transfer.cpp`, `test_mailbox_awaiter.cpp`, `test_mpsc_actor_mailbox.cpp`, `test_scheduler_control.cpp`, `test_dispatch_policy.cpp`, `test_hybrid_scheduler.cpp`

- [ ] **Step 1: Convert test_edf_queue.cpp (Pattern B — fixture with shared state)**

Read `tests/sched/test_edf_queue.cpp`. It creates an `EDFQueue` and tests push/pop/FIFO ordering. Convert to fixture:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/sched/edf_queue.hpp>
#include <hpactor/types/types.hpp>
#include <gtest/gtest.h>

namespace {

class EDFQueueTest : public ::testing::Test {
protected:
    void SetUp() override { q_.clear(); }
    hpactor::sched::EDFQueue q_;
};

} // namespace

TEST_F(EDFQueueTest, EmptyQueue) {
    EXPECT_TRUE(q_.empty());
    EXPECT_EQ(q_.size(), 0);
}

TEST_F(EDFQueueTest, PopOnEmptyReturnsFalse) {
    hpactor::sched::WorkItem item;
    item.actor = hpactor::ActorId{0};
    item.deadline_ns = 0;
    item.sequence = 0;
    EXPECT_FALSE(q_.pop(item));
}

TEST_F(EDFQueueTest, PushPopRoundtrip) {
    hpactor::sched::WorkItem in, out;
    in.actor = hpactor::ActorId{42};
    in.deadline_ns = 1000;
    in.sequence = 1;
    q_.push(1000, in);

    EXPECT_FALSE(q_.empty());
    EXPECT_EQ(q_.size(), 1);

    ASSERT_TRUE(q_.pop(out));
    EXPECT_EQ(out.actor.value(), 42);
    EXPECT_EQ(out.deadline_ns, 1000);
    EXPECT_TRUE(q_.empty());
}

TEST_F(EDFQueueTest, EDFOrderingEarlierDeadlineFirst) {
    hpactor::sched::WorkItem early, late, out;
    early.actor = hpactor::ActorId{1};
    early.deadline_ns = 100;
    early.sequence = 1;
    late.actor = hpactor::ActorId{2};
    late.deadline_ns = 200;
    late.sequence = 2;

    q_.push(200, late);
    q_.push(100, early);

    ASSERT_TRUE(q_.pop(out));
    EXPECT_EQ(out.actor.value(), 1);
    ASSERT_TRUE(q_.pop(out));
    EXPECT_EQ(out.actor.value(), 2);
}

TEST_F(EDFQueueTest, FIFOOrderingForSameDeadline) {
    hpactor::sched::WorkItem first, second, out;
    first.actor = hpactor::ActorId{10};
    first.deadline_ns = 500;
    first.sequence = 1;
    second.actor = hpactor::ActorId{20};
    second.deadline_ns = 500;
    second.sequence = 2;

    q_.push(500, second);
    q_.push(500, first);

    ASSERT_TRUE(q_.pop(out));
    EXPECT_EQ(out.actor.value(), 10);
    ASSERT_TRUE(q_.pop(out));
    EXPECT_EQ(out.actor.value(), 20);
}

TEST_F(EDFQueueTest, Peek) {
    int64_t deadline_out;
    EXPECT_FALSE(q_.peek(deadline_out));

    q_.push(300, hpactor::sched::WorkItem{hpactor::ActorId{5}, 300, 1});
    ASSERT_TRUE(q_.peek(deadline_out));
    EXPECT_EQ(deadline_out, 300);
}
```

- [ ] **Step 2: Convert the remaining 13 sched test files**

For each file at `tests/sched/<name>.cpp`:
1. `test_chaselev_deque.cpp` → Pattern B (fixture with ChaseLevDeque)
2. `test_multi_priority_work_queue.cpp` → Pattern B (fixture)
3. `test_a2ws.cpp` → Pattern B (fixture)
4. `test_calendar_queue.cpp` → Pattern B (fixture)
5. `test_coroutine_task.cpp` → Pattern A (simple TEST macros)
6. `test_coroutine_frame_pool.cpp` → Pattern B (fixture)
7. `test_actor_state.cpp` → Pattern A
8. `test_actor_state_transfer.cpp` → Pattern A or B
9. `test_mailbox_awaiter.cpp` → Pattern B (fixture with mocks)
10. `test_mpsc_actor_mailbox.cpp` → Pattern B (fixture, note: rename from `test_actor_mailbox.cpp`)
11. `test_scheduler_control.cpp` → Pattern B (fixture)
12. `test_dispatch_policy.cpp` → Pattern A
13. `test_hybrid_scheduler.cpp` → Pattern A (minimal test — just verify header is parseable)

Conversion rules for each:
- Remove `int main()` and all manual test orchestration
- Replace `assert(x)` → `EXPECT_TRUE(x)` / `ASSERT_TRUE(x)`
- Replace `assert(a == b)` → `EXPECT_EQ(a, b)`
- Replace custom `CHECK()` macro → `EXPECT_TRUE()`
- Replace `std::printf("...passed\n")` → no-op (GTest reports pass/fail)
- Wrap independent test blocks in `TEST()` or `TEST_F()` with descriptive names
- Use `ASSERT_*` (fatal) when subsequent test code depends on the assertion holding

- [ ] **Step 3: Create tests/unit/sched/CMakeLists.txt**

```cmake
add_executable(test_unit_sched
    test_chaselev_deque.cpp
    test_multi_priority_work_queue.cpp
    test_edf_queue.cpp
    test_a2ws.cpp
    test_calendar_queue.cpp
    test_coroutine_task.cpp
    test_coroutine_frame_pool.cpp
    test_actor_state.cpp
    test_actor_state_transfer.cpp
    test_mailbox_awaiter.cpp
    test_mpsc_actor_mailbox.cpp
    test_scheduler_control.cpp
    test_dispatch_policy.cpp
    test_hybrid_scheduler.cpp
)
target_link_libraries(test_unit_sched hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_sched)
```

- [ ] **Step 4: Build and run**

```bash
ninja -C build test_unit_sched
ctest --test-dir build -R test_unit_sched --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add tests/unit/sched/
git commit -m "test: migrate unit/sched (14 files) to GTest"
```

### Task 9: Migrate unit/mem (15 files)

**Files:**
- Create: `tests/unit/mem/CMakeLists.txt`
- Move + convert: 15 files from `tests/mem/` to `tests/unit/mem/`

Files: `test_size_class.cpp`, `test_alloc_header.cpp`, `test_freelist.cpp`, `test_segment_provider.cpp`, `test_slab_cache.cpp`, `test_thread_local_allocator.cpp`, `test_memory_stress.cpp`, `test_memory_tracker.cpp`, `test_telemetry_ring_buffer.cpp`, `test_memory_poisoning.cpp`, `test_hibernation.cpp`, `test_guard_page.cpp`, `test_compaction.cpp`, `test_allocator_benchmark.cpp`, `test_std_allocator.cpp`

- [ ] **Step 1: Convert all 15 mem test files**

For each file at `tests/mem/<name>.cpp`, follow the same conversion pattern:
1. Read the original
2. Write `tests/unit/mem/<name>.cpp`
3. Remove `main()`, use `TEST()`/`TEST_F()`
4. Replace `assert()` → `EXPECT_*`/`ASSERT_*`
5. Note: mem tests link `pthread` — add to CMakeLists.txt

- [ ] **Step 2: Create tests/unit/mem/CMakeLists.txt**

```cmake
add_executable(test_unit_mem
    test_size_class.cpp
    test_alloc_header.cpp
    test_freelist.cpp
    test_segment_provider.cpp
    test_slab_cache.cpp
    test_thread_local_allocator.cpp
    test_memory_stress.cpp
    test_memory_tracker.cpp
    test_telemetry_ring_buffer.cpp
    test_memory_poisoning.cpp
    test_hibernation.cpp
    test_guard_page.cpp
    test_compaction.cpp
    test_allocator_benchmark.cpp
    test_std_allocator.cpp
)
target_link_libraries(test_unit_mem hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_mem)
```

- [ ] **Step 3: Build and run**

```bash
ninja -C build test_unit_mem
ctest --test-dir build -R test_unit_mem --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add tests/unit/mem/
git commit -m "test: migrate unit/mem (15 files) to GTest"
```

### Task 10: Migrate unit/cli (6 files) and unit/config + unit/ref (3 files)

**Files:**
- Create: `tests/unit/cli/CMakeLists.txt`
- Create: `tests/unit/config/CMakeLists.txt`
- Create: `tests/unit/ref/CMakeLists.txt`
- Move + convert: `tests/cli/test_lexer.cpp`, `test_command_node.cpp`, `test_formatters.cpp`, `test_pager.cpp`, `test_line_editor.cpp`, `test_command_registry.cpp` → `tests/unit/cli/`
- Move + convert: `tests/config/test_actor_factory_registry.cpp` → `tests/unit/config/`
- Move + convert: `tests/ref/test_actor_address.cpp` → `tests/unit/ref/`

- [ ] **Step 1: Convert cli tests**

The CLI unit tests test components in isolation (no ActorSystem). Convert each:
- `test_lexer.cpp` → `TEST(LexerTest, ...)` / `TEST_F(LexerTest, ...)` 
- `test_command_node.cpp` → `TEST(CommandNodeTest, ...)`
- `test_formatters.cpp` → `TEST(FormattersTest, ...)` / `TEST_F(FormattersTest, ...)`
- `test_pager.cpp` → `TEST(PagerTest, ...)`
- `test_line_editor.cpp` → `TEST(LineEditorTest, ...)`
- `test_command_registry.cpp` → `TEST(CommandRegistryTest, ...)`

- [ ] **Step 2: Create tests/unit/cli/CMakeLists.txt**

```cmake
add_executable(test_unit_cli
    test_lexer.cpp
    test_command_node.cpp
    test_formatters.cpp
    test_pager.cpp
    test_line_editor.cpp
    test_command_registry.cpp
)
target_link_libraries(test_unit_cli hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_cli)
```

- [ ] **Step 3: Convert test_actor_factory_registry.cpp**

Only config file that is a true unit test. Wrap factory registration tests in `TEST(ConfigTest, ...)`.

- [ ] **Step 4: Create tests/unit/config/CMakeLists.txt**

```cmake
add_executable(test_unit_config
    test_actor_factory_registry.cpp
)
target_link_libraries(test_unit_config hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_config)
```

- [ ] **Step 5: Convert test_actor_address.cpp**

Wrap actor address tests in `TEST(ActorAddressTest, ...)`.

- [ ] **Step 6: Create tests/unit/ref/CMakeLists.txt**

```cmake
add_executable(test_unit_ref
    test_actor_address.cpp
)
target_link_libraries(test_unit_ref hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_ref)
```

- [ ] **Step 7: Build and run**

```bash
ninja -C build test_unit_cli test_unit_config test_unit_ref
ctest --test-dir build -R "test_unit_cli|test_unit_config|test_unit_ref" --output-on-failure
```

- [ ] **Step 8: Commit**

```bash
git add tests/unit/cli/ tests/unit/config/ tests/unit/ref/
git commit -m "test: migrate unit/cli (6), unit/config (1), unit/ref (1) to GTest"
```

### Task 11: Migrate unit/log, unit/tracing, unit/net, unit/supervision, unit/actor, unit/spawn

**Files:**
- Create: CMakeLists.txt for 6 more subsystems
- Move + convert remaining unit-tier test files

Files by subsystem:
- **unit/log (6):** test_log_level, test_log_category, test_log_config, test_log_formatter, test_log_ring_buffer, test_log_sinks
- **unit/tracing (5):** test_trace_config_smoke, test_trace_context, test_w3c_trace_context, test_sampler, test_trace_typed_message
- **unit/net (5):** test_frame, test_communication_endpoint, test_http_parser, test_http_serializer, test_registrar_serialization
- **unit/supervision (2):** test_one_for_one_supervisor, test_all_for_one_supervisor
- **unit/actor (3):** test_abstract_actor, test_lifecycle_state, test_proto_registry
- **unit/spawn (1):** test_spawn_serialization

- [ ] **Step 1: Convert and move each subsystem's files**

For each file listed above:
1. Read `tests/<old_subsys>/<name>.cpp`
2. Write converted file at `tests/unit/<new_subsys>/<name>.cpp`
3. Remove `main()`, use `TEST()`/`TEST_F()`, replace `assert()` → `EXPECT_*`/`ASSERT_*`

- [ ] **Step 2: Create CMakeLists.txt for each subsystem**

`tests/unit/log/CMakeLists.txt`:
```cmake
add_executable(test_unit_log
    test_log_level.cpp
    test_log_category.cpp
    test_log_config.cpp
    test_log_formatter.cpp
    test_log_ring_buffer.cpp
    test_log_sinks.cpp
)
target_link_libraries(test_unit_log hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_log)
```

`tests/unit/tracing/CMakeLists.txt`:
```cmake
add_executable(test_unit_tracing
    test_trace_config_smoke.cpp
    test_trace_context.cpp
    test_w3c_trace_context.cpp
    test_sampler.cpp
    test_trace_typed_message.cpp
)
target_link_libraries(test_unit_tracing hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_tracing)
```

`tests/unit/net/CMakeLists.txt`:
```cmake
add_executable(test_unit_net
    test_frame.cpp
    test_communication_endpoint.cpp
    test_http_parser.cpp
    test_http_serializer.cpp
    test_registrar_serialization.cpp
)
target_link_libraries(test_unit_net hpactor hpactor_proto hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_net)
```

`tests/unit/supervision/CMakeLists.txt`:
```cmake
add_executable(test_unit_supervision
    test_one_for_one_supervisor.cpp
    test_all_for_one_supervisor.cpp
)
target_link_libraries(test_unit_supervision hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_supervision)
```

`tests/unit/actor/CMakeLists.txt`:
```cmake
add_executable(test_unit_actor
    test_abstract_actor.cpp
    test_lifecycle_state.cpp
    test_proto_registry.cpp
)
target_link_libraries(test_unit_actor hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_actor)
```

`tests/unit/spawn/CMakeLists.txt`:
```cmake
add_executable(test_unit_spawn
    test_spawn_serialization.cpp
)
target_link_libraries(test_unit_spawn hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_unit_spawn)
```

- [ ] **Step 3: Build all unit binaries**

```bash
ninja -C build test_unit_log test_unit_tracing test_unit_net \
      test_unit_supervision test_unit_actor test_unit_spawn
```

- [ ] **Step 4: Run all unit tests**

```bash
ctest --test-dir build -R "test_unit_" --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add tests/unit/log/ tests/unit/tracing/ tests/unit/net/ \
        tests/unit/supervision/ tests/unit/actor/ tests/unit/spawn/
git commit -m "test: migrate unit/log, tracing, net, supervision, actor, spawn (22 files) to GTest"
```

---

## Phase 3: Integration Tier Migration

### Task 12: Migrate integration/actor (26 files)

**Files:**
- Create: `tests/integration/actor/CMakeLists.txt`
- Move + convert: 26 files from `tests/actor/` to `tests/integration/actor/`

Files to convert: `test_actor_system.cpp`, `test_actor_context.cpp`, `test_actor_context_schedule.cpp`, `test_actor_context_try_send.cpp`, `test_actor_mailbox.cpp`, `test_actor_system_backpressure.cpp`, `test_event_based_actor.cpp`, `test_blocking_actor.cpp`, `test_daemon_actor.cpp`, `test_stateful_actor.cpp`, `test_typed_actor.cpp`, `test_proto_stateful_actor.cpp`, `test_unified_message_passing.cpp`, `test_link_monitor.cpp`, `test_lifecycle_actor.cpp`, `test_is_system_actor.cpp`, `test_drain_policy.cpp`, `test_drain_timeout.cpp`, `test_actor_stop.cpp`, `test_shutdown_coordinator.cpp`, `test_drain_integration.cpp`, `test_scoped_actor.cpp`, `test_local_actor.cpp`, `test_spawn_receiver_construct.cpp`, `test_actor_ref_cache.cpp`, `test_backpressure_signals.cpp`

- [ ] **Step 1: Convert each actor integration test file**

For each file at `tests/actor/<name>.cpp`:
1. Read the original
2. Write `tests/integration/actor/<name>.cpp`
3. Remove `main()`, use `TEST()`/`TEST_F()`
4. Replace `assert()` → `EXPECT_*`/`ASSERT_*`
5. Integration tests may use `ActorSystem` and scheduler — GTest fixtures should manage ActorSystem lifecycle in `SetUp()`/`TearDown()`

Example fixture pattern for tests that use ActorSystem:
```cpp
class ActorIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        system_ = std::make_unique<hpactor::ActorSystem>(hpactor::ActorSystemConfig{
            .scheduler_threads = 0  // deterministic
        });
    }
    void TearDown() override {
        system_->shutdown();
    }
    std::unique_ptr<hpactor::ActorSystem> system_;
};
```

- [ ] **Step 2: Create tests/integration/actor/CMakeLists.txt**

```cmake
add_executable(test_integration_actor
    test_actor_system.cpp
    test_actor_context.cpp
    test_actor_context_schedule.cpp
    test_actor_context_try_send.cpp
    test_actor_mailbox.cpp
    test_actor_system_backpressure.cpp
    test_event_based_actor.cpp
    test_blocking_actor.cpp
    test_daemon_actor.cpp
    test_stateful_actor.cpp
    test_typed_actor.cpp
    test_proto_stateful_actor.cpp
    test_unified_message_passing.cpp
    test_link_monitor.cpp
    test_lifecycle_actor.cpp
    test_is_system_actor.cpp
    test_drain_policy.cpp
    test_drain_timeout.cpp
    test_actor_stop.cpp
    test_shutdown_coordinator.cpp
    test_drain_integration.cpp
    test_scoped_actor.cpp
    test_local_actor.cpp
    test_spawn_receiver_construct.cpp
    test_actor_ref_cache.cpp
    test_backpressure_signals.cpp
)
target_link_libraries(test_integration_actor
    hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main
)
gtest_discover_tests(test_integration_actor PROPERTIES TIMEOUT 35)
```

- [ ] **Step 3: Build and run**

```bash
ninja -C build test_integration_actor
ctest --test-dir build -R test_integration_actor --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add tests/integration/actor/
git commit -m "test: migrate integration/actor (26 files) to GTest"
```

### Task 13: Migrate integration/sched, integration/mailbox, integration/supervision

**Files:**
- Create: `tests/integration/sched/CMakeLists.txt`
- Create: `tests/integration/mailbox/CMakeLists.txt`
- Create: `tests/integration/supervision/CMakeLists.txt`

Files:
- **integration/sched (4):** test_priority_scheduler, test_worker_thread, test_dedicated_thread_pool, test_coroutine_scheduling (from `tests/sched/`)
- **integration/mailbox (2):** test_mailbox_backpressure_stress, test_dead_letter_queue (from `tests/mailbox/`)
- **integration/supervision (3):** test_supervision, test_supervisor_actor, test_self_supervising_actor (from `tests/supervision/`)

- [ ] **Step 1: Convert each file**

Same pattern as Task 12 — GTest fixtures for shared ActorSystem setup.

- [ ] **Step 2: Create CMakeLists.txt files**

`tests/integration/sched/CMakeLists.txt`:
```cmake
add_executable(test_integration_sched
    test_priority_scheduler.cpp
    test_worker_thread.cpp
    test_dedicated_thread_pool.cpp
    test_coroutine_scheduling.cpp
)
target_link_libraries(test_integration_sched hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_sched PROPERTIES TIMEOUT 15)
```

`tests/integration/mailbox/CMakeLists.txt`:
```cmake
add_executable(test_integration_mailbox
    test_mailbox_backpressure_stress.cpp
    test_dead_letter_queue.cpp
)
target_link_libraries(test_integration_mailbox hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_mailbox)
```

`tests/integration/supervision/CMakeLists.txt`:
```cmake
add_executable(test_integration_supervision
    test_supervision.cpp
    test_supervisor_actor.cpp
    test_self_supervising_actor.cpp
)
target_link_libraries(test_integration_supervision hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_supervision)
```

- [ ] **Step 3: Build and run**

```bash
ninja -C build test_integration_sched test_integration_mailbox test_integration_supervision
ctest --test-dir build -R "test_integration_sched|test_integration_mailbox|test_integration_supervision" --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add tests/integration/sched/ tests/integration/mailbox/ tests/integration/supervision/
git commit -m "test: migrate integration/sched, mailbox, supervision (9 files) to GTest"
```

### Task 14: Migrate integration/metrics, integration/cli, integration/config

**Files:**
- Create: `tests/integration/metrics/CMakeLists.txt`
- Create: `tests/integration/cli/CMakeLists.txt`
- Create: `tests/integration/config/CMakeLists.txt`

Files:
- **integration/metrics (3):** test_metrics_registry, test_metrics_aggregator, test_metrics_integration (from `tests/metrics/`)
- **integration/cli (2):** test_cli_actor, test_cli_integration (from `tests/cli/`)
- **integration/config (6):** test_toml_parser, test_toml_parser_registry, test_bootstrap_engine, test_mailbox_config, test_shutdown_config, test_binary_roundtrip (from `tests/config/`)

- [ ] **Step 1: Convert each file**

- [ ] **Step 2: Create CMakeLists.txt files**

`tests/integration/metrics/CMakeLists.txt`:
```cmake
add_executable(test_integration_metrics
    test_metrics_registry.cpp
    test_metrics_aggregator.cpp
    test_metrics_integration.cpp
)
target_link_libraries(test_integration_metrics hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_metrics)
```

`tests/integration/cli/CMakeLists.txt`:
```cmake
add_executable(test_integration_cli
    test_cli_actor.cpp
    test_cli_integration.cpp
)
target_link_libraries(test_integration_cli hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_cli)
```

`tests/integration/config/CMakeLists.txt`:
```cmake
add_executable(test_integration_config
    test_toml_parser.cpp
    test_toml_parser_registry.cpp
    test_bootstrap_engine.cpp
    test_mailbox_config.cpp
    test_shutdown_config.cpp
    test_binary_roundtrip.cpp
)
target_compile_definitions(test_integration_config PRIVATE
    TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data/toml")
target_link_libraries(test_integration_config hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_config PROPERTIES TIMEOUT 15)
```

- [ ] **Step 3: Build and run**

```bash
ninja -C build test_integration_metrics test_integration_cli test_integration_config
ctest --test-dir build -R "test_integration_metrics|test_integration_cli|test_integration_config" --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add tests/integration/metrics/ tests/integration/cli/ tests/integration/config/
git commit -m "test: migrate integration/metrics, cli, config (11 files) to GTest"
```

### Task 15: Migrate integration/log, integration/tracing, integration/spawn, integration/rpc, integration/ref

**Files:**
- Create: CMakeLists.txt for 5 more subsystems
- Move + convert remaining integration-tier files

Files by subsystem:
- **integration/log (1):** test_log_integration (from `tests/log/`)
- **integration/tracing (6):** test_trace_actor_context, test_trace_actor_system, test_trace_manager, test_trace_message_propagation, test_trace_exporters, test_trace_rpc (from `tests/tracing/`)
- **integration/spawn (4):** test_actor_type_registry, test_async_actor, test_spawn_integration, test_spawn_receiver (from `tests/spawn/`)
- **integration/rpc (1):** test_rpc_channel (from `tests/rpc/`)
- **integration/ref (3):** test_actor_ref (from `tests/test_actor_ref.cpp`), test_actor_proxy, test_actor_proxy_dead_letters (from `tests/ref/`)

- [ ] **Step 1: Convert each file**

- [ ] **Step 2: Create CMakeLists.txt files**

`tests/integration/log/CMakeLists.txt`:
```cmake
add_executable(test_integration_log
    test_log_integration.cpp
)
target_link_libraries(test_integration_log hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_log)
```

`tests/integration/tracing/CMakeLists.txt`:
```cmake
add_executable(test_integration_tracing
    test_trace_actor_context.cpp
    test_trace_actor_system.cpp
    test_trace_manager.cpp
    test_trace_message_propagation.cpp
    test_trace_exporters.cpp
    test_trace_rpc.cpp
)
target_link_libraries(test_integration_tracing hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_tracing PROPERTIES TIMEOUT 15)
```

`tests/integration/spawn/CMakeLists.txt`:
```cmake
add_executable(test_integration_spawn
    test_actor_type_registry.cpp
    test_async_actor.cpp
    test_spawn_integration.cpp
    test_spawn_receiver.cpp
)
target_link_libraries(test_integration_spawn hpactor hpactor_proto pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_spawn)
```

`tests/integration/rpc/CMakeLists.txt`:
```cmake
add_executable(test_integration_rpc
    test_rpc_channel.cpp
)
target_link_libraries(test_integration_rpc hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_rpc)
```

`tests/integration/ref/CMakeLists.txt`:
```cmake
add_executable(test_integration_ref
    test_actor_ref.cpp
    test_actor_proxy.cpp
    test_actor_proxy_dead_letters.cpp
)
target_link_libraries(test_integration_ref hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_integration_ref)
```

- [ ] **Step 3: Build and run**

```bash
ninja -C build test_integration_log test_integration_tracing \
      test_integration_spawn test_integration_rpc test_integration_ref
ctest --test-dir build -R "test_integration_" --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add tests/integration/log/ tests/integration/tracing/ tests/integration/spawn/ \
        tests/integration/rpc/ tests/integration/ref/
git commit -m "test: migrate integration/log, tracing, spawn, rpc, ref (15 files) to GTest"
```

---

## Phase 4: System Tier Migration

### Task 16: Migrate system/net (20 files)

**Files:**
- Create: `tests/system/net/CMakeLists.txt`
- Move + convert: 20 files from `tests/net/` to `tests/system/net/`

Files: `test_async_io_backend.cpp`, `test_event_loop.cpp`, `test_connection.cpp`, `test_connection_pool.cpp`, `test_tls_context.cpp`, `test_tls_connection.cpp`, `test_tls_integration.cpp`, `test_registrar.cpp`, `test_registrar_connection.cpp`, `test_unix_domain_socket.cpp`, `test_uds_integration.cpp`, `test_tcp_transport_comprehensive.cpp`, `test_reactor_dispatcher.cpp`, `test_proactor_dispatcher.cpp`, `test_gossip_membership.cpp`, `test_service_discovery.cpp`, `test_hybrid_discovery.cpp`, `test_http_connection.cpp`, `test_http_gateway.cpp`, `test_iouring_backend.cpp`

- [ ] **Step 1: Handle test_gossip_membership.cpp — `#define private public` → `FRIEND_TEST`**

Read the original file at `tests/net/test_gossip_membership.cpp` (uses `#define private public`).

Read the production header at `include/hpactor/net/gossip_membership.hpp`.

In the production header, add `FRIEND_TEST` declarations for each test that accesses private members:

```cpp
#include <gtest/gtest_prod.h>

class GossipMembership {
    // Allow unit tests of internal SWIM protocol methods
    FRIEND_TEST(GossipMembershipTest, ConstructionWithDefaults);
    FRIEND_TEST(GossipMembershipTest, BootstrapSoloCluster);
    FRIEND_TEST(GossipMembershipTest, AnnounceBumpsIncarnation);
    FRIEND_TEST(GossipMembershipTest, DiscoverAllReturnsCopy);
    FRIEND_TEST(GossipMembershipTest, MergeMemberNoneToAlive);
    FRIEND_TEST(GossipMembershipTest, MergeMemberHigherIncarnation);
    FRIEND_TEST(GossipMembershipTest, MergeMemberLowerIncarnationStale);
    FRIEND_TEST(GossipMembershipTest, MergeMemberDeadToAliveReincarnation);
    FRIEND_TEST(GossipMembershipTest, MarkSuspiciousMarkDeadTransitions);
    FRIEND_TEST(GossipMembershipTest, PickRandomPeersAllAvailable);
    FRIEND_TEST(GossipMembershipTest, PickRandomPeersSoloCluster);
    FRIEND_TEST(GossipMembershipTest, PurgeDeadTombstones);
    FRIEND_TEST(GossipMembershipTest, WireEncodeDecodePingWithPiggyback);
    FRIEND_TEST(GossipMembershipTest, MetadataPiggybackEncodeDecodeRoundtrip);
    // ...
};
```

Then in the test file, replace `#define private public` / `#include` / `#undef private` with a normal include and `TEST_F(GossipMembershipTest, ...)` macros.

- [ ] **Step 2: Convert remaining system net test files**

For each test at `tests/net/<name>.cpp` (excluding the 5 already in unit/net):
1. Read the original
2. Write `tests/system/net/<name>.cpp`
3. Remove `main()`, use `TEST()`/`TEST_F()`
4. Replace `assert()` → `EXPECT_*`/`ASSERT_*`
5. System tests that use real sockets must use non-blocking fds (per CLAUDE.md test constraints)

- [ ] **Step 3: Create tests/system/net/CMakeLists.txt**

```cmake
add_executable(test_system_net
    test_async_io_backend.cpp
    test_event_loop.cpp
    test_connection.cpp
    test_connection_pool.cpp
    test_tls_context.cpp
    test_tls_connection.cpp
    test_tls_integration.cpp
    test_registrar.cpp
    test_registrar_connection.cpp
    test_unix_domain_socket.cpp
    test_uds_integration.cpp
    test_tcp_transport_comprehensive.cpp
    test_reactor_dispatcher.cpp
    test_proactor_dispatcher.cpp
    test_gossip_membership.cpp
    test_service_discovery.cpp
    test_hybrid_discovery.cpp
    test_http_connection.cpp
    test_http_gateway.cpp
    test_iouring_backend.cpp
)
target_link_libraries(test_system_net hpactor pthread hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_system_net PROPERTIES TIMEOUT 30)
```

- [ ] **Step 4: Build and run**

```bash
ninja -C build test_system_net
ctest --test-dir build -R test_system_net --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add tests/system/net/ include/hpactor/net/gossip_membership.hpp
git commit -m "test: migrate system/net (20 files) to GTest, add FRIEND_TEST to GossipMembership"
```

### Task 17: Migrate system/examples (1 file)

**Files:**
- Create: `tests/system/examples/CMakeLists.txt`
- Move + convert: `tests/examples/test_order_platform_messages.cpp` → `tests/system/examples/test_order_platform_messages.cpp`

- [ ] **Step 1: Convert test_order_platform_messages.cpp**

Read the original, convert to GTest, place at new path.

- [ ] **Step 2: Create tests/system/examples/CMakeLists.txt**

```cmake
add_executable(test_system_examples
    test_order_platform_messages.cpp
)
target_include_directories(test_system_examples PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_system_examples hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_system_examples PROPERTIES TIMEOUT 15)
```

- [ ] **Step 3: Build and run**

```bash
ninja -C build test_system_examples
ctest --test-dir build -R test_system_examples --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add tests/system/examples/
git commit -m "test: migrate system/examples (1 file) to GTest"
```

---

## Phase 5: Cleanup and Verification

### Task 18: Remove old test directories and files

- [ ] **Step 1: Delete old test directories**

```bash
rm -rf tests/actor tests/adt tests/cli tests/config tests/core \
       tests/examples tests/log tests/mailbox tests/mem tests/metrics \
       tests/net tests/ref tests/rpc tests/sched tests/spawn \
       tests/supervision tests/tracing
rm -f tests/test_actor_ref.cpp
rm -f tests/spawn/CMakeLists.txt 2>/dev/null || true
rm -f tests/CMakeLists.txt.old
```

- [ ] **Step 2: Remove .gitkeep files (no longer needed)**

```bash
find tests/unit tests/integration tests/system -name ".gitkeep" -delete
```

- [ ] **Step 3: Commit**

```bash
git add -A tests/
git commit -m "chore: remove old test directory structure"
```

### Task 19: Full build and verification

- [ ] **Step 1: Clean rebuild from scratch**

```bash
rm -rf build
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
```

Expected: Zero build errors.

- [ ] **Step 2: Run all tests through CTest**

```bash
ctest --test-dir build --output-on-failure --parallel 8
```

Expected: All 152 tests pass (100% pass rate). Each individual `TEST()` from the GTest suites appears as a separate CTest test.

- [ ] **Step 3: Verify CTest test count**

```bash
ctest --test-dir build -N | wc -l
```

Expected: More than 152 individual test cases (each `TEST()` macro registers as one CTest test, so the count should be significantly higher than the 152 files).

- [ ] **Step 4: Verify GTest filtering works**

```bash
# Direct GTest binary filtering
./build/tests/unit/test_unit_core --gtest_filter="*ActorId*"
```

Expected: Only ActorId-related tests run.

- [ ] **Step 5: Commit any final fixes**

```bash
git add -A
git commit -m "chore: final verification — all 152 tests passing under GTest"
```

### Task 20: Update CLAUDE_MEMORY.md and CLAUDE.md

- [ ] **Step 1: Update CLAUDE_MEMORY.md**

Update the test count and directory references to reflect the new structure:
- Change "152 test source files across 16 subdirectories" → reflect the three-tier structure
- Update build commands section to reference GTest filtering

- [ ] **Step 2: Update CLAUDE.md important files section**

Update the tests description and add GTest reference.

- [ ] **Step 3: Commit**

```bash
git add CLAUDE_MEMORY.md CLAUDE.md
git commit -m "docs: update project memory for GTest test reorganization"
```

---

## Summary

| Phase | Tasks | Files |
|-------|-------|-------|
| 1. Infrastructure | 1-5 | GTest vendor, cmake module, tier scaffolding |
| 2. Unit migration | 6-11 | 91 files → 14 binaries |
| 3. Integration migration | 12-15 | 39 files → 11 binaries |
| 4. System migration | 16-17 | 22 files → 2 binaries |
| 5. Cleanup + verify | 18-20 | Remove old dirs, full build, update docs |

**Total:** 20 tasks, 152 test files reorganized into 27 binaries across 3 tiers.
