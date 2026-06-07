# Code Organization Reorganization — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reorganize 22 headers and 10 source files across 3 new directories (`timer/`, `coroutine/`, `actor/lifecycle/`) and update all ~90 include references atomically.

**Architecture:** Atomic file moves followed by grep-assisted include path updates across include/, src/, tests/, examples/, and apps/. CMakeLists.txt updated with new source paths. Full build + test verification. Single commit, no forwarding headers.

**Tech Stack:** C++20, CMake/Ninja, git

---

## Task 1: Create new directories and move headers

**Files:**
- Create: `include/hpactor/timer/`
- Create: `include/hpactor/coroutine/`
- Create: `include/hpactor/actor/lifecycle/`

- [ ] **Step 1: Create target directories**

```bash
mkdir -p include/hpactor/timer
mkdir -p include/hpactor/coroutine
mkdir -p include/hpactor/actor/lifecycle
mkdir -p src/timer
mkdir -p src/coroutine
mkdir -p src/actor/lifecycle
```

- [ ] **Step 2: Move timer headers**

```bash
git mv include/hpactor/sched/timing_wheel.hpp include/hpactor/timer/timing_wheel.hpp
git mv include/hpactor/sched/calendar_queue.hpp include/hpactor/timer/calendar_queue.hpp
```

- [ ] **Step 3: Move coroutine headers**

```bash
git mv include/hpactor/sched/coroutine_task.hpp include/hpactor/coroutine/coroutine_task.hpp
git mv include/hpactor/sched/coroutine_awaiters.hpp include/hpactor/coroutine/coroutine_awaiters.hpp
git mv include/hpactor/sched/coroutine_frame_pool.hpp include/hpactor/coroutine/coroutine_frame_pool.hpp
git mv include/hpactor/sched/actor_coroutine.hpp include/hpactor/coroutine/actor_coroutine.hpp
git mv include/hpactor/sched/yield_awaiter.hpp include/hpactor/coroutine/yield_awaiter.hpp
```

- [ ] **Step 4: Move lifecycle headers**

```bash
git mv include/hpactor/actor/lifecycle_actor.hpp include/hpactor/actor/lifecycle/lifecycle_actor.hpp
git mv include/hpactor/actor/lifecycle_state.hpp include/hpactor/actor/lifecycle/lifecycle_state.hpp
git mv include/hpactor/actor/shutdown_phase.hpp include/hpactor/actor/lifecycle/shutdown_phase.hpp
git mv include/hpactor/actor/shutdown_coordinator.hpp include/hpactor/actor/lifecycle/shutdown_coordinator.hpp
git mv include/hpactor/actor/passivation_manager.hpp include/hpactor/actor/lifecycle/passivation_manager.hpp
git mv include/hpactor/actor/passivation_config.hpp include/hpactor/actor/lifecycle/passivation_config.hpp
git mv include/hpactor/actor/drain_policy.hpp include/hpactor/actor/lifecycle/drain_policy.hpp
git mv include/hpactor/actor/drain_config.hpp include/hpactor/actor/lifecycle/drain_config.hpp
git mv include/hpactor/actor/quarantine_policy.hpp include/hpactor/actor/lifecycle/quarantine_policy.hpp
git mv include/hpactor/actor/quarantine_reason.hpp include/hpactor/actor/lifecycle/quarantine_reason.hpp
git mv include/hpactor/actor/circuit_breaker.hpp include/hpactor/actor/lifecycle/circuit_breaker.hpp
git mv include/hpactor/actor/failure_rate_tracker.hpp include/hpactor/actor/lifecycle/failure_rate_tracker.hpp
```

- [ ] **Step 5: Move delivery headers to mailbox/**

```bash
git mv include/hpactor/actor/backpressure_coordinator.hpp include/hpactor/mailbox/backpressure_coordinator.hpp
git mv include/hpactor/actor/local_delivery_engine.hpp include/hpactor/mailbox/local_delivery_engine.hpp
git mv include/hpactor/actor/memory_pressure_monitor.hpp include/hpactor/mailbox/memory_pressure_monitor.hpp
```

- [ ] **Step 6: Move durable header into durable/ subdirectory**

```bash
git mv include/hpactor/actor/durable_actor.hpp include/hpactor/actor/durable/durable_actor.hpp
```

- [ ] **Step 7: Move source files**

```bash
# Timer sources
git mv src/sched/timing_wheel.cpp src/timer/timing_wheel.cpp
git mv src/adt/calendar_queue.cpp src/timer/calendar_queue.cpp

# Coroutine sources
git mv src/sched/coroutine_frame_pool.cpp src/coroutine/coroutine_frame_pool.cpp

# Mailbox (delivery) sources
git mv src/actor/backpressure_coordinator.cpp src/mailbox/backpressure_coordinator.cpp
git mv src/actor/local_delivery_engine.cpp src/mailbox/local_delivery_engine.cpp
git mv src/actor/memory_pressure_monitor.cpp src/mailbox/memory_pressure_monitor.cpp

# Lifecycle sources
git mv src/actor/lifecycle_actor.cpp src/actor/lifecycle/lifecycle_actor.cpp
git mv src/actor/passivation_manager.cpp src/actor/lifecycle/passivation_manager.cpp
git mv src/actor/quarantine_reason.cpp src/actor/lifecycle/quarantine_reason.cpp
git mv src/actor/shutdown_coordinator.cpp src/actor/lifecycle/shutdown_coordinator.cpp
```

---

## Task 2: Update include paths in moved headers (internal references)

**Files:**
- Modify: `include/hpactor/sched/coroutine_awaiters.hpp:20` — includes `sched/coroutine_task.hpp`
- Modify: `include/hpactor/sched/yield_awaiter.hpp:18` — includes `sched/coroutine_task.hpp`
- Modify: `include/hpactor/sched/worker_thread.hpp:18` — includes `sched/coroutine_frame_pool.hpp`
- Modify: `include/hpactor/coroutine/actor_coroutine.hpp:18` — includes `sched/coroutine_task.hpp` (now `coroutine/coroutine_task.hpp`)
- Modify: `include/hpactor/actor/lifecycle/passivation_manager.hpp:17` — includes `actor/passivation_config.hpp`
- Modify: `include/hpactor/actor/lifecycle/lifecycle_actor.hpp:20-22` — includes `actor/drain_config.hpp`, `actor/lifecycle_state.hpp`, `actor/quarantine_reason.hpp`
- Modify: `include/hpactor/actor/lifecycle/shutdown_coordinator.hpp:20` — includes `actor/shutdown_phase.hpp`
- Modify: `include/hpactor/actor/lifecycle/drain_config.hpp:18` — includes `actor/drain_policy.hpp`

Each edit follows the same pattern — update the include path from old location to new location while preserving everything else about the line.

- [ ] **Step 1: Fix include in `coroutine_awaiters.hpp`**

File: `include/hpactor/sched/coroutine_awaiters.hpp`, line 20
```
old: #include <hpactor/sched/coroutine_task.hpp>
new: #include <hpactor/coroutine/coroutine_task.hpp>
```

- [ ] **Step 2: Fix include in `yield_awaiter.hpp`**

File: `include/hpactor/sched/yield_awaiter.hpp`, line 18
```
old: #include <hpactor/sched/coroutine_task.hpp>
new: #include <hpactor/coroutine/coroutine_task.hpp>
```

- [ ] **Step 3: Fix include in `worker_thread.hpp`**

File: `include/hpactor/sched/worker_thread.hpp`, line 18
```
old: #include <hpactor/sched/coroutine_frame_pool.hpp>
new: #include <hpactor/coroutine/coroutine_frame_pool.hpp>
```

- [ ] **Step 4: Fix include in `actor_coroutine.hpp`** (now at new location)

File: `include/hpactor/coroutine/actor_coroutine.hpp`, line 18
```
old: #include <hpactor/sched/coroutine_task.hpp>
new: #include <hpactor/coroutine/coroutine_task.hpp>
```

- [ ] **Step 5: Fix includes in `passivation_manager.hpp`** (now at new location)

File: `include/hpactor/actor/lifecycle/passivation_manager.hpp`, line 17
```
old: #include <hpactor/actor/passivation_config.hpp>
new: #include <hpactor/actor/lifecycle/passivation_config.hpp>
```

- [ ] **Step 6: Fix includes in `lifecycle_actor.hpp`** (now at new location)

File: `include/hpactor/actor/lifecycle/lifecycle_actor.hpp`, lines 20-22
```
old: #include <hpactor/actor/drain_config.hpp>
     #include <hpactor/actor/lifecycle_state.hpp>
     #include <hpactor/actor/quarantine_reason.hpp>
new: #include <hpactor/actor/lifecycle/drain_config.hpp>
     #include <hpactor/actor/lifecycle/lifecycle_state.hpp>
     #include <hpactor/actor/lifecycle/quarantine_reason.hpp>
```

- [ ] **Step 7: Fix include in `shutdown_coordinator.hpp`** (now at new location)

File: `include/hpactor/actor/lifecycle/shutdown_coordinator.hpp`, line 20
```
old: #include <hpactor/actor/shutdown_phase.hpp>
new: #include <hpactor/actor/lifecycle/shutdown_phase.hpp>
```

- [ ] **Step 8: Fix include in `drain_config.hpp`** (now at new location)

File: `include/hpactor/actor/lifecycle/drain_config.hpp`, line 18
```
old: #include <hpactor/actor/drain_policy.hpp>
new: #include <hpactor/actor/lifecycle/drain_policy.hpp>
```

---

## Task 3: Update include paths in `include/hpactor/sched/` headers

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp:23,26` — includes `sched/calendar_queue.hpp` and `sched/timing_wheel.hpp`

- [ ] **Step 1: Fix scheduler.hpp includes**

File: `include/hpactor/sched/scheduler.hpp`, lines 23 and 26
```
old: #include <hpactor/sched/calendar_queue.hpp>
     ...
     #include <hpactor/sched/timing_wheel.hpp>
new: #include <hpactor/timer/calendar_queue.hpp>
     ...
     #include <hpactor/timer/timing_wheel.hpp>
```

---

## Task 4: Update include paths in `include/hpactor/actor/` headers

**Files:**
- Modify: `include/hpactor/actor/event_based_actor.hpp:18-22,39-41` — lifecycle + coroutine includes
- Modify: `include/hpactor/actor/actor_route.hpp:17-18` — lifecycle + passivation includes

- [ ] **Step 1: Fix lifecycle includes in `event_based_actor.hpp`**

File: `include/hpactor/actor/event_based_actor.hpp`, lines 18-22
```
old: #include <hpactor/actor/circuit_breaker.hpp>
     #include <hpactor/actor/drain_config.hpp>
     #include <hpactor/actor/failure_rate_tracker.hpp>
     #include <hpactor/actor/lifecycle_actor.hpp>
     #include <hpactor/actor/quarantine_policy.hpp>
new: #include <hpactor/actor/lifecycle/circuit_breaker.hpp>
     #include <hpactor/actor/lifecycle/drain_config.hpp>
     #include <hpactor/actor/lifecycle/failure_rate_tracker.hpp>
     #include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
     #include <hpactor/actor/lifecycle/quarantine_policy.hpp>
```

- [ ] **Step 2: Fix coroutine includes in `event_based_actor.hpp`** (conditional block)

File: `include/hpactor/actor/event_based_actor.hpp`, lines 39-41
```
old: #    include <hpactor/sched/actor_coroutine.hpp>
     #    include <hpactor/sched/coroutine_awaiters.hpp>
     #    include <hpactor/sched/coroutine_task.hpp>
new: #    include <hpactor/coroutine/actor_coroutine.hpp>
     #    include <hpactor/coroutine/coroutine_awaiters.hpp>
     #    include <hpactor/coroutine/coroutine_task.hpp>
```

- [ ] **Step 3: Fix includes in `actor_route.hpp`**

File: `include/hpactor/actor/actor_route.hpp`, lines 17-18
```
old: #include <hpactor/actor/lifecycle_state.hpp>
     #include <hpactor/actor/passivation_config.hpp>
new: #include <hpactor/actor/lifecycle/lifecycle_state.hpp>
     #include <hpactor/actor/lifecycle/passivation_config.hpp>
```

---

## Task 5: Update include paths in other `include/hpactor/` subsystems

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp:21-24` — drain_config, lifecycle_actor, passivation_manager, shutdown_phase
- Modify: `include/hpactor/config/topology_model.hpp:17` — quarantine_policy
- Modify: `include/hpactor/supervision/supervision.hpp:18` — quarantine_policy

- [ ] **Step 1: Fix actor_system.hpp includes**

File: `include/hpactor/core/actor_system.hpp`, lines 21-24
```
old: #include <hpactor/actor/drain_config.hpp>
     #include <hpactor/actor/lifecycle_actor.hpp>
     #include <hpactor/actor/passivation_manager.hpp>
     #include <hpactor/actor/shutdown_phase.hpp>
new: #include <hpactor/actor/lifecycle/drain_config.hpp>
     #include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
     #include <hpactor/actor/lifecycle/passivation_manager.hpp>
     #include <hpactor/actor/lifecycle/shutdown_phase.hpp>
```

- [ ] **Step 2: Fix topology_model.hpp include**

File: `include/hpactor/config/topology_model.hpp`, line 17
```
old: #include <hpactor/actor/quarantine_policy.hpp>
new: #include <hpactor/actor/lifecycle/quarantine_policy.hpp>
```

- [ ] **Step 3: Fix supervision.hpp include**

File: `include/hpactor/supervision/supervision.hpp`, line 18
```
old: #include <hpactor/actor/quarantine_policy.hpp>
new: #include <hpactor/actor/lifecycle/quarantine_policy.hpp>
```

---

## Task 6: Update include paths in `src/` implementation files

**Files:**
- Modify: `src/actor/actor_system.cpp:16,21-24` — backpressure, local_delivery, passivation, shutdown
- Modify: `src/actor/abstract_actor.cpp:16` — lifecycle_actor
- Modify: `src/actor/event_based_actor.cpp:17-18` — lifecycle_actor, quarantine_reason
- Modify: `src/actor/actor_route.cpp:17` — lifecycle_actor
- Modify: `src/actor/passivation_manager.cpp:17,19,21` — durable_actor, lifecycle_actor, passivation_manager (self)
- Modify: `src/actor/lifecycle_actor.cpp:15` — lifecycle_actor (self, now at new location)
- Modify: `src/actor/quarantine_reason.cpp:15` — quarantine_reason (self, now at new location)
- Modify: `src/actor/shutdown_coordinator.cpp:15` — shutdown_coordinator (self, now at new location)
- Modify: `src/actor/backpressure_coordinator.cpp:15` — backpressure_coordinator (self, now at new location)
- Modify: `src/actor/local_delivery_engine.cpp:16` — local_delivery_engine (self, now at new location)
- Modify: `src/actor/memory_pressure_monitor.cpp:15` — memory_pressure_monitor (self, now at new location)
- Modify: `src/sched/actor_execution_engine.cpp:25` — coroutine_task
- Modify: `src/sched/timing_wheel.cpp:15` — timing_wheel (self, now at new location)
- Modify: `src/supervision/supervision.cpp:20` — lifecycle_actor
- Modify: `src/net/endpoint_circuit_breaker.cpp` — check for circuit_breaker include
- Modify: `src/config/parsers/transport_outbound_config_parser.cpp` — check for lifecycle includes

- [ ] **Step 1: Fix includes in `src/actor/actor_system.cpp`**

File: `src/actor/actor_system.cpp`, lines 16, 21-24
```
old: #include <hpactor/actor/backpressure_coordinator.hpp>
     #include <hpactor/actor/local_delivery_engine.hpp>
     #include <hpactor/actor/passivation_config.hpp>
     #include <hpactor/actor/passivation_manager.hpp>
     #include <hpactor/actor/shutdown_coordinator.hpp>
new: #include <hpactor/mailbox/backpressure_coordinator.hpp>
     #include <hpactor/mailbox/local_delivery_engine.hpp>
     #include <hpactor/actor/lifecycle/passivation_config.hpp>
     #include <hpactor/actor/lifecycle/passivation_manager.hpp>
     #include <hpactor/actor/lifecycle/shutdown_coordinator.hpp>
```

- [ ] **Step 2: Fix include in `src/actor/abstract_actor.cpp`**

File: `src/actor/abstract_actor.cpp`, line 16
```
old: #include <hpactor/actor/lifecycle_actor.hpp>
new: #include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
```

- [ ] **Step 3: Fix includes in `src/actor/event_based_actor.cpp`**

File: `src/actor/event_based_actor.cpp`, lines 17-18
```
old: #include <hpactor/actor/lifecycle_actor.hpp>
     #include <hpactor/actor/quarantine_reason.hpp>
new: #include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
     #include <hpactor/actor/lifecycle/quarantine_reason.hpp>
```

- [ ] **Step 4: Fix include in `src/actor/actor_route.cpp`**

File: `src/actor/actor_route.cpp`, line 17
```
old: #include <hpactor/actor/lifecycle_actor.hpp>
new: #include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
```

- [ ] **Step 5: Fix includes in `src/actor/passivation_manager.cpp`** (will be moved, but fix self-include first)

File: `src/actor/passivation_manager.cpp`, lines 17, 19, 21
```
old: #include <hpactor/actor/durable_actor.hpp>
     #include <hpactor/actor/lifecycle_actor.hpp>
     #include <hpactor/actor/passivation_manager.hpp>
new: #include <hpactor/actor/durable/durable_actor.hpp>
     #include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
     #include <hpactor/actor/lifecycle/passivation_manager.hpp>
```

- [ ] **Step 6: Fix self-includes in moved source files**

After `git mv`, each moved source file includes its own header with the old path. Fix them all:

```bash
# Fix self-includes in moved source files
sed -i '' 's|#include <hpactor/actor/lifecycle_actor.hpp>|#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>|' src/actor/lifecycle/lifecycle_actor.cpp
sed -i '' 's|#include <hpactor/actor/quarantine_reason.hpp>|#include <hpactor/actor/lifecycle/quarantine_reason.hpp>|' src/actor/lifecycle/quarantine_reason.cpp
sed -i '' 's|#include <hpactor/actor/shutdown_coordinator.hpp>|#include <hpactor/actor/lifecycle/shutdown_coordinator.hpp>|' src/actor/lifecycle/shutdown_coordinator.cpp
sed -i '' 's|#include <hpactor/actor/backpressure_coordinator.hpp>|#include <hpactor/mailbox/backpressure_coordinator.hpp>|' src/mailbox/backpressure_coordinator.cpp
sed -i '' 's|#include <hpactor/actor/local_delivery_engine.hpp>|#include <hpactor/mailbox/local_delivery_engine.hpp>|' src/mailbox/local_delivery_engine.cpp
sed -i '' 's|#include <hpactor/actor/memory_pressure_monitor.hpp>|#include <hpactor/mailbox/memory_pressure_monitor.hpp>|' src/mailbox/memory_pressure_monitor.cpp
sed -i '' 's|#include <hpactor/sched/timing_wheel.hpp>|#include <hpactor/timer/timing_wheel.hpp>|' src/timer/timing_wheel.cpp
```

- [ ] **Step 7: Fix include in `src/sched/actor_execution_engine.cpp`**

File: `src/sched/actor_execution_engine.cpp`, line 25
```
old: #    include <hpactor/sched/coroutine_task.hpp>
new: #    include <hpactor/coroutine/coroutine_task.hpp>
```

- [ ] **Step 8: Fix include in `src/supervision/supervision.cpp`**

File: `src/supervision/supervision.cpp`, line 20
```
old: #include <hpactor/actor/lifecycle_actor.hpp>
new: #include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
```

- [ ] **Step 9: Fix include in `src/net/endpoint_circuit_breaker.cpp`** (if present)

Check file and fix any `circuit_breaker.hpp` include:
```bash
grep -n "circuit_breaker.hpp" src/net/endpoint_circuit_breaker.cpp
```
If found:
```
old: #include <hpactor/actor/circuit_breaker.hpp>
new: #include <hpactor/actor/lifecycle/circuit_breaker.hpp>
```

- [ ] **Step 10: Fix include in `src/config/parsers/transport_outbound_config_parser.cpp`** (if present)

Check file and fix any lifecycle/passivation includes:
```bash
grep -n "circuit_breaker\|lifecycle\|passivation\|quarantine\|drain" src/config/parsers/transport_outbound_config_parser.cpp
```

---

## Task 7: Update include paths in test files

**Files (unit tests):**
- Modify: `tests/unit/actor/test_failure_rate_tracker.cpp:16`
- Modify: `tests/unit/actor/test_circuit_breaker_tracker.cpp:16`
- Modify: `tests/unit/actor/test_circuit_breaker_result.cpp:15,17-20`
- Modify: `tests/unit/actor/test_actor_route.cpp:17`
- Modify: `tests/unit/actor/test_lifecycle_state.cpp:16-17`
- Modify: `tests/unit/actor/test_passivation_config.cpp:16`
- Modify: `tests/unit/actor/test_passivation_lifecycle.cpp:16`
- Modify: `tests/unit/actor/test_quarantine_reason.cpp:16`
- Modify: `tests/unit/core/test_circuit_breaker_delivery.cpp:15,17`
- Modify: `tests/unit/supervision/test_supervision_quarantine.cpp:15`
- Modify: `tests/unit/sched/test_mailbox_awaiter.cpp:20-21`
- Modify: `tests/unit/sched/test_coroutine_frame_pool.cpp:27`
- Modify: `tests/unit/sched/test_coroutine_task.cpp:18`

**Files (integration tests):**
- Modify: `tests/integration/actor/test_quarantine_handler.cpp:15,17-18`
- Modify: `tests/integration/actor/test_drain_policy.cpp:16-17,19`
- Modify: `tests/integration/actor/test_shutdown_coordinator.cpp:16-17,19`
- Modify: `tests/integration/actor/test_drain_timeout.cpp:17`
- Modify: `tests/integration/actor/test_drain_integration.cpp:16-17,19-20`
- Modify: `tests/integration/actor/test_passivation_reactivation.cpp:18,21-23`
- Modify: `tests/integration/actor/test_lifecycle_actor.cpp:17`
- Modify: `tests/integration/actor/test_event_based_actor.cpp:17-19`
- Modify: `tests/integration/actor/test_circuit_breaker_lifecycle.cpp:15,17-19`
- Modify: `tests/integration/actor/test_actor_stop.cpp:17`
- Modify: `tests/integration/actor/test_circuit_breaker_inspect.cpp:15,17-19`
- Modify: `tests/integration/sched/test_coroutine_scheduling.cpp:21`
- Modify: `tests/integration/sched/test_worker_thread.cpp:16`

**Files (system tests):**
- Modify: `tests/system/test_system_supervision_directives.cpp:21-22`

**Files (test support):**
- Modify: `tests/support/system_test_fixture.hpp:21-22`

All edits follow the same 3 patterns:

Pattern A — lifecycle:
```
old: <hpactor/actor/<file>.hpp>
new: <hpactor/actor/lifecycle/<file>.hpp>
```
Applies to: `lifecycle_actor`, `lifecycle_state`, `shutdown_phase`, `shutdown_coordinator`, `passivation_manager`, `passivation_config`, `drain_policy`, `drain_config`, `quarantine_policy`, `quarantine_reason`, `circuit_breaker`, `failure_rate_tracker`

Pattern B — coroutine:
```
old: <hpactor/sched/<file>.hpp>
new: <hpactor/coroutine/<file>.hpp>
```
Applies to: `coroutine_task`, `coroutine_awaiters`, `coroutine_frame_pool`

Pattern C — durable:
```
old: <hpactor/actor/durable_actor.hpp>
new: <hpactor/actor/durable/durable_actor.hpp>
```

- [ ] **Step 1: Bulk-update all test file includes with sed**

```bash
# Pattern A — lifecycle files (actor/<name>.hpp → actor/lifecycle/<name>.hpp)
for file in lifecycle_actor lifecycle_state shutdown_phase shutdown_coordinator \
    passivation_manager passivation_config drain_policy drain_config \
    quarantine_policy quarantine_reason circuit_breaker failure_rate_tracker; do
    find tests/ -name "*.cpp" -o -name "*.hpp" | xargs sed -i '' \
        "s|#include <hpactor/actor/${file}.hpp>|#include <hpactor/actor/lifecycle/${file}.hpp>|g"
done

# Pattern B — coroutine files (sched/<name>.hpp → coroutine/<name>.hpp)
for file in coroutine_task coroutine_awaiters coroutine_frame_pool; do
    find tests/ -name "*.cpp" -o -name "*.hpp" | xargs sed -i '' \
        "s|#include <hpactor/sched/${file}.hpp>|#include <hpactor/coroutine/${file}.hpp>|g"
done

# Pattern C — durable_actor
find tests/ -name "*.cpp" -o -name "*.hpp" | xargs sed -i '' \
    "s|#include <hpactor/actor/durable_actor.hpp>|#include <hpactor/actor/durable/durable_actor.hpp>|g"
```

- [ ] **Step 2: Verify no stale paths remain in tests**

```bash
grep -rn "hpactor/actor/lifecycle_actor\.hpp\|hpactor/actor/lifecycle_state\.hpp\|hpactor/actor/passivation_manager\.hpp\|hpactor/actor/passivation_config\.hpp\|hpactor/actor/drain_policy\.hpp\|hpactor/actor/drain_config\.hpp\|hpactor/actor/quarantine_policy\.hpp\|hpactor/actor/quarantine_reason\.hpp\|hpactor/actor/circuit_breaker\.hpp\|hpactor/actor/failure_rate_tracker\.hpp\|hpactor/actor/shutdown_phase\.hpp\|hpactor/actor/shutdown_coordinator\.hpp" tests/
```
Expected: **zero matches** (all moved to `actor/lifecycle/` paths)

```bash
grep -rn "hpactor/sched/coroutine_task\.hpp\|hpactor/sched/coroutine_awaiters\.hpp\|hpactor/sched/coroutine_frame_pool\.hpp" tests/
```
Expected: **zero matches** (all moved to `coroutine/` paths)

---

## Task 8: Update include paths in examples and apps

**Files:**
- Modify: `examples/01_echo_actor.cpp:46` — `sched/coroutine_awaiters.hpp`
- Modify: `examples/08_coroutine_scheduler_demo.cpp:41` — `sched/coroutine_awaiters.hpp`
- Modify: `apps/order_platform/13_order_platform.cpp:131` — `actor/lifecycle_actor.hpp`

- [ ] **Step 1: Fix example includes**

```bash
sed -i '' 's|<hpactor/sched/coroutine_awaiters.hpp>|<hpactor/coroutine/coroutine_awaiters.hpp>|g' examples/01_echo_actor.cpp examples/08_coroutine_scheduler_demo.cpp
```

- [ ] **Step 2: Fix app include**

```bash
sed -i '' 's|<hpactor/actor/lifecycle_actor.hpp>|<hpactor/actor/lifecycle/lifecycle_actor.hpp>|g' apps/order_platform/13_order_platform.cpp
```

---

## Task 9: Update `src/CMakeLists.txt` with new source file paths

**File:** `src/CMakeLists.txt`

- [ ] **Step 1: Update the source file list**

Replace these lines in the `add_library(hpactor_lib SHARED ...)` block:

```
old (lines 9-11):     actor/local_delivery_engine.cpp
                      actor/backpressure_coordinator.cpp
                      actor/shutdown_coordinator.cpp
new:                  mailbox/local_delivery_engine.cpp
                      mailbox/backpressure_coordinator.cpp
                      actor/lifecycle/shutdown_coordinator.cpp
```

```
old (line 19):        actor/lifecycle_actor.cpp
new:                  actor/lifecycle/lifecycle_actor.cpp
```

```
old (line 24):        actor/memory_pressure_monitor.cpp
new:                  mailbox/memory_pressure_monitor.cpp
```

```
old (line 25):        actor/passivation_manager.cpp
new:                  actor/lifecycle/passivation_manager.cpp
```

```
old (line 58):        actor/quarantine_reason.cpp
new:                  actor/lifecycle/quarantine_reason.cpp
```

```
old (line 91):        sched/timing_wheel.cpp
new:                  timer/timing_wheel.cpp
```

```
old (line 92):        adt/calendar_queue.cpp
new:                  timer/calendar_queue.cpp
```

```
old (line 93):        sched/coroutine_frame_pool.cpp
new:                  coroutine/coroutine_frame_pool.cpp
```

---

## Task 10: Update `CLAUDE.md` with module-boundary enforcement rule

**File:** `CLAUDE.md`

- [ ] **Step 1: Add header placement rules**

Add to the architecture section of CLAUDE.md:

```markdown
### Header Placement Rules

New public headers must be placed according to architecture module boundaries:

| Concern | Directory |
|---------|-----------|
| Timer types (timing wheel, calendar queue) | `include/hpactor/timer/` |
| Coroutine infrastructure (tasks, awaiters, frame pool) | `include/hpactor/coroutine/` |
| Lifecycle, shutdown, passivation, drain, quarantine, circuit breaker | `include/hpactor/actor/lifecycle/` |
| Mailbox admission, backpressure, delivery, pressure monitoring | `include/hpactor/mailbox/` |
| Durable actor state | `include/hpactor/actor/durable/` |
| Scheduler internals (scheduler, workers, queues, EDF, A2WS, dispatch) | `include/hpactor/sched/` |

New source files follow the same directory structure under `src/`.
```

---

## Task 11: Configure and build

- [ ] **Step 1: Clean configure**

```bash
rm -rf build
cmake -S . -B build -GNinja
```
Expected: Configures successfully with no errors.

- [ ] **Step 2: Build**

```bash
ninja -C build
```
Expected: Builds successfully with zero errors.

If build errors occur, fix the specific missing include and re-run `ninja -C build`.

---

## Task 12: Run full test suite

- [ ] **Step 1: Run all tests**

```bash
ctest --output-on-failure --parallel 8
```
Expected: All 1411 tests pass.

- [ ] **Step 2: If any tests fail**, inspect the failure, fix, rebuild, re-run.

---

## Task 13: Final stale-path audit

- [ ] **Step 1: Audit for any remaining old include paths**

```bash
# Timer
grep -rn "sched/timing_wheel\|sched/calendar_queue" include/ src/ tests/ examples/ apps/ --include="*.hpp" --include="*.cpp"
# Expected: zero matches

# Coroutine
grep -rn "sched/coroutine_task\|sched/coroutine_awaiters\|sched/coroutine_frame_pool\|sched/yield_awaiter\|sched/actor_coroutine" include/ src/ tests/ examples/ apps/ --include="*.hpp" --include="*.cpp"
# Expected: zero matches (except self-includes within the moved headers themselves)

# Lifecycle (old flat paths)
grep -rn "actor/lifecycle_actor\.hpp\|actor/lifecycle_state\.hpp\|actor/shutdown_phase\.hpp\|actor/shutdown_coordinator\.hpp\|actor/passivation_manager\.hpp\|actor/passivation_config\.hpp\|actor/drain_policy\.hpp\|actor/drain_config\.hpp\|actor/quarantine_policy\.hpp\|actor/quarantine_reason\.hpp\|actor/circuit_breaker\.hpp\|actor/failure_rate_tracker\.hpp" include/ src/ tests/ examples/ apps/ --include="*.hpp" --include="*.cpp"
# Expected: zero matches (only paths under actor/lifecycle/ should remain)

# Delivery (old actor/ paths)
grep -rn "actor/backpressure_coordinator\.hpp\|actor/local_delivery_engine\.hpp\|actor/memory_pressure_monitor\.hpp" include/ src/ tests/ examples/ apps/ --include="*.hpp" --include="*.cpp"
# Expected: zero matches (only paths under mailbox/ should remain)

# Durable (old actor/ path)
grep -rn "actor/durable_actor\.hpp" include/ src/ tests/ examples/ apps/ --include="*.hpp" --include="*.cpp"
# Expected: zero matches (only paths under actor/durable/ should remain)
```

---

## Task 14: Commit

- [ ] **Step 1: Verify working tree state**

```bash
git status
```

- [ ] **Step 2: Commit all changes**

```bash
git add -A
git commit -m "refactor: reorganize headers into timer/, coroutine/, actor/lifecycle/

Move 22 headers and 10 source files to align code organization with
architecture design doc module boundaries:

- New include/hpactor/timer/: timing_wheel.hpp, calendar_queue.hpp
- New include/hpactor/coroutine/: coroutine_task, awaiters, frame_pool,
  actor_coroutine, yield_awaiter
- New include/hpactor/actor/lifecycle/: lifecycle, shutdown, passivation,
  drain, quarantine, circuit_breaker, failure_rate_tracker
- Delivery files moved to include/hpactor/mailbox/: backpressure_coordinator,
  local_delivery_engine, memory_pressure_monitor
- durable_actor.hpp moved into existing actor/durable/ subdirectory

Atomic migration — no forwarding headers, all include paths updated.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```
