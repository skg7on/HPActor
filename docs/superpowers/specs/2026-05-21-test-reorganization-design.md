# Test Reorganization: Unit/Integration/System Split with Google Test

**Date:** 2026-05-21
**Status:** Design — awaiting implementation plan
**Scope:** Reorganize all 152 test files into three tiers, vendor Google Test in `third_party/`, convert all tests from raw `assert()` to GTest macros, and restructure the CMake build from a 665-line monolithic file to a hierarchical tree.

## Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Tier model | Three-tier: unit / integration / system | Tests span isolated data structures through cross-process network scenarios — two tiers would blur the middle |
| GTest source | Vendored in `third_party/googletest/` | User preference; matches project convention for third-party deps |
| Binary granularity | One executable per subsystem per tier | Reduces link duplication vs one-per-file; finer ctest filtering than one monolithic binary |
| Migration scope | Full conversion of all 152 files | No mixed-style maintenance burden |

## Directory Structure

```
tests/
├── CMakeLists.txt                   # Root: shared config, gtest include, add_subdirectory per tier
├── support/                         # Shared test headers (scheduler_test_driver.hpp)
├── data/                            # Test data fixtures (TOML files, etc.)
├── unit/
│   ├── CMakeLists.txt
│   ├── core/CMakeLists.txt          # test_unit_core
│   ├── adt/CMakeLists.txt           # test_unit_adt
│   ├── mailbox/CMakeLists.txt       # test_unit_mailbox
│   ├── sched/CMakeLists.txt         # test_unit_sched
│   ├── mem/CMakeLists.txt           # test_unit_mem
│   ├── cli/CMakeLists.txt           # test_unit_cli (guarded: ENABLE_CLI)
│   ├── config/CMakeLists.txt        # test_unit_config
│   ├── ref/CMakeLists.txt           # test_unit_ref
│   ├── log/CMakeLists.txt           # test_unit_log
│   ├── tracing/CMakeLists.txt       # test_unit_tracing (guarded: ENABLE_ACTOR_TRACING)
│   ├── net/CMakeLists.txt           # test_unit_net
│   ├── supervision/CMakeLists.txt   # test_unit_supervision
│   ├── actor/CMakeLists.txt         # test_unit_actor
│   └── spawn/CMakeLists.txt         # test_unit_spawn
├── integration/
│   ├── CMakeLists.txt
│   ├── actor/CMakeLists.txt         # test_integration_actor
│   ├── sched/CMakeLists.txt         # test_integration_sched
│   ├── mailbox/CMakeLists.txt       # test_integration_mailbox
│   ├── supervision/CMakeLists.txt   # test_integration_supervision
│   ├── metrics/CMakeLists.txt       # test_integration_metrics
│   ├── cli/CMakeLists.txt           # test_integration_cli (guarded: ENABLE_CLI)
│   ├── config/CMakeLists.txt        # test_integration_config
│   ├── log/CMakeLists.txt           # test_integration_log
│   ├── tracing/CMakeLists.txt       # test_integration_tracing (guarded)
│   ├── spawn/CMakeLists.txt         # test_integration_spawn
│   ├── rpc/CMakeLists.txt           # test_integration_rpc
│   └── ref/CMakeLists.txt           # test_integration_ref
└── system/
    ├── CMakeLists.txt
    ├── net/CMakeLists.txt           # test_system_net
    └── examples/CMakeLists.txt      # test_system_examples
```

## Tier Classification Criteria

| Criteria | Unit | Integration | System |
|----------|------|-------------|--------|
| ActorSystem | No | Yes | Yes |
| Scheduler threads | No | Yes (or scheduler_threads=0) | Yes |
| Real network/fds | No | No | Yes |
| Filesystem data files | No | Yes (TOML fixtures) | Yes |
| Multiple processes | No | No | Yes |
| Mock/stub dependencies | Allowed | Not used | Not used |

## File Classification

### Unit (91 files) — no ActorSystem, scheduler threads, network, or filesystem

| Binary | Files |
|--------|-------|
| test_unit_core | test_types, test_result |
| test_unit_adt | test_adt_mpsc_ring_buffer |
| test_unit_mailbox | test_message, test_message_advanced, test_mailbox_interface, test_mailbox_factory, test_mutex_mailbox, test_mailbox_stress, test_bounded_mailbox, test_mailbox_overflow_policies, test_mailbox_policy |
| test_unit_sched | test_chaselev_deque, test_multi_priority_work_queue, test_edf_queue, test_a2ws, test_calendar_queue, test_coroutine_task, test_coroutine_frame_pool, test_actor_state, test_actor_state_transfer, test_mailbox_awaiter, test_mpsc_actor_mailbox, test_scheduler_control, test_dispatch_policy, test_hybrid_scheduler |
| test_unit_mem | test_size_class, test_alloc_header, test_freelist, test_segment_provider, test_slab_cache, test_thread_local_allocator, test_memory_stress, test_memory_tracker, test_telemetry_ring_buffer, test_memory_poisoning, test_hibernation, test_guard_page, test_compaction, test_allocator_benchmark, test_std_allocator |
| test_unit_cli | test_lexer, test_command_node, test_formatters, test_pager, test_line_editor, test_command_registry |
| test_unit_config | test_actor_factory_registry |
| test_unit_ref | test_actor_address |
| test_unit_log | test_log_level, test_log_category, test_log_config, test_log_formatter, test_log_ring_buffer, test_log_sinks |
| test_unit_tracing | test_trace_config_smoke, test_trace_context, test_w3c_trace_context, test_sampler, test_trace_typed_message |
| test_unit_net | test_frame, test_communication_endpoint, test_http_parser, test_http_serializer, test_registrar_serialization |
| test_unit_supervision | test_one_for_one_supervisor, test_all_for_one_supervisor |
| test_unit_actor | test_abstract_actor, test_lifecycle_state, test_proto_registry |
| test_unit_spawn | test_spawn_serialization |

### Integration (39 files) — uses ActorSystem, scheduler, multi-component, or filesystem test data

| Binary | Files |
|--------|-------|
| test_integration_actor | test_actor_system, test_actor_context, test_actor_context_schedule, test_actor_context_try_send, test_actor_mailbox, test_actor_system_backpressure, test_event_based_actor, test_blocking_actor, test_daemon_actor, test_stateful_actor, test_typed_actor, test_proto_stateful_actor, test_unified_message_passing, test_link_monitor, test_lifecycle_actor, test_is_system_actor, test_drain_policy, test_drain_timeout, test_actor_stop, test_shutdown_coordinator, test_drain_integration, test_scoped_actor, test_local_actor, test_spawn_receiver_construct, test_actor_ref_cache, test_backpressure_signals |
| test_integration_sched | test_priority_scheduler, test_worker_thread, test_dedicated_thread_pool, test_coroutine_scheduling |
| test_integration_mailbox | test_mailbox_backpressure_stress, test_dead_letter_queue |
| test_integration_supervision | test_supervision, test_supervisor_actor, test_self_supervising_actor |
| test_integration_metrics | test_metrics_registry, test_metrics_aggregator, test_metrics_integration |
| test_integration_cli | test_cli_actor, test_cli_integration |
| test_integration_config | test_toml_parser, test_toml_parser_registry, test_bootstrap_engine, test_mailbox_config, test_shutdown_config, test_binary_roundtrip |
| test_integration_log | test_log_integration |
| test_integration_tracing | test_trace_actor_context, test_trace_actor_system, test_trace_manager, test_trace_message_propagation, test_trace_exporters, test_trace_rpc |
| test_integration_spawn | test_actor_type_registry, test_async_actor, test_spawn_integration, test_spawn_receiver |
| test_integration_rpc | test_rpc_channel |
| test_integration_ref | test_actor_ref, test_actor_proxy, test_actor_proxy_dead_letters |

### System (22 files) — real network, cross-process, end-to-end

| Binary | Files |
|--------|-------|
| test_system_net | test_async_io_backend, test_event_loop, test_connection, test_connection_pool, test_tls_context, test_tls_connection, test_tls_integration, test_registrar, test_registrar_connection, test_unix_domain_socket, test_uds_integration, test_tcp_transport_comprehensive, test_reactor_dispatcher, test_proactor_dispatcher, test_gossip_membership, test_service_discovery, test_hybrid_discovery, test_http_connection, test_http_gateway, test_iouring_backend |
| test_system_examples | test_order_platform_messages |

### File moves

- `tests/test_actor_ref.cpp` → `tests/integration/ref/test_actor_ref.cpp`
- `tests/sched/test_actor_mailbox.cpp` → `tests/unit/sched/test_mpsc_actor_mailbox.cpp` (rename for disambiguation from `tests/integration/actor/test_actor_mailbox.cpp`)
- `tests/spawn/CMakeLists.txt` → dissolved; spawn tests distributed to unit + integration tiers
- `tests/actor/test_proto_registry.cpp` → `tests/unit/actor/test_proto_registry.cpp`
- `tests/supervision/test_supervision.cpp` → `tests/integration/supervision/test_supervision.cpp`
- `tests/config/test_toml_parser.cpp` → `tests/integration/config/test_toml_parser.cpp`
- `tests/config/test_toml_parser_registry.cpp` → `tests/integration/config/test_toml_parser_registry.cpp`

## Google Test Vendoring

### Layout

```
third_party/googletest/             # Full Google Test source
cmake/gtest.cmake                   # CMake module exposing GTest targets
```

### cmake/gtest.cmake

```cmake
# Google Test — vendored in third_party/googletest
# Exposes: GTest::gtest (library), GTest::gtest_main (library + main())

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)

add_subdirectory(
    ${CMAKE_SOURCE_DIR}/third_party/googletest
    ${CMAKE_BINARY_DIR}/googletest
    EXCLUDE_FROM_ALL
)
```

## CMake Design

### Root tests/CMakeLists.txt

```cmake
add_compile_options(-UNDEBUG)

add_library(hpactor_test_support INTERFACE)
target_include_directories(hpactor_test_support INTERFACE
    ${CMAKE_SOURCE_DIR}/tests/support)

include(${CMAKE_SOURCE_DIR}/cmake/gtest.cmake)

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(system)
```

Replaces the current 665-line monolithic file. Each tier and subsystem is a short `CMakeLists.txt`:

- Tier-level files contain only `add_subdirectory()` calls (optionally guarded by `if(ENABLE_CLI)` etc.)
- Subsystem files define one test executable, list its sources, link dependencies, and call `gtest_discover_tests()`

### Subsystem CMakeLists.txt pattern

```cmake
add_executable(test_unit_mailbox
    test_message.cpp
    test_message_advanced.cpp
    # ...
)
target_link_libraries(test_unit_mailbox
    hpactor hpactor_test_support GTest::gtest_main
)
gtest_discover_tests(test_unit_mailbox)
```

Variations:
- Extra link deps: add `hpactor_proto pthread` for tests that need protobuf or threading
- TIMEOUT: add `gtest_discover_tests(test_integration_actor PROPERTIES TIMEOUT 35)` for long-running tests
- TEST_DATA_DIR: add `target_compile_definitions(test_unit_config PRIVATE TEST_DATA_DIR="...")` for TOML parser tests
- GUARDS: wrap `add_subdirectory(cli)` in `if(ENABLE_CLI)`, `add_subdirectory(tracing)` in `if(ENABLE_ACTOR_TRACING)`

### Build target naming

| Tier | Pattern | Example |
|------|---------|---------|
| Unit | `test_unit_<subsystem>` | `test_unit_mailbox` |
| Integration | `test_integration_<subsystem>` | `test_integration_actor` |
| System | `test_system_<subsystem>` | `test_system_net` |

## Conversion Patterns (assert → GTest)

### Pattern A: Simple assertions (no shared state)

```cpp
// Before
void test_actor_id_default() {
    ActorId id;
    assert(id.node == 0);
}
int main() {
    test_actor_id_default();
    return 0;
}

// After
TEST(TypesTest, ActorIdDefault) {
    ActorId id;
    EXPECT_EQ(id.node, 0);
}
```

### Pattern B: Shared state (use fixture)

```cpp
// Before
int main() {
    EDFQueue<int> q(16);
    { /* test 1 using q */ }
    { /* test 2 using q */ }
}

// After
class EDFQueueTest : public ::testing::Test {
protected:
    void SetUp() override { q_ = EDFQueue<int>(16); }
    EDFQueue<int> q_;
};
TEST_F(EDFQueueTest, PushPop) { /* ... */ }
TEST_F(EDFQueueTest, FifoTiebreaker) { /* ... */ }
```

### Pattern C: Mock dependencies (use fixture members)

```cpp
// Before: MockScheduler declared at file scope, instantiated in main()
struct MockScheduler : public IScheduler { ... };

// After: MockScheduler as fixture member
class BoundedMailboxTest : public ::testing::Test {
protected:
    void SetUp() override {
        mb_ = std::make_unique<MPSCActorMailbox<TypedMessage>>(
            ActorId{77}, &scheduler_, cfg_);
    }
    MockScheduler scheduler_;
    MailboxConfig cfg_{/* ... */};
    std::unique_ptr<MPSCActorMailbox<TypedMessage>> mb_;
};
```

### Pattern D: `#define private public` → `FRIEND_TEST`

```cpp
// Before (in test file)
#define private public
#include <hpactor/net/gossip_membership.hpp>
#undef private

// After (in production header)
class GossipMembership {
    FRIEND_TEST(GossipMembershipTest, MergeMemberUpdatesStatus);
};
```

### Assertion mapping

| Raw assert | GTest |
|------------|-------|
| `assert(cond)` | `ASSERT_TRUE(cond)` (fatal) or `EXPECT_TRUE(cond)` (non-fatal) |
| `assert(a == b)` | `EXPECT_EQ(a, b)` |
| `assert(a != b)` | `EXPECT_NE(a, b)` |
| `assert(a < b)` | `EXPECT_LT(a, b)` |
| `assert(ptr)` | `EXPECT_NE(ptr, nullptr)` |
| `assert(!ptr)` | `EXPECT_EQ(ptr, nullptr)` |
| `static_assert(...)` | Keep as `static_assert` (compile-time, no change) |

Default to `EXPECT_*` (continues on failure) unless the test cannot meaningfully continue after the failure, then use `ASSERT_*`.

## CTest Integration

`gtest_discover_tests()` registers each `TEST()` macro as an individual CTest test, enabling:

```bash
ctest --output-on-failure --parallel 8     # All individual TEST()s
ctest -R BoundedMailbox                    # Filter by GTest suite/case name
ctest -R test_unit_mailbox                 # All tests in one binary
ctest --repeat-until-fail 10               # Flake hunting
./build/tests/unit/test_unit_mailbox --gtest_filter="*Accepts*"
```

## What Does Not Change

- CTest as the test runner
- `-UNDEBUG` compilation flag (ensures asserts fire in all build types)
- Test design constraints from CLAUDE.md (no timing assumptions, deterministic, scheduler_threads=0 for state inspection, inject_for_test() for mailbox seeding)
- `scheduler_test_driver.hpp` at `tests/support/`
- Test data fixtures at `tests/data/`
- Build commands: `cmake -S . -B build -GNinja && ninja -C build && ctest --output-on-failure --parallel 8`

## Old Paths Removed

The existing flat test directories are removed:
- `tests/actor/`, `tests/adt/`, `tests/cli/`, `tests/config/`, `tests/core/`, `tests/examples/`, `tests/log/`, `tests/mailbox/`, `tests/mem/`, `tests/metrics/`, `tests/net/`, `tests/ref/`, `tests/rpc/`, `tests/sched/`, `tests/spawn/`, `tests/supervision/`, `tests/tracing/`
- `tests/test_actor_ref.cpp`
- `tests/spawn/CMakeLists.txt`

The existing monolithic `tests/CMakeLists.txt` is replaced.
