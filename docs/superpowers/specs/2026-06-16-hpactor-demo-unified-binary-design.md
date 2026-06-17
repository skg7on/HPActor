# HPActor Demo — Unified Foreground/Service Binary Design

## 1. Problem

The existing `apps/cli_demo/15_cli_demo.cpp` runs only in foreground mode with
an in-process CLI (`CliActor` on stdin/stdout). PR #290 (issue #284) shipped a
full daemon infrastructure — `ProcessManager`, `CliServerActor`, `CliSession`,
`hpactor-cli`, `WatchdogActor`, `HealthHttpServer`, `SyslogSink`, and a systemd
unit file — but there is no binary that ties the daemon infrastructure together
with the cli_demo actor workload. The systemd unit file references
`ExecStart=/usr/local/bin/hpactor --systemd`, but that binary doesn't exist.

## 2. Goal

Create a new `hpactor_demo` app that:
- Uses the 10 actors from `apps/cli_demo/actors/` with identical functionality
- Runs in **foreground mode** on macOS (and Linux) with both direct stdin CLI
  (`CliActor`) AND socket-based CLI server (`CliServerActor`) for remote attach
  via `hpactor-cli`
- Runs as a **systemd service** on Linux (`--systemd`) with socket-cli-only
  access
- Runs as a **traditional daemon** on Linux (`--daemon`) with
  double-fork/setsid
- Demonstrates the unified ProcessManager architecture from PR #290

## 3. Non-Goals

- Container-native orchestration (Kubernetes, Docker entrypoint)
- Socket activation (`sd_listen_fds`)
- Cross-platform daemon support (macOS launchd)
- Modifying 15_cli_demo.cpp — it remains unchanged as a simpler reference

## 4. File Structure

```
apps/hpactor_demo/
├── CMakeLists.txt                 # Build target: hpactor_demo
├── main.cpp                       # CLI arg parsing, mode dispatch (~80 lines)
├── cli_demo_actor_factory.hpp     # Shared actor factory interface
├── cli_demo_actor_factory.cpp     # Spawn all 10 actors + wiring (~120 lines)
├── foreground_runner.hpp          # Foreground mode runner
├── foreground_runner.cpp          # CliActor + CliServerActor + actors (~60 lines)
├── daemon_runner.hpp              # Daemon/systemd mode runner
└── daemon_runner.cpp              # CliServerActor + Watchdog + Health + actors (~60 lines)
```

Actors are included from `apps/cli_demo/actors/` via CMake include path — no
code duplication.

## 5. Architecture

```
main()
  ├── parse_args(argc, argv)          → ProcessMode, Config, overrides
  ├── build ProcessConfig + Config
  ├── ActorSystem system(config)       ← ProcessManager::init() called here
  │
  ├── IF mode == Foreground:
  │     spawn_cli_demo_actors(system)
  │     ForegroundRunner::run(system)
  │       ├── CliActor (stdin/stdout interactive CLI)
  │       ├── CliServerActor (UDS listener for hpactor-cli)
  │       └── block: while (cli_actor->is_running()) sleep(200ms)
  │       └── system.shutdown()
  │
  └── IF mode == Systemd || Daemon:
        spawn_cli_demo_actors(system)
        DaemonRunner::run(system)
          ├── CliServerActor (UDS + optional TCP listener)
          ├── WatchdogActor (sd_notify WATCHDOG=1, if systemd)
          ├── HealthHttpServer (port 8089, /health/live|ready|startup)
          ├── ProcessManager::notify_ready()
          └── block: ProcessManager::wait_for_signal()
          └── ProcessManager::notify_stopping() → system.shutdown()
```

### 5.1 Foreground Mode

Runs BOTH CLI input paths simultaneously:

```
Terminal 1:  ./hpactor_demo --foreground
             > /actor list           ← direct stdin CLI (CliActor)

Terminal 2:  ./hpactor-cli --socket /tmp/hpactor/hpactor.sock
             > /actor list           ← same actors, same commands
```

- UDS path: `/tmp/hpactor/hpactor.sock` on macOS, configurable
- Both sessions share the same `CommandTree` — identical commands
- Multiple concurrent `hpactor-cli` sessions (up to `max_sessions`)
- Direct stdin EOF or `/quit` triggers graceful shutdown

### 5.2 Daemon/Systemd Mode (Linux only)

- Singles access path: CliServerActor (no terminal, no CliActor)
- CLI access only via `hpactor-cli` connecting to UDS/TCP
- Full systemd lifecycle: READY → STATUS → WATCHDOG → STOPPING
- Health HTTP endpoints for load balancers / orchestration

### 5.3 Actor Factory (shared)

```cpp
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

CliDemoActors spawn_cli_demo_actors(ActorSystem& system);
```

Extracted from 15_cli_demo: spawns all 10 actors with the same configs
(Worker-1 at 100msg/s, Worker-2 at 500msg/s, Worker-3 circuit breaker,
Worker-4 delivery failures, etc.) and wires addresses.

## 6. Configuration

### 6.1 CLI Flags

```
--foreground              Foreground mode (default)
--systemd                 systemd Type=notify mode (Linux only)
--daemon                  Traditional double-fork daemon (Linux only)
--config <path>           TOML config file path
--uds-path <path>         Override UDS listen path
--health-port <port>      Health HTTP port (default: 8089, 0=disabled)
--log-level <level>       Override log level
```

### 6.2 Mode Precedence

1. CLI flag (`--foreground`, `--systemd`, `--daemon`)
2. TOML `[system.process].mode`
3. Default: `foreground`

### 6.3 Config Layering

```
CLI flags (highest) → TOML config → hard-coded defaults (lowest)
```

TOML sections `[system.process]`, `[system.cli]`, `[system.mailbox]`,
`[system.shutdown]`, etc. all work as documented in PR #290.

### 6.4 Hard-coded Defaults

Matches 15_cli_demo: 4 scheduler threads, bounded 256-msg mailboxes with
DeadLetter overflow, 30s drain timeout, pretty CLI formatting, 20-line paging.

### 6.5 macOS Platform Guard

- `--systemd` and `--daemon` on macOS: print error + exit(1)
- ProcessManager on macOS: `ProcessMode::Foreground` only — skips
  daemonization, uses self-pipe signal fallback (no signalfd)
- UDS path defaults: `/tmp/hpactor/hpactor.sock` (macOS),
  `/var/run/hpactor/hpactor.sock` (Linux)

## 7. Build Integration

```cmake
# apps/hpactor_demo/CMakeLists.txt
add_executable(hpactor_demo
    main.cpp
    cli_demo_actor_factory.cpp
    foreground_runner.cpp
    daemon_runner.cpp
)
target_link_libraries(hpactor_demo PRIVATE hpactor_lib)
target_include_directories(hpactor_demo PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/cli_demo    # for actors/
)
```

Added to `apps/CMakeLists.txt`: `add_subdirectory(hpactor_demo)`

## 8. Test Strategy

### 8.1 Unit Tests

| Component | Test File | What's Tested |
|-----------|-----------|---------------|
| Actor factory | `test_cli_demo_actor_factory.cpp` | Spawns all 10 actors, verifies wiring, validates configs |
| Foreground runner | `test_foreground_runner.cpp` | CliActor + CliServerActor creation, dual-listener setup |
| Daemon runner | `test_daemon_runner.cpp` | CliServerActor + Watchdog + Health creation, notify sequence |

### 8.2 Integration Tests

| Scenario | What's Verified |
|----------|-----------------|
| Foreground mode → hpactor-cli connect | End-to-end: spawn, connect via UDS, execute commands |
| Foreground mode → /quit graceful exit | Drain → shutdown sequence |
| hpactor-cli → concurrent sessions | Session isolation, no cross-talk |

### 8.3 Manual Smoke Tests

- macOS: `./hpactor_demo --foreground` + `hpactor-cli` connect
- Linux: `./hpactor_demo --systemd` + `hpactor-cli` connect + health check
- Linux: `./hpactor_demo --daemon` + PID file + `hpactor-cli` + SIGTERM

## 9. Design Decisions

1. **Separate app, not modification of 15_cli_demo.** Keeps the simpler
   reference demo intact. The new app demonstrates the unified architecture
   without complicating the existing demo.
2. **Shared actor factory function.** Extracted from 15_cli_demo into a
   standalone function. Both runners use the identical factory. No code
   duplication.
3. **Foreground mode runs BOTH CliActor and CliServerActor.** Provides the
   best developer experience on macOS: direct terminal access AND remote
   attach capability from the same running process.
4. **Daemon mode has CliServerActor only.** No terminal attached; socket is
   the sole CLI path. Matches production usage.
5. **Platform guard for daemon modes on macOS.** Clear error messages rather
   than silent failure or confusing daemonization on unsupported platforms.
6. **TOML config + CLI flag overlay.** Production uses TOML for reproducible
   configs. CLI flags override for quick dev/demo.

## 10. Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| CliActor + CliServerActor race on shutdown | Both share CliSession; shutdown coordinator drains all system actors last |
| Two CLIs producing output concurrently | CliActor owns stdout; CliServerActor sessions have independent output_fn |
| UDS path permissions on macOS | `/tmp/hpactor/` created with 0700; socket at 0600 |
| signalfd unavailable on macOS | Self-pipe trick fallback already implemented in ProcessManager |
