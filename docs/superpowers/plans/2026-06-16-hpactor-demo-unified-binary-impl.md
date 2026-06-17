# hpactor_demo Unified Binary — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a new `hpactor_demo` app that reuses cli_demo actors, supports `--foreground` (macOS + Linux, dual CLI: stdin + socket), `--systemd`, and `--daemon` (Linux only) modes, integrating with the existing daemon infrastructure from PR #290.

**Architecture:** Shared actor factory (`cli_demo_actor_factory.cpp`) extracted from 15_cli_demo spawns all 10 actors with identical configs. `ForegroundRunner` wraps CliActor + CliServerActor for dual CLI access. `DaemonRunner` wraps CliServerActor + WatchdogActor + HealthHttpServer for headless operation. `main.cpp` dispatches by mode.

**Tech Stack:** C++20, hpactor_lib, Google Test. No new dependencies.

---

## File Structure Map

```
NEW FILES (create):
  apps/hpactor_demo/
    CMakeLists.txt                 — build target, links hpactor_lib
    main.cpp                       — CLI arg parsing, mode dispatch
    cli_demo_actor_factory.hpp     — shared actor factory interface
    cli_demo_actor_factory.cpp     — spawn all 10 actors + wiring
    foreground_runner.hpp          — ForegroundRunner interface
    foreground_runner.cpp          — CliActor + CliServerActor + event loop
    daemon_runner.hpp              — DaemonRunner interface
    daemon_runner.cpp              — CliServerActor + Watchdog + Health

MODIFIED FILES:
  apps/CMakeLists.txt              — add_subdirectory(hpactor_demo)

TEST FILES (create):
  tests/unit/apps/CMakeLists.txt   — unit test build for hpactor_demo
  tests/unit/apps/test_cli_demo_actor_factory.cpp
  tests/integration/apps/CMakeLists.txt
  tests/integration/apps/test_hpactor_demo_foreground.cpp
```

---

### Task 1: CMake Build Infrastructure

**Files:**
- Create: `apps/hpactor_demo/CMakeLists.txt`
- Modify: `apps/CMakeLists.txt`

- [ ] **Step 1: Create `apps/hpactor_demo/CMakeLists.txt`**

```cmake
add_executable(hpactor_demo
    main.cpp
    cli_demo_actor_factory.cpp
    foreground_runner.cpp
    daemon_runner.cpp
)
target_link_libraries(hpactor_demo PRIVATE hpactor_lib)
target_include_directories(hpactor_demo PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/cli_demo
)
```

- [ ] **Step 2: Add subdirectory to `apps/CMakeLists.txt`**

After the `add_subdirectory(bench_perf)` line, add:

```cmake
add_subdirectory(hpactor_demo)
```

- [ ] **Step 3: Verify build configuration**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF -DENABLE_APPS=ON
```

Expected: CMake configures without errors. `hpactor_demo` target is known but fails to link (no .cpp files yet).

- [ ] **Step 4: Commit**

```bash
git add apps/hpactor_demo/CMakeLists.txt apps/CMakeLists.txt
git commit -m "build: add hpactor_demo CMake target

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Actor Factory — Header

**Files:**
- Create: `apps/hpactor_demo/cli_demo_actor_factory.hpp`

- [ ] **Step 1: Write the header**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <memory>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace apps::cli_demo {

// Forward declarations for all actor types
class WorkerActor;
class AggregatorActor;
class HealthCheckActor;
class BroadcastActor;
class ClockActor;
class LogActor;
class SystemMonitorActor;
class DlqDemoActor;
class QueryActor;

/// \brief All spawned cli_demo actors with their shared pointers.
///
/// Owns references to every actor spawned by \c spawn_cli_demo_actors().
/// Callers use this to kick off periodic work and access raw pointers
/// for post-spawn wiring.
struct CliDemoActors {
    std::shared_ptr<WorkerActor> workers[4];
    std::shared_ptr<AggregatorActor> aggregator;
    std::shared_ptr<HealthCheckActor> health_check;
    std::shared_ptr<BroadcastActor> broadcast;
    std::shared_ptr<ClockActor> clock;
    std::shared_ptr<LogActor> log;
    std::shared_ptr<SystemMonitorActor> monitor;
    std::shared_ptr<DlqDemoActor> dlq_demo;
    std::shared_ptr<QueryActor> query;
};

/// \brief Spawn all 10 cli_demo actors and wire them up.
///
/// Creates the same actor topology as \c 15_cli_demo.cpp:
/// - 4 x WorkerActor (Worker-1 at 100msg/s, Worker-2 at 500msg/s,
///   Worker-3 circuit breaker, Worker-4 delivery failures)
/// - 1 x AggregatorActor, HealthCheckActor, BroadcastActor, ClockActor,
///   LogActor, SystemMonitorActor, DlqDemoActor, QueryActor
///
/// Workers are wired to Aggregator and Log. HealthCheck + Broadcast are
/// wired to all workers. DlqDemo is wired to all workers. QueryActor is
/// wired to Clock.
///
/// \param[in] system The actor system to spawn into.
/// \return All spawned actors with their shared pointers.
CliDemoActors spawn_cli_demo_actors(ActorSystem& system);

/// \brief Send StartTag and PeriodicTickTag messages to kick off periodic work.
///
/// Must be called after \c spawn_cli_demo_actors() and a short sleep
/// (~100ms) to ensure mailboxes are initialized.
///
/// \param[in] system The actor system for local delivery.
/// \param[in] actors The actors returned by \c spawn_cli_demo_actors().
void kickoff_cli_demo_actors(ActorSystem& system, const CliDemoActors& actors);

} // namespace apps::cli_demo
} // namespace hpactor
```

- [ ] **Step 2: Commit**

```bash
git add apps/hpactor_demo/cli_demo_actor_factory.hpp
git commit -m "feat: add cli_demo_actor_factory header

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Actor Factory — Implementation

**Files:**
- Create: `apps/hpactor_demo/cli_demo_actor_factory.cpp`

- [ ] **Step 1: Write unit test for actor factory (RED)**

Create: `tests/unit/apps/test_cli_demo_actor_factory.cpp`

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <hpactor/core/actor_system.hpp>

// Include the factory header from the app
#include "apps/hpactor_demo/cli_demo_actor_factory.hpp"
#include "apps/cli_demo/actors/worker_actor.hpp"
#include "apps/cli_demo/actors/aggregator_actor.hpp"
#include "apps/cli_demo/actors/health_check_actor.hpp"
#include "apps/cli_demo/actors/broadcast_actor.hpp"
#include "apps/cli_demo/actors/clock_actor.hpp"
#include "apps/cli_demo/actors/log_actor.hpp"
#include "apps/cli_demo/actors/system_monitor_actor.hpp"
#include "apps/cli_demo/actors/dlq_demo_actor.hpp"
#include "apps/cli_demo/actors/query_actor.hpp"

using namespace hpactor;
using namespace hpactor::apps::cli_demo;

TEST(CliDemoActorFactoryTest, SpawnsAllTenActors) {
    Config config;
    config.scheduler_threads = 1;
    // Disable subsystems that aren't needed for this unit test
    config.cli.enabled = false;
    ActorSystem system(config);

    auto actors = spawn_cli_demo_actors(system);

    // All 10 actors should be non-null
    EXPECT_NE(actors.aggregator, nullptr);
    EXPECT_NE(actors.health_check, nullptr);
    EXPECT_NE(actors.broadcast, nullptr);
    EXPECT_NE(actors.clock, nullptr);
    EXPECT_NE(actors.log, nullptr);
    EXPECT_NE(actors.monitor, nullptr);
    EXPECT_NE(actors.dlq_demo, nullptr);
    EXPECT_NE(actors.query, nullptr);
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(actors.workers[i], nullptr) << "Worker " << (i + 1) << " is null";
    }
}

TEST(CliDemoActorFactoryTest, WorkerConfigsAreCorrect) {
    Config config;
    config.scheduler_threads = 1;
    config.cli.enabled = false;
    ActorSystem system(config);

    auto actors = spawn_cli_demo_actors(system);

    // Worker-1: rate limiter at 100 msg/s
    EXPECT_GT(actors.workers[0]->rate_limit(), 0.0);
    EXPECT_NEAR(actors.workers[0]->rate_limit(), 100.0, 1.0);
    EXPECT_EQ(actors.workers[0]->rate_burst(), 10u);

    // Worker-2: rate limiter at 500 msg/s
    EXPECT_NEAR(actors.workers[1]->rate_limit(), 500.0, 1.0);
    EXPECT_EQ(actors.workers[1]->rate_burst(), 50u);

    // Worker-3: no rate limit (circuit breaker mode)
    EXPECT_DOUBLE_EQ(actors.workers[2]->rate_limit(), 0.0);

    // Worker-4: no rate limit (delivery failure mode)
    EXPECT_DOUBLE_EQ(actors.workers[3]->rate_limit(), 0.0);
}

TEST(CliDemoActorFactoryTest, KickoffDoesNotCrash) {
    Config config;
    config.scheduler_threads = 1;
    config.cli.enabled = false;
    ActorSystem system(config);

    auto actors = spawn_cli_demo_actors(system);

    // Kickoff should not throw or crash
    kickoff_cli_demo_actors(system, actors);

    // Let a few ticks process
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    SUCCEED();
}
```

Create: `tests/unit/apps/CMakeLists.txt`

```cmake
add_executable(test_unit_apps
    test_cli_demo_actor_factory.cpp
)
target_link_libraries(test_unit_apps hpactor hpactor_test_support GTest::gtest_main)
target_include_directories(test_unit_apps PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/cli_demo
)
gtest_discover_tests(test_unit_apps)
```

- [ ] **Step 2: Add test subdirectory to unit CMakeLists**

Edit `tests/unit/CMakeLists.txt` — add after the last `add_subdirectory`:

```cmake
add_subdirectory(apps)
```

- [ ] **Step 3: Verify test fails (RED)**

```bash
cmake -S . -B build -GNinja -DENABLE_EXAMPLES=OFF
ninja -C build test_unit_apps
```

Expected: Linker error — `spawn_cli_demo_actors` and `kickoff_cli_demo_actors` are undefined.

- [ ] **Step 4: Write the implementation (GREEN)**

Create: `apps/hpactor_demo/cli_demo_actor_factory.cpp`

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cli_demo_actor_factory.hpp"

#include "apps/cli_demo/actors/aggregator_actor.hpp"
#include "apps/cli_demo/actors/broadcast_actor.hpp"
#include "apps/cli_demo/actors/clock_actor.hpp"
#include "apps/cli_demo/actors/dlq_demo_actor.hpp"
#include "apps/cli_demo/actors/health_check_actor.hpp"
#include "apps/cli_demo/actors/log_actor.hpp"
#include "apps/cli_demo/actors/query_actor.hpp"
#include "apps/cli_demo/actors/system_monitor_actor.hpp"
#include "apps/cli_demo/actors/worker_actor.hpp"
#include "apps/cli_demo/messages.hpp"

#include <hpactor/core/actor_system.hpp>

#include <thread>
#include <vector>

namespace hpactor::apps::cli_demo {

namespace {

void deliver_local(ActorSystem& system, ActorId target, TypeTag tag,
                   StreamBuffer payload = {}) {
    system.deliver_local(target, TypedMessage(tag, std::move(payload)));
}

} // namespace

CliDemoActors spawn_cli_demo_actors(ActorSystem& system) {
    CliDemoActors a;

    // System actors
    a.log = std::static_pointer_cast<LogActor>(
        system.get_actor(system.spawn<LogActor>().id()));
    a.clock = std::static_pointer_cast<ClockActor>(
        system.get_actor(system.spawn<ClockActor>().id()));
    a.aggregator = std::static_pointer_cast<AggregatorActor>(
        system.get_actor(system.spawn<AggregatorActor>().id()));
    a.monitor = std::static_pointer_cast<SystemMonitorActor>(
        system.get_actor(system.spawn<SystemMonitorActor>().id()));
    a.health_check = std::static_pointer_cast<HealthCheckActor>(
        system.get_actor(system.spawn<HealthCheckActor>().id()));
    a.broadcast = std::static_pointer_cast<BroadcastActor>(
        system.get_actor(system.spawn<BroadcastActor>().id()));
    a.dlq_demo = std::static_pointer_cast<DlqDemoActor>(
        system.get_actor(system.spawn<DlqDemoActor>().id()));
    a.query = std::static_pointer_cast<QueryActor>(
        system.get_actor(system.spawn<QueryActor>().id()));

    // Worker-1: Rate limiter 100 msg/s
    {
        WorkerConfig cfg;
        cfg.worker_id = 1;
        cfg.aggregator_id = a.aggregator->id();
        cfg.log_id = a.log->id();
        cfg.rate_limit = 100.0;
        cfg.rate_burst = 10;
        a.workers[0] = std::static_pointer_cast<WorkerActor>(
            system.get_actor(system.spawn<WorkerActor>(cfg).id()));
    }

    // Worker-2: Rate limiter 500 msg/s
    {
        WorkerConfig cfg;
        cfg.worker_id = 2;
        cfg.aggregator_id = a.aggregator->id();
        cfg.log_id = a.log->id();
        cfg.rate_limit = 500.0;
        cfg.rate_burst = 50;
        a.workers[1] = std::static_pointer_cast<WorkerActor>(
            system.get_actor(system.spawn<WorkerActor>(cfg).id()));
    }

    // Worker-3: Circuit breaker + quarantine enabled
    {
        WorkerConfig cfg;
        cfg.worker_id = 3;
        cfg.aggregator_id = a.aggregator->id();
        cfg.log_id = a.log->id();
        cfg.rate_limit = 0.0;
        cfg.quarantine_enabled = true;
        a.workers[2] = std::static_pointer_cast<WorkerActor>(
            system.get_actor(system.spawn<WorkerActor>(cfg).id()));
    }

    // Worker-4: Delivery failure generation + quarantine enabled
    {
        WorkerConfig cfg;
        cfg.worker_id = 4;
        cfg.aggregator_id = a.aggregator->id();
        cfg.log_id = a.log->id();
        cfg.rate_limit = 0.0;
        cfg.quarantine_enabled = true;
        cfg.generate_delivery_failures = true;
        a.workers[3] = std::static_pointer_cast<WorkerActor>(
            system.get_actor(system.spawn<WorkerActor>(cfg).id()));
    }

    // Wire addresses
    for (auto& w : a.workers) {
        w->set_aggregator_addr(a.aggregator->address());
        w->set_log_addr(a.log->address());
    }

    auto* health_raw = a.health_check.get();
    auto* broadcast_raw = a.broadcast.get();

    std::vector<ActorAddress> worker_addrs;
    for (auto& w : a.workers) {
        health_raw->add_worker(w->address());
        broadcast_raw->add_worker(w->address());
        worker_addrs.push_back(w->address());
    }

    auto* dlq_raw = a.dlq_demo.get();
    dlq_raw->set_target_actors(worker_addrs);

    auto* query_raw = a.query.get();
    query_raw->set_clock_addr(a.clock->address());

    return a;
}

void kickoff_cli_demo_actors(ActorSystem& system, const CliDemoActors& actors) {
    // Rate limiters configured automatically in WorkerActor::set_mailbox()
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (auto& w : actors.workers) {
        deliver_local(system, w->id(), StartTag);
    }
    deliver_local(system, actors.health_check->id(), StartTag);
    deliver_local(system, actors.broadcast->id(), StartTag);
    deliver_local(system, actors.monitor->id(), StartTag);
    deliver_local(system, actors.clock->id(), PeriodicTickTag);
    deliver_local(system, actors.dlq_demo->id(), StartTag);
    deliver_local(system, actors.query->id(), StartTag);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

} // namespace hpactor::apps::cli_demo
```

- [ ] **Step 5: Build and run test (GREEN)**

```bash
ninja -C build test_unit_apps
./build/tests/unit/apps/test_unit_apps
```

Expected: All 3 tests pass.

- [ ] **Step 6: Commit**

```bash
git add apps/hpactor_demo/cli_demo_actor_factory.cpp \
        tests/unit/apps/CMakeLists.txt \
        tests/unit/apps/test_cli_demo_actor_factory.cpp \
        tests/unit/CMakeLists.txt
git commit -m "feat: add cli_demo_actor_factory with unit tests

Extracts actor spawning and wiring from 15_cli_demo into a reusable
factory function. Three tests validate spawn count, worker configs,
and kickoff safety.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Foreground Runner — Header + Stub

**Files:**
- Create: `apps/hpactor_demo/foreground_runner.hpp`
- Create: `apps/hpactor_demo/foreground_runner.cpp` (stub)

- [ ] **Step 1: Write the header**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <string>

namespace hpactor {

class ActorSystem;

namespace apps::hpactor_demo {

/// \brief Configuration overrides for foreground mode.
struct ForegroundConfig {
    /// UDS listen path for CliServerActor (empty = use default).
    std::string uds_path;
};

/// \brief Run the actor system in foreground mode.
///
/// Spawns CliActor (stdin/stdout interactive CLI) and CliServerActor
/// (UDS listener for hpactor-cli), then blocks until the CliActor
/// exits or a signal is received.
///
/// \param[in] system The actor system (already constructed).
/// \param[in] cfg Foreground mode configuration overrides.
/// \return 0 on success, non-zero on error.
int run_foreground(ActorSystem& system, const ForegroundConfig& cfg);

} // namespace apps::hpactor_demo
} // namespace hpactor
```

- [ ] **Step 2: Write the stub implementation**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "foreground_runner.hpp"

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/cli_server_actor.hpp>
#include <hpactor/cli/cli_server_config.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/process/process_manager.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace hpactor::apps::hpactor_demo {

int run_foreground(ActorSystem& system, const ForegroundConfig& cfg) {
    // Spawn CliServerActor for remote hpactor-cli access
    cli::CliServerConfig server_cfg;
    server_cfg.uds_listen_path =
        cfg.uds_path.empty() ? "/tmp/hpactor/hpactor.sock" : cfg.uds_path;
    server_cfg.max_sessions = 16;
    server_cfg.default_format = "pretty";
    server_cfg.page_size = 20;

    auto cli_server = system.spawn<cli::CliServerActor>(server_cfg);

    // ProcessManager: notify ready
    process::ProcessManager::notify_ready();

    // Block until CliActor exits (/quit or EOF)
    std::cout << "\n[hpactor_demo foreground mode — type /help for commands, "
                 "/quit to exit]\n"
              << "[CliServerActor listening on " << server_cfg.uds_listen_path
              << "]\n"
              << std::endl;

    while (system.cli_actor() && system.cli_actor()->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Shutdown the CLI server first
    auto* server_raw = std::static_pointer_cast<cli::CliServerActor>(
                           system.get_actor(cli_server.id()))
                           .get();
    if (server_raw) {
        server_raw->request_shutdown();
    }

    std::cout << "\nInitiating graceful shutdown..." << std::endl;
    auto shutdown_result = system.shutdown();
    if (shutdown_result.has_value()) {
        std::cout << "Shutdown complete — all actors drained." << std::endl;
    } else {
        std::cout << "Shutdown timed out — forcing exit." << std::endl;
    }

    std::cout << "=== Demo Complete ===" << std::endl;
    return 0;
}

} // namespace hpactor::apps::hpactor_demo
```

- [ ] **Step 3: Commit**

```bash
git add apps/hpactor_demo/foreground_runner.hpp apps/hpactor_demo/foreground_runner.cpp
git commit -m "feat: add foreground_runner with dual CLI (stdin + socket)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Daemon Runner — Header + Stub

**Files:**
- Create: `apps/hpactor_demo/daemon_runner.hpp`
- Create: `apps/hpactor_demo/daemon_runner.cpp`

- [ ] **Step 1: Write the header**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor {

class ActorSystem;

namespace apps::hpactor_demo {

/// \brief Configuration overrides for daemon/systemd mode.
struct DaemonConfig {
    /// UDS listen path for CliServerActor.
    std::string uds_path;
    /// TCP port (0 = disabled).
    uint16_t tcp_port = 0;
    /// Health HTTP server port (0 = disabled).
    uint16_t health_port = 8089;
    /// Watchdog ping interval (0 = disabled).
    std::chrono::milliseconds watchdog_interval{0};
};

/// \brief Run the actor system in daemon or systemd mode (Linux only).
///
/// Spawns CliServerActor, WatchdogActor (if watchdog_interval > 0),
/// and HealthHttpServer (if health_port > 0). Blocks on signal wait
/// until SIGTERM/SIGINT triggers graceful shutdown.
///
/// On macOS, prints an error and returns 1.
///
/// \param[in] system The actor system (already constructed).
/// \param[in] cfg Daemon mode configuration overrides.
/// \return 0 on success, non-zero on error.
int run_daemon(ActorSystem& system, const DaemonConfig& cfg);

} // namespace apps::hpactor_demo
} // namespace hpactor
```

- [ ] **Step 2: Write the implementation**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "daemon_runner.hpp"

#include <hpactor/cli/cli_server_actor.hpp>
#include <hpactor/cli/cli_server_config.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/process/health_http_server.hpp>
#include <hpactor/process/process_manager.hpp>
#include <hpactor/process/watchdog_actor.hpp>

#include <iostream>

namespace hpactor::apps::hpactor_demo {

int run_daemon(ActorSystem& system, const DaemonConfig& cfg) {
#ifdef __APPLE__
    std::cerr << "Error: daemon/systemd mode is not supported on macOS.\n"
              << "Use --foreground mode instead.\n";
    return 1;
#else
    // Spawn CliServerActor (sole CLI access path)
    cli::CliServerConfig server_cfg;
    server_cfg.uds_listen_path = cfg.uds_path;
    server_cfg.tcp_listen_port = cfg.tcp_port;
    server_cfg.max_sessions = 16;
    server_cfg.default_format = "pretty";
    server_cfg.page_size = 50;
    system.spawn<cli::CliServerActor>(server_cfg);

    // Spawn WatchdogActor if configured
    if (cfg.watchdog_interval.count() > 0) {
        system.spawn<process::WatchdogActor>(cfg.watchdog_interval);
    }

    // Spawn HealthHttpServer if configured
    if (cfg.health_port > 0) {
        process::HealthHttpConfig health_cfg;
        health_cfg.port = cfg.health_port;
        system.spawn<process::HealthHttpServer>(health_cfg);
    }

    // Signal readiness to systemd (no-op in daemon mode without systemd)
    process::ProcessManager::notify_ready();

    // Block until a signal arrives
    process::ProcessManager::wait_for_signal();

    // Begin graceful shutdown
    process::ProcessManager::notify_stopping();

    auto shutdown_result = system.shutdown();
    if (shutdown_result.has_value()) {
        process::ProcessManager::notify_status("Shutdown complete");
    }

    process::ProcessManager::notify_stopped();
    return 0;
#endif
}

} // namespace hpactor::apps::hpactor_demo
```

- [ ] **Step 3: Commit**

```bash
git add apps/hpactor_demo/daemon_runner.hpp apps/hpactor_demo/daemon_runner.cpp
git commit -m "feat: add daemon_runner for systemd/daemon modes (Linux only)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Main — CLI Argument Parsing + Mode Dispatch

**Files:**
- Create: `apps/hpactor_demo/main.cpp`

- [ ] **Step 1: Write main.cpp**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// \file main.cpp
/// \brief hpactor_demo — unified foreground/service binary using cli_demo actors.
///
/// Supports three modes:
///   --foreground  Interactive CLI via stdin + UDS socket (default)
///   --systemd     systemd Type=notify service (Linux only)
///   --daemon      Traditional double-fork daemon (Linux only)

#include "cli_demo_actor_factory.hpp"
#include "daemon_runner.hpp"
#include "foreground_runner.hpp"

#include <hpactor/cli/cli_config.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mailbox_config.hpp>
#include <hpactor/process/process_config.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace hpactor;
using namespace hpactor::apps;

namespace {

struct CliOptions {
    process::ProcessMode mode = process::ProcessMode::Foreground;
    std::string config_path;
    std::string uds_path;
    uint16_t tcp_port = 0;
    uint16_t health_port = 8089;
    std::string log_level;
};

void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  --foreground          Foreground mode with interactive CLI (default)\n"
        << "  --systemd             systemd Type=notify mode (Linux only)\n"
        << "  --daemon              Traditional double-fork daemon (Linux only)\n"
        << "  --config PATH         TOML configuration file path\n"
        << "  --uds-path PATH       UDS listen path for CLI server\n"
        << "                        (default: /tmp/hpactor/hpactor.sock on macOS,\n"
        << "                         /var/run/hpactor/hpactor.sock on Linux)\n"
        << "  --tcp-port PORT       TCP port for CLI server (0 = disabled)\n"
        << "  --health-port PORT    Health HTTP port (default: 8089, 0 = disabled)\n"
        << "  --log-level LEVEL     Override log level\n"
        << "  --help                Show this help message\n";
}

std::string default_uds_path() {
#ifdef __APPLE__
    return "/tmp/hpactor/hpactor.sock";
#else
    return "/var/run/hpactor/hpactor.sock";
#endif
}

CliOptions parse_args(int argc, char* argv[]) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--foreground") {
            opts.mode = process::ProcessMode::Foreground;
        } else if (arg == "--systemd") {
            opts.mode = process::ProcessMode::Systemd;
        } else if (arg == "--daemon") {
            opts.mode = process::ProcessMode::Daemon;
        } else if (arg == "--config") {
            if (i + 1 < argc) opts.config_path = argv[++i];
        } else if (arg == "--uds-path") {
            if (i + 1 < argc) opts.uds_path = argv[++i];
        } else if (arg == "--tcp-port") {
            if (i + 1 < argc) opts.tcp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--health-port") {
            if (i + 1 < argc) opts.health_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--log-level") {
            if (i + 1 < argc) opts.log_level = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    return opts;
}

Config build_config(const CliOptions& opts) {
    Config config;

    // Core settings matching 15_cli_demo
    config.scheduler_threads = 4;
    config.max_queue_depth = 1024;

    // Process mode
    config.process.mode = opts.mode;

    // Mailbox defaults matching 15_cli_demo
    config.mailbox.default_capacity = 256;
    config.mailbox.default_policy = mailbox::OverflowPolicy::DeadLetter;
    config.dead_letters.capacity = 1024;

    // Graceful shutdown matching 15_cli_demo
    config.shutdown_drain =
        DrainConfig{DrainPolicy::Drain, std::chrono::milliseconds{30'000}};

    // CLI configuration
    if (opts.mode == process::ProcessMode::Foreground) {
        config.cli = cli::CliConfig{.enabled = true,
                                     .listen_path = "",
                                     .tcp_port = 0,
                                     .default_format = "pretty",
                                     .page_size = 20,
                                     .history_path = "",
                                     .history_max = 1000};
    }

    // Log level override
    if (!opts.log_level.empty()) {
        config.logging.default_level = opts.log_level;
    }

    return config;
}

} // namespace

int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);

    // Print splash for foreground mode
    if (opts.mode == process::ProcessMode::Foreground) {
        std::cout
            << "\n"
            << "╔══════════════════════════════════════════════════════════════╗\n"
            << "║     HPActor Demo — Unified Foreground/Service Binary         ║\n"
            << "║     CLI Interactive Demo + Daemon Infrastructure             ║\n"
            << "╠══════════════════════════════════════════════════════════════╣\n"
            << "║                                                              ║\n"
            << "║  Architecture:                                               ║\n"
            << "║    10 actors across 8 types                                  ║\n"
            << "║    4 scheduler threads with A2WS work-stealing               ║\n"
            << "║    Dual CLI: stdin (direct) + UDS socket (hpactor-cli)       ║\n"
            << "║                                                              ║\n"
            << "║  Production Features:                                        ║\n"
            << "║    • Rate limiting (Worker-1: 100msg/s, Worker-2: 500msg/s)  ║\n"
            << "║    • Circuit breaker + quarantine (Worker-3, DlqDemoActor)   ║\n"
            << "║    • Delivery failure generation (Worker-4)                  ║\n"
            << "║    • Bounded mailboxes (256 msg, DeadLetter overflow)        ║\n"
            << "║    • Graceful shutdown (30s drain timeout)                   ║\n"
            << "║                                                              ║\n"
            << "║  Connect via hpactor-cli:                                    ║\n"
            << "║    hpactor-cli --socket " << (opts.uds_path.empty() ? default_uds_path() : opts.uds_path) << "\n"
            << "║                                                              ║\n"
            << "╚══════════════════════════════════════════════════════════════╝\n"
            << std::endl;
    }

    // Build config and construct ActorSystem (ProcessManager::init called here)
    auto config = build_config(opts);
    ActorSystem system(config);

    // Spawn all cli_demo actors (shared between modes)
    auto actors = hpactor_demo::spawn_cli_demo_actors(system);
    hpactor_demo::kickoff_cli_demo_actors(system, actors);

    // Dispatch by mode
    if (opts.mode == process::ProcessMode::Foreground) {
        hpactor_demo::ForegroundConfig fg_cfg;
        fg_cfg.uds_path =
            opts.uds_path.empty() ? default_uds_path() : opts.uds_path;
        return hpactor_demo::run_foreground(system, fg_cfg);
    }

    // Daemon or systemd mode
    hpactor_demo::DaemonConfig daemon_cfg;
    daemon_cfg.uds_path =
        opts.uds_path.empty() ? default_uds_path() : opts.uds_path;
    daemon_cfg.tcp_port = opts.tcp_port;
    daemon_cfg.health_port = opts.health_port;
    daemon_cfg.watchdog_interval =
        (opts.mode == process::ProcessMode::Systemd)
            ? std::chrono::milliseconds{5000}
            : std::chrono::milliseconds{0};
    return hpactor_demo::run_daemon(system, daemon_cfg);
}
```

- [ ] **Step 2: Build and verify**

```bash
ninja -C build hpactor_demo
```

Expected: Build succeeds. Binary links.

- [ ] **Step 3: Commit**

```bash
git add apps/hpactor_demo/main.cpp
git commit -m "feat: add hpactor_demo main with mode dispatch

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Integration Test — Foreground Mode Smoke Test

**Files:**
- Create: `tests/integration/apps/CMakeLists.txt`
- Create: `tests/integration/apps/test_hpactor_demo_foreground.cpp`
- Modify: `tests/integration/CMakeLists.txt`

- [ ] **Step 1: Write the integration test (RED)**

Create: `tests/integration/apps/test_hpactor_demo_foreground.cpp`

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/cli_server_actor.hpp>
#include <hpactor/cli/cli_server_config.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/process/process_manager.hpp>

#include "apps/hpactor_demo/cli_demo_actor_factory.hpp"
#include "apps/hpactor_demo/foreground_runner.hpp"

#include <chrono>
#include <thread>

using namespace hpactor;
using namespace hpactor::apps;

TEST(HpactorDemoForegroundTest, ActorFactorySpawnsAllActors) {
    Config config;
    config.scheduler_threads = 1;
    config.cli.enabled = false;
    config.mailbox.default_capacity = 256;
    config.mailbox.default_policy = mailbox::OverflowPolicy::DeadLetter;
    ActorSystem system(config);

    auto actors = hpactor_demo::spawn_cli_demo_actors(system);

    // Verify all 10 actors are non-null
    EXPECT_NE(actors.aggregator, nullptr);
    EXPECT_NE(actors.health_check, nullptr);
    EXPECT_NE(actors.broadcast, nullptr);
    EXPECT_NE(actors.clock, nullptr);
    EXPECT_NE(actors.log, nullptr);
    EXPECT_NE(actors.monitor, nullptr);
    EXPECT_NE(actors.dlq_demo, nullptr);
    EXPECT_NE(actors.query, nullptr);
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(actors.workers[i], nullptr) << "Worker " << (i + 1) << " is null";
    }
}

TEST(HpactorDemoForegroundTest, ConfigIsForegroundByDefault) {
    Config config;
    EXPECT_EQ(config.process.mode, process::ProcessMode::Foreground);
}

TEST(HpactorDemoForegroundTest, DualCliSpawning) {
    Config config;
    config.scheduler_threads = 1;
    config.cli.enabled = true;
    config.cli.default_format = "pretty";
    config.cli.page_size = 20;
    config.mailbox.default_capacity = 256;
    config.mailbox.default_policy = mailbox::OverflowPolicy::DeadLetter;
    ActorSystem system(config);

    // Spawn actors first
    auto actors = hpactor_demo::spawn_cli_demo_actors(system);

    // Verify CliActor was created by ActorSystem (enabled=true)
    ASSERT_NE(system.cli_actor(), nullptr);
    EXPECT_TRUE(system.cli_actor()->is_running());

    // Spawn CliServerActor alongside
    cli::CliServerConfig server_cfg;
    server_cfg.uds_listen_path = "/tmp/hpactor_test.sock";
    server_cfg.max_sessions = 4;
    auto cli_server = system.spawn<cli::CliServerActor>(server_cfg);
    EXPECT_NE(cli_server.id().value(), 0u);

    // Brief spin to let daemon threads start
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Shutdown CliActor
    system.cli_actor()->request_shutdown();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Clean shutdown
    system.shutdown();
    SUCCEED();
}
```

Create: `tests/integration/apps/CMakeLists.txt`

```cmake
add_executable(test_integration_apps
    test_hpactor_demo_foreground.cpp
)
target_link_libraries(test_integration_apps hpactor hpactor_test_support GTest::gtest_main)
target_include_directories(test_integration_apps PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/cli_demo
)
gtest_discover_tests(test_integration_apps)
```

- [ ] **Step 2: Add subdirectory to integration CMakeLists**

Edit `tests/integration/CMakeLists.txt` — add after the last `add_subdirectory`:

```cmake
add_subdirectory(apps)
```

- [ ] **Step 3: Build and run integration tests (GREEN)**

```bash
ninja -C build test_integration_apps
./build/tests/integration/apps/test_integration_apps
```

Expected: All 3 tests pass.

- [ ] **Step 4: Commit**

```bash
git add tests/integration/apps/CMakeLists.txt \
        tests/integration/apps/test_hpactor_demo_foreground.cpp \
        tests/integration/CMakeLists.txt
git commit -m "test: add hpactor_demo integration tests

Validates actor factory, foreground config defaults, and dual CLI
(CliActor + CliServerActor) spawning in the same system.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: REFACTOR — Cleanup and Polish

**Files:**
- Modify: `apps/hpactor_demo/main.cpp`
- Modify: `apps/hpactor_demo/foreground_runner.cpp`

- [ ] **Step 1: Review for consistency with codebase conventions**

- Ensure all new files have Apache 2.0 license headers ✓ (done in each task)
- No RTTI, no exceptions ✓
- Match LLVM naming conventions ✓
- All `#include` paths use project-relative paths ✓

- [ ] **Step 2: Verify all tests still pass**

```bash
ninja -C build
ctest --output-on-failure -R "test_unit_apps|test_integration_apps" --parallel 4
```

Expected: All tests pass.

- [ ] **Step 3: Verify 15_cli_demo still builds and passes**

```bash
ninja -C build 15_cli_demo
```

Expected: Builds successfully, unchanged.

- [ ] **Step 4: Full test suite regression check**

```bash
ctest --output-on-failure --parallel 8
```

Expected: All existing tests pass (no regressions).

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor: final polish for hpactor_demo unified binary

Verified no regressions in existing test suite. Confirmed 15_cli_demo
builds unchanged.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Verification Summary

After all tasks complete:

| Check | Command | Expected |
|-------|---------|----------|
| hpactor_demo builds | `ninja -C build hpactor_demo` | Success |
| 15_cli_demo still builds | `ninja -C build 15_cli_demo` | Success |
| Unit tests pass | `ctest -R test_unit_apps` | All pass |
| Integration tests pass | `ctest -R test_integration_apps` | All pass |
| Full regression | `ctest --output-on-failure --parallel 8` | 0 failures |
| `--help` output | `./build/apps/hpactor_demo/hpactor_demo --help` | Usage printed |
| `--foreground` smoke | `timeout 3 ./build/apps/hpactor_demo/hpactor_demo --foreground` | Splash, starts, exits cleanly |
