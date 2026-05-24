# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Session Warmup

At the start of a substantive task, read these files first:

1. `AGENTS.md` — Codex-facing working instructions for this repo.
2. `CLAUDE.md` — Claude-facing instructions; keep shared build and architecture guidance in sync when it changes.
3. `CLAUDE_MEMORY.md` — project memory summary with current feature status, implementation history, docs, and recent test counts.

Treat `CLAUDE_MEMORY.md` as the high-level project memory source in this checkout. If persistent memory directories are introduced later, add their exact path here instead of relying on wildcard paths.

## Required Worktree Workflow

Every design or implementation job must happen in an isolated git worktree under
the repository-local `.worktrees/` directory.

- Before writing a design/spec/plan or changing source, docs, config, tests, or
  build files, detect whether the current checkout is already a linked worktree.
- If not already in a linked worktree, create one at `.worktrees/<short-task-name>`
  on a task-specific branch, then do all edits there.
- Do not create new worktrees under `.worktree/`; that legacy directory may
  exist locally, but `.worktrees/` is the project convention.
- Keep `.worktrees/` ignored. If the ignore rule is missing, add it before
  creating a project-local worktree.
- Use the worktree's own `build/` directory for configure/build/test output.
- Pure read-only inspection may happen from the main checkout, but any design or
  implementation write must move into `.worktrees/` first.

### CRITICAL: Never Leak Files to the Main Checkout

The main checkout at the repository root (e.g., `/Users/<user>/Workspace/.../HPActor/`)
and each worktree (e.g., `.worktrees/<name>/`) are **separate working directories**
that share the same `.git` repository. Writing to the wrong one leaks changes onto
the wrong branch.

**Hard rules:**

- **Never use the repository root path as a file target.** The repo root path
  (`/Users/skg7on/Workspace/Projects/HPActor/`) is the **main checkout** — files
  written there land on `main`, not your worktree branch.
- **Always use the worktree path for writes.** The worktree lives at
  `/Users/skg7on/Workspace/Projects/HPActor/.worktrees/<name>/`. Use this absolute
  path, or use paths relative to the current working directory (which the harness
  sets to the worktree root).
- **Verify before writing.** Before creating or modifying any file, confirm the
  session working directory is the worktree: `pwd` should print
  `.../HPActor/.worktrees/<name>`, NOT `.../HPActor` (the main checkout).
- **Prefer relative paths** (e.g., `tests/unit/core/test_smoke.cpp`) — they resolve
  against the worktree root automatically.
- **When subagents or scripts run commands**, they inherit the session's CWD. If a
  subagent uses an absolute path, it must derive it from `pwd` at runtime, never
  from a hardcoded string.
- **Before committing, verify the branch:** `git branch --show-current` must show
  the worktree branch, not `main`.
- **After any task that writes files**, run `git status` to confirm all changes
  appear in the worktree and no untracked files appear in the main checkout.

**Example — correct:**
```bash
# Write to worktree (CWD is already the worktree root)
Write file_path="tests/unit/core/test_smoke.cpp" ...
# Or with absolute worktree path
Write file_path="/Users/skg7on/Workspace/Projects/HPActor/.worktrees/test-reorg-gtest/tests/unit/core/test_smoke.cpp" ...
```

**Example — WRONG (leaks to main):**
```bash
# NEVER do this — this is the main checkout, not the worktree
Write file_path="/Users/skg7on/Workspace/Projects/HPActor/tests/unit/core/test_smoke.cpp" ...
```

## Build Commands

```bash
# Configure and build
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build

# Run tests
ctest --output-on-failure --parallel 8

# Run a single test
./build/tests/unit/core/test_unit_core

# Run a single GTest suite or test via filter
./build/tests/unit/core/test_unit_core --gtest_list_tests
./build/tests/unit/core/test_unit_core --gtest_filter="*ActorId*"

# Run a specific GTest case through ctest
ctest -R "ActorIdDefaultConstruction" --output-on-failure

# Build with sanitizers
cmake -DENABLE_TSAN=ON ..  # ThreadSanitizer
cmake -DENABLE_ASAN=ON ..  # AddressSanitizer

# Build options
cmake -DENABLE_EXAMPLES=OFF ..    # Disable examples (default ON)
cmake -DENABLE_PROACTOR=ON ..     # Enable proactor backend
cmake -DENABLE_MEMORY_TRACKING=OFF .. # Disable per-actor memory tracking (default ON)
cmake -DENABLE_MEMORY_DEBUG=ON .. # Enable memory poisoning + canary verification
cmake -DENABLE_ACTOR_METRICS=OFF .. # Disable actor-level metrics (default ON)
cmake -DENABLE_ACTOR_LOGGING=OFF .. # Disable structured actor logging (default ON)
cmake -DENABLE_ACTOR_TRACING=OFF .. # Disable distributed tracing (default ON)
cmake -DENABLE_CLI=OFF ..       # Disable interactive CLI subsystem (default ON, runtime opt-in via cli.enabled)
cmake -DENABLE_COVERAGE=ON ..   # Enable gcov/llvm-cov style coverage instrumentation
```

## Architecture

HPActor is a C++20 event-based actor framework inspired by CAF (C++ Actor Framework).

### Production Reliability Direction

The current architecture roadmap is organized around a production reliability
plane for 24x7 distributed actor operation. The primary entry point is
`docs/architecture/production/production-reliability-plane.md`, with a refined
feature-gap backlog in
`docs/architecture/production/feature-gap-refined-requirement-backlog.md`.

When adding production-facing actor-system features, align the design with these
planes:

- **Data plane**: delivery semantics, mailbox admission, DLQ, reliable
  messaging, tracing, actor lifecycle.
- **Control plane**: cluster failure model, node identity, sharding, placement,
  rebalancing, graceful shutdown, rolling upgrades.
- **Operations plane**: health, admin API, security, audit, config reload,
  incident timelines, chaos/soak/fuzz testing.

### Claude Operating Rules

- Treat the production architecture docs as requirements and design backlog
  until code proves otherwise; do not describe backlog items as implemented
  runtime behavior.
- Start design work from the relevant architecture doc in
  `docs/architecture/production/`, then capture runtime contracts, failure
  semantics, observability, and acceptance evidence before implementation.
- Preserve source-compatible defaults for existing actor APIs. Production-grade
  behavior such as delivery results, bounded mailboxes, reliable messaging,
  tracing, security, and durability should be opt-in or safely defaulted.
- Keep actor boundaries explicit: use protobuf `TypedMessage` type tags for
  dynamic messages, typed actor signatures for static contracts, and avoid
  shared mutable state between actors.
- Prefer subsystem-owned extension points over central switches. New TOML
  subsystem config should use self-registering parsers and opaque
  `TomlTableView`, not public `toml++` headers or edits that grow a monolithic
  parser.
- For production-facing changes, include the operations surface in the same
  design: metrics, CLI/admin visibility, health/readiness, audit or trace
  correlation, and runbook impact when applicable.

### Actor Type Hierarchy

```
AbstractActor (interface base)
    └── LocalActor (has ActorContext access)
            ├── EventBasedActor (cooperative, behavior-based, proto handlers)
            │       ├── StatefulActor<T> (explicit state)
            │       ├── ProtoStatefulActor<T> (protobuf-native + explicit state)
            │       ├── SpawnReceiver (system actor, handles remote spawn)
            │       └── DenseComputingActor (dedicated pool dispatch)
            ├── TypedEventBasedActor<Signatures...> (statically typed)
            ├── BlockingActor (thread-based, blocking receive)
            │       └── ScopedActor (for main/non-actor contexts)
            └── DaemonActor (dedicated thread, run_once() loop)
                    ├── PollingActor (CPU affinity, poll budget)
                    ├── ExternalMsgGatewayActor (named route table, message transforms)
                    │       └── net::HTTPGatewayActor (HTTP ingress gateway)
                    └── cli::CliActor (stdin/socket I/O, command tree, InspectState requests)
```

### Key Components

| Component | Location | Purpose |
|-----------|----------|---------|
| `ActorSystem` | `core/actor_system.hpp` | Actor environment, spawn, registry, clock, topology loading |
| `ActorContext` | `actor_context.hpp` | Actor execution context, message sending |
| `Behavior` | `behavior.hpp` | Message handler with `become()` support |
| `TypedBehavior` | `actor/typed_actor.hpp` | Statically typed handlers |
| `Supervision` | `supervision/*.hpp` | Fault-tolerance (OneForOne, AllForOne, self-supervising) |
| `HybridScheduler` | `sched/hybrid_scheduler.hpp` | Work-stealing scheduler with A2WS victim selection |
| `MPSCActorMailbox` | `mailbox/mpsc_actor_mailbox.hpp` | Lock-free MPSC queue with edge-triggered CAS wakeup |
| `SlabCache` / `SegmentProvider` | `mem/*.hpp` | Two-tier slab allocator (thread-local caches + mmap segments) |
| `EventLoop` | `net/event_loop.hpp` | kqueue/epoll edge-triggered event loop |
| `TomlParser` | `config/toml_parser.hpp` | TOML topology parser: coordinator that delegates to self-registered subsystem parsers |
| `TomlParserRegistry` | `config/toml_parser_registry.hpp` | IoC registry for TOML subsystem parsers with static self-registration |
| `TomlTableView` | `config/toml_table_view.hpp` | Opaque TOML table wrapper isolating toml.hpp from parser interfaces |
| `TopologyModel` | `config/topology_model.hpp` | System topology (actors, dispatchers, system config) |
| `ActorFactoryRegistry` | `config/actor_factory_registry.hpp` | Maps behavior name strings to actor factory functions |
| `BinaryLoader` / `BinarySerializer` | `config/binary_*.hpp` | mmap-friendly binary topology format |
| `MetricsActor` / `MpscRingBuffer` | `metrics/*.hpp` | Lock-free ring buffer instrumentation, OpenMetrics /metrics endpoint for Prometheus |
| `CliActor` / `CommandNode` | `cli/*.hpp` | Interactive CLI with trie-based command tree, InspectState introspection, paged output |
| `IServiceDiscovery` | `net/service_discovery.hpp` | Pluggable discovery interface (UdpRegistrar, Gossip, Static, Hybrid) |
| `GossipMembership` | `net/gossip_membership.hpp` | SWIM protocol for decentralized cross-server discovery |
| `ActorLocationCache` | `net/actor_location_cache.hpp` | TTL cache for ActorId → EndPoint resolution |

### Production Architecture Backlog

The production reliability docs are architecture requirements and design
backlog. Do not assume a backlog item is runtime behavior until code proves it.
Current implemented foundations include scheduled messages, delivery-mode
configuration, receiver deduplication, structured failure envelopes, bounded
mailboxes, DLQ, distributed tracing, HTTP gateway, graceful shutdown, actor
lifecycle, and actor quarantine. Durable outbox/inbox, ACK/NACK retry, cluster
control, security, and operations-plane admin APIs remain design/backlog.

Key files:

| Document | Purpose |
|----------|---------|
| `docs/architecture/production/production-reliability-plane.md` | Top-level 24x7 reliability roadmap |
| `docs/architecture/production/architecture-requirement-backlog.md` | Summary production requirement backlog |
| `docs/architecture/production/feature-gap-refined-requirement-backlog.md` | Detailed subsystem requirement cards |
| `docs/architecture/production/actor-delivery-semantics-design.md` | Delivery results, TTL, retry, duplicate semantics |
| `docs/architecture/production/dead-letter-queue-design.md` | Dead-letter record, retention, replay, observability |
| `docs/architecture/production/cluster-failure-model-design.md` | Node state, partition, quarantine, fencing model |
| `docs/architecture/production/cluster-sharding-placement-design.md` | Shards, placement, handoff, route invalidation |
| `docs/architecture/production/security-architecture-design.md` | mTLS, authorization, audit, secret handling |
| `docs/architecture/production/operations-sre-design.md` | Health, admin API, incident timeline, SLO signals |
| `docs/architecture/production/chaos-reliability-testing-design.md` | Fault injection, chaos, soak, fuzz, compatibility tests |

### Message Flow

Actors communicate via protobuf `TypedMessage` (TypeTag + payload). From within an actor:
- `context()->send(addr, msg)` — send message
- `context()->reply(msg)` — reply to sender
- `context()->reply_with_error(code)` — reply with error
- `context()->schedule(delay, msg)` — schedule self-delivery after delay (returns `AlarmHandle`)
- `context()->cancel_schedule(handle)` — cancel a pending scheduled message
- `become(Behavior)` — change behavior dynamically
- `co_await mailbox_awaiter` — suspend until message arrives (coroutine actors)

Proto actors register handlers declaratively:
- `on<T>(handler)` — fire-and-forget proto handler
- `on_request<ReqT, ResT>(handler)` — request-response proto handler

### Topology Configuration

The TOML config topology system bootstraps actor trees from declarative config:

```
ActorSystem::load_topology("config.toml")  // parse + bootstrap
```

Pipeline: TOML file(s) → `TomlParser::parse()` (coordinator) → registered subsystem parsers → `TopologyModel` → `BootstrapEngine::execute()` → spawned actors. The `tools/toml-compiler/` AOT compiler pre-compiles TOML to a compact binary format for production (zero-parse mmap bootstrap).

Subsystem parsers self-register via file-scope static registrar objects (`TomlSystemParserRegistration<T>` / `TomlDocumentParserRegistration<T>`). New TOML subsystem config is added by creating a parser source file in `src/config/parsers/` with a registrar — no edits to `parse_file_data` needed. Parser interfaces use opaque `TomlTableView` so no public header includes `toml.hpp`.

### Supervision

- `OneForOneSupervisor` — only failed child restarts
- `AllForOneSupervisor` — all children restart when one fails
- `SupervisorActor` — supervises children via strategy pattern
- `SelfSupervisingActor` — manages own children with policy (max restarts, interval)

### Design Constraints

- C++20, no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`)
- Header-only types + compiled runtime (hpactor_lib)
- LLVM coding standards (see CMakeLists.txt compiler flags)
- `-fexceptions` enabled only for `src/config/toml_parser.cpp`, `src/config/toml_table_view.cpp`, and `tools/toml-compiler/compiler.cpp` (toml++ requires exceptions in including TUs)
- System packages: OpenSSL, Protobuf; vendored: llhttp, toml++ v3.4.0 (single-header in `third_party/`)

### Implementation Constraints

- Do not introduce `dynamic_cast`, `typeid`, exception-based control flow, or
  public APIs that require RTTI/exceptions.
- Keep blocking I/O and long-running work out of event-loop and cooperative
  scheduler paths; use the existing daemon, blocking, dense-compute, or async
  abstractions where appropriate.
- Maintain memory-accounting and allocator ownership rules when adding queues,
  envelopes, buffers, or actor state. Bounded capacity and explicit failure
  paths are preferred over unbounded growth.
- When changing lock-free, scheduler, mailbox, timer, or transport code, state
  the concurrency contract in the design and add focused stress or race-oriented
  tests where practical.
- Keep generated/protobuf contracts and TypeTag assignments explicit and
  backward-aware. Compatibility checks are required for protocol, binary
  topology, or persisted-state changes.
- Tests should match risk: narrow unit tests for local behavior, integration
  tests for actor/network/config interactions, and sanitizer/chaos/soak coverage
  for reliability-plane features.

### Test Design Constraints

Tests must be deterministic across platforms, build configurations, and CI
environments. The following rules prevent flaky tests:

- **No timing assumptions.** Never assume a timer fires within N ms, a thread
  completes within a deadline, or a sleep is "long enough." Use condition-based
  polling with generous timeouts (5s+) for tests that genuinely need the
  scheduler, or disable the scheduler (`scheduler_threads = 0`) for tests that
  inspect mailbox/lifecycle state directly.
- **No assumed thread execution order.** Never assume the scheduler processes a
  message before or after a specific line of test code. If a test sends a
  message and then inspects the mailbox, the scheduler may have already drained
  it. Use `scheduler_threads = 0` when the test needs to observe intermediate
  state (mailbox contents, lifecycle transitions, backpressure thresholds).
- **No platform-specific syscall behavior in assertions.** Behaviors that differ
  between Linux and macOS (e.g., `sendto` on connected sockets returning
  EISCONN vs. silently succeeding, `readv` with zero-length buffers, signal
  delivery in forked children, page sizes) must be guarded with `#ifdef` or
  tested portably. Prefer testing the observable outcome rather than the
  specific errno or signal number.
- **Non-blocking I/O for async tests.** Any test that calls the epoll/kqueue
  backend's `async_recv`/`async_send` (which loop until EAGAIN) must use
  non-blocking file descriptors. Blocking fds cause infinite hangs in the
  edge-triggered drain loop.
- **No reliance on NDEBUG-compiled-out asserts for control flow.** Tests must
  fail explicitly (return non-zero, print FAIL) rather than relying solely on
  `assert()` which is removed in Release builds. Use `assert` for invariants
  that indicate test infrastructure bugs, not for the condition under test.
- **Inject messages directly for mailbox/drain tests.** Use
  `mailbox->inject_for_test()` to place messages without triggering scheduler
  notification. This avoids races where the scheduler processes messages before
  the test can observe them.
- **Generous CI timeouts.** Tests that require the scheduler to process messages
  (link/monitor, concurrent sends) should poll with at least 5s timeout. Set
  CMake `TIMEOUT` properties for tests that legitimately need more than the
  global ctest timeout.

## Important Files

- `include/hpactor/` — public headers (actor, cli, config, core, mailbox, metrics, mem, net, ref, rpc, sched, spawn, supervision, types)
- `src/` — implementation files (linked into hpactor_lib)
- `tests/` — 187 test source files in three-tier structure (unit, integration, system) using Google Test; 29 GTest binaries are discovered through CTest
- `examples/` — 12 API usage examples
- `tools/toml-compiler/` — AOT TOML-to-binary compiler
- `third_party/` — vendored dependencies (googletest v1.14.0, llhttp, toml++)
- `cmake/` — CMake modules (gtest, protobuf codegen, toml++ interface target)
- `docs/architecture/production/` — production reliability plane, missing design docs, and refined requirement backlog
- `docs/superpowers/tutorials/actor-framework-tutorial.md` — usage guide
- `.claude/projects/*/memory/` — persistent project memory
