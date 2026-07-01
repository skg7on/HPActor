# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Session Warmup

At the start of a substantive task, read these files first:

1. `AGENTS.md` — Codex-facing working instructions for this repo.
2. `CLAUDE.md` — parallel Claude-facing instructions; keep shared build and architecture guidance in sync when it changes.
3. `CLAUDE_MEMORY.md` — project memory summary with current feature status, implementation history, docs, and recent test counts.
4. `HPACTOR_PROJECT_OUTLINE.md` — Project reference for directory layout, architecture overview, key subsystems, and quick navigation.

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

## Build Commands

```bash
# Configure and build
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build

# Run tests
ctest --output-on-failure

# Run a single test
./build/tests/test_<name>

# Build with sanitizers
cmake -DENABLE_TSAN=ON ..  # ThreadSanitizer
cmake -DENABLE_ASAN=ON ..  # AddressSanitizer

# Build options
cmake -DENABLE_EXAMPLES=OFF ..    # Disable examples (default ON)
cmake -DENABLE_APPS=OFF ..        # Disable complex demo applications (default ON)
cmake -DENABLE_PROACTOR=ON ..     # Enable proactor backend
cmake -DENABLE_MEMORY_DEBUG=ON .. # Enable memory poisoning + canary verification
cmake -DENABLE_ACTOR_METRICS=OFF .. # Disable actor-level metrics (default ON)
cmake -DENABLE_CLI=OFF ..       # Disable interactive CLI subsystem (default ON, runtime opt-in via cli.enabled)
cmake -DENABLE_CLANG_TIDY=ON .. # Enable clang-tidy checks during C++ builds (default OFF)
```

## Build Verification Discipline

After code modifications, do not rebuild the whole project by default. Prefer
the narrowest verification that covers the changed surface, such as a targeted
`ninja` target, one test binary, or `ctest -R <pattern> --output-on-failure`.
Run a full configure/build/test cycle only when it is necessary because the
change affects shared build configuration, generated files, broad public
headers, cross-cutting runtime behavior, or when the user explicitly asks for
full-project verification.

## Project Reference

Architecture overview, directory layout, actor type hierarchy, key subsystems, design constraints, build options, and key file locations are documented in [HPACTOR_PROJECT_OUTLINE.md](HPACTOR_PROJECT_OUTLINE.md). Read it after the warmup files to orient yourself in the codebase.

## Actor Concurrency Rules

Before designing or implementing features that touch actor delivery, mailboxes,
lock-free queues, scheduler state, worker placement, timers, or actor
multi-threading, read
`docs/architecture/mailbox/actor-concurrency-and-lockfree-mailbox-rules.md`.
Treat it as the normative rule set for MPSC mailbox use, actor state ownership,
ready-gate transitions, implementation contracts, and concurrency test design.

## Production Reliability Direction

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

## Agent Operating Rules

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

## Implementation Constraints

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
