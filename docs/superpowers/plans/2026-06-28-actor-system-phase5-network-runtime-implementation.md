# ActorSystem Phase 5 NetworkRuntime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` task-by-task. Invoke the repository
> `.claude/skills/tddflow-development/` skill before production edits and
> `superpowers:verification-before-completion` before commits or completion
> claims. Checkboxes are execution state, not evidence until commands pass.

**Goal:** Replace facade/shell network ownership with one side-effect-free,
result-started, idempotently stopped `NetworkRuntime` using one event loop and
fixed callback ports.

**Architecture:** `NetworkRuntime` owns `TcpTransport`, its authoritative loop
and thread, discovery/registrar, location cache, timers, RPC, HTTP client, and
remote-spawn protocol integration. Router, actor, messaging, and stream policy
remain in their existing owners and are reached through fixed ports.

**Tech Stack:** C++20, CMake, Ninja, GoogleTest, CTest architecture checks,
HPActor `result<T>`, fixed function-pointer ports, ASan, TSAN, no RTTI or
exception control flow.

## Global Constraints

- Start only after Phases 0–4 are merged into `origin/main`.
- Work in `.claude/worktrees/actor-system-network-runtime/` on branch
  `refactor/actor-system-network-runtime`, created from updated `origin/main`.
- Before every write, verify that exact worktree and branch.
- Follow RED -> GREEN -> REFACTOR and record the RED command before production
  edits.
- Preserve public `ActorSystem` network signatures, defaults, constness,
  result behavior, and `noexcept` guarantees.
- Use `TcpTransport::loop()` as the only authoritative loop; do not recreate
  `ActorSystem::network_loop_` elsewhere.
- Keep protocol routing in `InboundFrameRouter`, delivery/retry policy in
  `MessagingRuntime`, stream policy in `StreamRuntime`, and actors in
  `ActorRuntime`.
- Construction must not start threads, listen, publish discovery membership,
  register timers, or install actor endpoints.
- Never hold the lifecycle lock across a port call, transport/discovery call,
  loop wait/join, telemetry, actor code, or user callback.
- Quiesce callback sources and join the loop before destroying targets.
- Add no service locator, generic DI container, unbounded callback queue,
  per-frame virtual dispatch, `dynamic_cast`, `typeid`, or exceptions.
- Use this worktree's `build/`, `build-asan/`, and `build-tsan/`.

## Design References

- `docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`
- `docs/superpowers/specs/2026-06-28-actor-system-phase4-frame-stream-routing-design.md`
- `docs/superpowers/specs/2026-06-28-actor-system-phase5-network-runtime-design.md`
- `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- `docs/architecture/production/production-reliability-plane.md`
- `.claude/rules`

## Expected File Structure

**Create:**

- `include/hpactor/runtime/network_runtime.hpp`
- `src/runtime/network_runtime.cpp`
- `include/hpactor/runtime/network_runtime_ports.hpp`
- `include/hpactor/net/network_snapshot.hpp`
- `tests/unit/runtime/test_network_runtime.cpp`
- `tests/integration/net/test_network_runtime_lifecycle.cpp`
- `tests/integration/net/test_network_runtime_services.cpp`
- `tests/architecture/assert_network_runtime_boundaries.cmake`

**Modify:** runtime CMake/Impl files; `TcpTransport`; discovery/registrar
interfaces needed for detachable subscriptions; public facade forwards;
remote-spawn adapters; focused test CMake files; lifetime inventory; and
`CLAUDE_MEMORY.md`.

Private component/port headers stay under `src/runtime/`. Public snapshots are
bounded values and expose no transport implementation object.

---

### Task 0: Create the worktree and freeze the current contract

**Deliverable:** Passing focused baseline and a committed network ownership,
callback, thread, and shutdown inventory.

- [ ] Create and verify the worktree:

```bash
git fetch origin
git worktree add -b refactor/actor-system-network-runtime \
  .claude/worktrees/actor-system-network-runtime origin/main
cd .claude/worktrees/actor-system-network-runtime
pwd
git branch --show-current
git status --short
```

- [ ] Verify Phase 4 prerequisites:

```bash
test -f src/net/inbound_frame_router.hpp
test -f src/actor/stream_runtime.hpp
test -f src/runtime/messaging_runtime.hpp
rg -n "InboundFrameSink|class InboundFrameRouter|class StreamRuntime" src
```

- [ ] Read `AGENTS.md`, `CLAUDE.md`, `CLAUDE_MEMORY.md`, `.claude/rules`, and
  the actor concurrency rules.
- [ ] Configure/build/run focused net, RPC, remote-spawn, and shutdown tests.
- [ ] Update the Phase 1 lifetime inventory with current owner, loop/thread,
  callback source/target, stop trigger, quiescence barrier, and destruction
  order for both loops, transport, discovery, timers, cache, RPC, HTTP, and
  spawn receiver.
- [ ] Add RED characterization for loop progress/identity, disabled null
  accessors, discovery leave, socket/RPC progress, and pending-work shutdown.
- [ ] Commit as `test: characterize ActorSystem network lifecycle`.

---

### Task 1: Add value types, ports, and lifecycle skeleton

**Deliverable:** Independently tested component state machine without real
network side effects.

- [ ] RED-test constructed/running/stopped/failed transitions, concurrent
  start/stop, idempotent stop, start-after-stop rejection, disabled state,
  bounded snapshots, and loop-thread `StopDeferred`.
- [ ] Add `NetworkSnapshot`, state/stop enums, and reviewed error codes.
- [ ] Add private `NetworkRuntimeConfig`, `NodeEventSink`,
  `OutboundRetryPort`, `RemoteSpawnPort`, and `NetworkTelemetryPort`.
- [ ] Add one lifecycle mutex/condition variable, atomic ingress gate, staged
  rollback records, and a destructor that calls the same stop operation.
- [ ] Ensure ports are fixed-size and trivially copyable where applicable.
- [ ] Run `test_unit_runtime` filters for state/disabled behavior.
- [ ] Commit as `refactor: add NetworkRuntime lifecycle boundary`.

---

### Task 2: Move transport, authoritative loop, and thread

**Deliverable:** One runtime-owned transport loop and thread; no shell loop or
thread.

- [ ] RED-test that `event_loop() == &transport()->loop()`, timer/socket work
  progresses on the runtime thread, stop closes ingress before join, and
  repeated stop joins once.
- [ ] Make listen, handler detach, loop stop, and failure reporting explicit
  where current transport APIs cannot prove lifecycle behavior.
- [ ] Move `transport_` and `network_thread_` into `NetworkRuntime`; delete
  `network_loop_`.
- [ ] Install exactly one Phase 4 `InboundFrameSink` before listen.
- [ ] Start the thread with a progress barrier; do not publish `Running` before
  the loop accepts work.
- [ ] Fault-inject transport creation, handler installation, listen, thread
  creation, and barrier failure; assert reverse rollback and no live handler.
- [ ] Run unit/runtime and socket-level integration filters.
- [ ] Commit as `refactor: move transport loop into NetworkRuntime`.

---

### Task 3: Move discovery, registrar, and location cache

**Deliverable:** One owner and a provable membership-callback quiescence
boundary.

- [ ] RED-test injected discovery, UDP registrar, static discovery, join/leave,
  callback after ingress close, stop during callback, cache ownership, and
  failure after discovery start.
- [ ] Add a detachable subscription token plus drain, or equivalent explicit
  quiescence contract, to discovery implementations.
- [ ] Construct loop-aware discovery/registrar with
  `&transport_->loop()` and forward membership only through `NodeEventSink`.
- [ ] Move `location_cache_` and compatibility accessors.
- [ ] Prove stop disables membership callbacks, stops discovery, drains an
  in-flight callback, then permits target destruction.
- [ ] Run discovery/registrar/cache filters and commit as
  `refactor: move discovery and cache into NetworkRuntime`.

---

### Task 4: Move maintenance timers and retry wiring

**Deliverable:** Cache purge and reliable retry use the one loop and narrow
ports with cancellation barriers.

- [ ] RED-test due retry, cache purge, stop-before-fire, stop-during-callback,
  and no callback after `Stopped`, using controlled loop/clock barriers.
- [ ] Document/implement timer cancellation so no future callback starts and
  loop join covers an already-running callback.
- [ ] Register both timers during startup. Callbacks capture the component only,
  check ingress, then call owned cache state or `OutboundRetryPort`.
- [ ] Expose retry work from `MessagingRuntime` through the fixed port.
- [ ] Delete shell timer ids and direct tracker access.
- [ ] Run focused runtime/net/msg tests and commit as
  `refactor: move network maintenance timers`.

---

### Task 5: Move RPC and HTTP client ownership

**Deliverable:** Both clients use the authoritative loop and cannot callback
after component stop.

- [ ] RED-test RPC response, timeout/retry, late response, cancellation, HTTP
  progress, HTTP-disabled state, rollback, and stop with pending work.
- [ ] Construct `RpcChannel` under `NetworkRuntime`, install its handler before
  listen, cancel pending work during stop, and detach after join.
- [ ] Construct `HttpClient` only when enabled and pass
  `&transport_->loop()`; add cancel/drain if destruction is not a barrier.
- [ ] Forward legacy accessors and ask operations through the optional runtime;
  return null or `NetworkDisabled` as specified.
- [ ] Run RPC/HTTP/net tests and commit as
  `refactor: move RPC and HTTP into NetworkRuntime`.

---

### Task 6: Move remote-spawn network integration

**Deliverable:** Network runtime owns protocol registration; actor runtime owns
the receiver actor and canonical adoption state.

- [ ] RED-test reserved address, mailbox policy, context, system flag,
  directory ownership, failure rollback, stop removal, and wire behavior.
- [ ] Implement `RemoteSpawnPort` through the Phase 2 system-actor adoption
  path; expose no actor-directory internals to the network component.
- [ ] Move spawn protocol handler/client registration into `NetworkRuntime`.
- [ ] Pair every successful receiver install with one rollback/stop removal.
- [ ] Delete manual facade construction/insertion of receiver, mailbox,
  context, and directory entry.
- [ ] Run spawn/actor/net tests and commit as
  `refactor: isolate remote spawn network integration`.

---

### Task 7: Integrate and delete shell network ownership

**Deliverable:** One optional `NetworkRuntime` field in `Impl`, thin facade
forwards, and no obsolete owner field or callback.

- [ ] RED compile/execute enabled, disabled, running, and stopped behavior for
  all legacy network accessors and operations.
- [ ] Translate current config into `NetworkRuntimeConfig`, inject fixed ports,
  and start from the Phase 1 shell. Preserve constructor compatibility until
  the Phase 6 factory exists.
- [ ] Replace shell/destructor network teardown with one `stop()` call and route
  `StopDeferred` to the existing non-network owner thread.
- [ ] Delete loop/thread/transport/discovery/registrar/cache/timer/RPC/HTTP and
  spawn owner fields from facade/shell.
- [ ] Add an architecture check that rejects those field types outside
  `NetworkRuntime`, facade/Impl callback captures, and a second runtime loop.
- [ ] Run net/RPC/actor/architecture tests and commit as
  `refactor: make NetworkRuntime the network owner`.

---

### Task 8: Produce failure, sanitizer, and compatibility evidence

**Deliverable:** Reviewable proof of one-loop and callback-lifetime invariants.

- [ ] Run focused normal verification:

```bash
ninja -C build hpactor_lib test_unit_net test_unit_runtime test_unit_actor \
  test_unit_msg test_integration_net test_integration_rpc \
  test_integration_actor test_architecture
ctest --test-dir build --output-on-failure \
  -R 'network|transport|discovery|registrar|rpc|http|remote_spawn|architecture'
```

- [ ] Run the full startup fault matrix and assert terminal state, reverse
  rollback trace, zero live callbacks, zero joinable threads, and paired spawn
  install/removal.
- [ ] Configure ASan and run runtime/net/RPC/actor lifecycle tests.
- [ ] Configure TSAN and run concurrent send/snapshot/stop, callback teardown,
  self-stop deferral, and RPC shutdown tests.
- [ ] Compile all examples/apps that use legacy network accessors.
- [ ] Update `CLAUDE_MEMORY.md` and the lifetime inventory with owners, start
  and stop order, rollback matrix, evidence, and Phase 6 handoff.
- [ ] Verify old fields/captures are absent:

```bash
rg -n "network_loop_|network_thread_|cache_purge_timer_|retry_timer_" \
  include/hpactor/actor src/actor src/runtime
rg -n "\[this\].*(member|frame|rpc|retry|purge)" src/runtime src/net
git diff --check
git status --short
```

- [ ] Commit evidence as `docs: record NetworkRuntime ownership evidence`.

## Definition of Done

- [ ] Exactly one network loop and one owning thread exist.
- [ ] No network resource owner field remains in facade/shell.
- [ ] Construction has no side effect and startup returns a result.
- [ ] Every partial-start stage has a tested reverse rollback.
- [ ] Stop is idempotent and callback-quiescent.
- [ ] No network callback captures `ActorSystem` or `Impl`.
- [ ] Disabled networking creates no dummy service.
- [ ] Remote spawn uses canonical actor adoption.
- [ ] Public compatibility APIs and consumers compile unchanged.
- [ ] Focused normal, architecture, ASan, and TSAN evidence passes.
