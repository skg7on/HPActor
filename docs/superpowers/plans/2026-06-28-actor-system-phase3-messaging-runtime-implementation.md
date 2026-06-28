# ActorSystem Phase 3 MessagingRuntime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Invoke the
> repository `.claude/skills/tddflow-development/` skill before production
> edits and `superpowers:verification-before-completion` before commits or
> completion claims. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `MessagingRuntime` the sole owner of ActorSystem delivery policy
and messaging state, remove facade-capturing/late-wired delivery dependencies,
and preserve one full ordinary local/decoded-remote ingress plus an explicitly
restricted fast path.

**Architecture:** `MessagingRuntime` owns the stable DLQ, dedup cache, reliable
and compatibility outbound trackers, `BackpressureCoordinator`,
`DeliveryPipeline`, and `LocalDeliveryEngine`. In-process hot-path dependencies
are concrete references. Reliable ACK and remote-backpressure output cross the
future network boundary through fixed function-pointer/context ports bound to
stable transitional network state. `ActorSystem` remains a public facade and a
temporary frame demultiplexer until Phase 4.

**Tech Stack:** C++20, CMake, Ninja, GoogleTest 1.14, CTest architecture
scripts, HPActor `result<T>`, ASan, TSAN, existing protobuf/wire contracts, no
RTTI, no exception-based control flow.

## Global Constraints

- Start only after Phase 0, Phase 1, and Phase 2 are merged into `origin/main`
  and their focused normal/ASan/TSAN evidence is available.
- Execute in `.claude/worktrees/actor-system-messaging-runtime/` on branch
  `refactor/actor-system-messaging-runtime`, created from updated
  `origin/main`. This follows `.claude/rules` category/description and worktree
  naming requirements.
- Before every write, verify `pwd` ends in
  `.claude/worktrees/actor-system-messaging-runtime` and
  `git branch --show-current` prints
  `refactor/actor-system-messaging-runtime`.
- Follow RED -> GREEN -> REFACTOR for every production change. Run and record
  each stated RED command before editing its production files.
- Preserve all existing public `ActorSystem` delivery, remote delivery, DLQ,
  dedup, reliable ACK, tracker, and backpressure signatures, defaults,
  constness, return values, and `noexcept` guarantees.
- Keep `ActorSystem::deliver_remote()` as the Phase 3 frame classifier. Do not
  introduce `InboundFrameRouter`, move stream registries, or extract network
  lifecycle on this branch.
- Ordinary local and decoded ordinary-remote actor data must use the full
  `DeliveryPipeline`. The fast engine is limited to the reviewed stream and
  explicit compatibility allowlist.
- Preserve current delivery policy order, message metadata, wire bytes, ACK
  status values, result mapping, DLQ reason mapping, metrics, and logging.
- Preserve one stable DLQ address through reload and until scheduler workers,
  network ingress, and retry timers stop.
- Keep mailbox MPSC, reservation/release, pressure-state, ready-gate,
  single-consumer, and lost-wakeup contracts unchanged.
- Never invoke actor context, metrics formatting, logging, transport, or test
  callbacks while holding a directory lock or mailbox reservation.
- Do not introduce `dynamic_cast`, `typeid`, exception control flow, a service
  locator, generic DI container, virtual hot-path interface, component-wide
  messaging mutex, or public runtime-internal header.
- Do not merge `msg::OutboundDeliveryTracker` and
  `mailbox::OutboundTracker`, change retry semantics, or claim reliable resend
  completion.
- Use the worktree's own `build/`, `build-asan/`, and `build-tsan/` directories.

## Design References

- `docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`
- `docs/superpowers/specs/2026-06-27-actor-system-phase1-runtime-shell-design.md`
- `docs/superpowers/specs/2026-06-27-actor-system-phase2-actor-runtime-design.md`
- `docs/superpowers/specs/2026-06-28-actor-system-phase3-messaging-runtime-design.md`
- `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- `docs/architecture/production/production-reliability-plane.md`

## Expected File Structure

**Create:**

- `src/runtime/messaging_network_ports.hpp` — fixed reliable ACK and remote
  pressure output ports.
- `src/runtime/messaging_runtime.hpp` — private component API and ownership.
- `src/runtime/messaging_runtime.cpp` — composition, forwarding, control, and
  reconfigure implementation.
- `tests/unit/mailbox/test_messaging_network_ports.cpp` — port argument and
  unavailable-state coverage.
- `tests/unit/mailbox/test_messaging_runtime.cpp` — ownership, identity,
  delivery, fast-reason, tracker, and reconfigure tests.
- `tests/integration/actor/test_actor_system_messaging_runtime.cpp` — facade
  parity, decoded-remote convergence, lifecycle, and cached-accessor tests.
- `tests/architecture/assert_messaging_runtime_boundaries.cmake` — ownership,
  callback, fast-path, and facade fitness checks.

**Modify:**

- `src/runtime/CMakeLists.txt`
- `src/runtime/actor_system_impl.hpp`
- `src/runtime/actor_system_impl.cpp`
- `include/hpactor/actor/actor_system.hpp`
- `src/actor/actor_system.cpp`
- `include/hpactor/mailbox/delivery_pipeline.hpp`
- `src/mailbox/delivery_pipeline.cpp`
- `include/hpactor/mailbox/backpressure_coordinator.hpp`
- `src/mailbox/backpressure_coordinator.cpp`
- `include/hpactor/mailbox/local_delivery_engine.hpp` only if the reason is
  enforced there rather than in `MessagingRuntime`.
- `src/msg/outbound_delivery_tracker.cpp` only for characterized adapter
  changes; do not redesign retry semantics.
- `tests/unit/mailbox/CMakeLists.txt`
- existing delivery pipeline tests in their actual Phase 2 location.
- `tests/integration/mailbox/test_dead_letter_queue.cpp`
- `tests/integration/mailbox/test_reliable_messaging.cpp`
- `tests/integration/actor/CMakeLists.txt`
- `tests/integration/actor/test_actor_system_backpressure.cpp`
- `tests/integration/actor/test_backpressure_signals.cpp`
- `tests/integration/actor/test_remote_backpressure_signals.cpp`
- `tests/integration/actor/test_remote_delivery_result.cpp`
- `tests/architecture/CMakeLists.txt`
- `docs/architecture/actor/actor-system-phase1-lifetime-inventory.md`
- `CLAUDE_MEMORY.md`

If Phase 1/2 produced slightly different private filenames, update this list to
the merged names before implementation. Do not create parallel duplicate
runtime shells to satisfy the planned names.

---

### Task 0: Create the implementation worktree and verify prerequisites

**Deliverable:** A clean Phase 3 worktree with merged Phase 0-2 contracts, a
passing focused baseline, and an updated messaging lifetime/call-site inventory.

- [ ] **Step 1: Update remote state and create the required worktree**

Run from the main checkout:

```bash
git fetch origin
git worktree add -b refactor/actor-system-messaging-runtime \
  .claude/worktrees/actor-system-messaging-runtime origin/main
cd .claude/worktrees/actor-system-messaging-runtime
pwd
git branch --show-current
git status --short
```

Expected: correct worktree path/branch and empty status.

- [ ] **Step 2: Verify Phase 1 and Phase 2 are present**

```bash
test -f src/runtime/actor_system_impl.hpp
test -f src/runtime/actor_runtime.hpp
test -f docs/superpowers/specs/2026-06-28-actor-system-phase3-messaging-runtime-design.md
rg -n "ActorRuntime|ActorExecutionDependencies|MessagingRuntimeState" \
  src/runtime include/hpactor/sched
rg -n "DeadLetterQueue" \
  include/hpactor/sched/actor_execution_dependencies.hpp src/runtime
```

Expected: Phase 1 PImpl/state shell and Phase 2 actor/scheduler dependency
contracts exist. If not, stop; do not retrofit Phase 3 onto pre-Phase-2 code.

- [ ] **Step 3: Read mandatory guidance**

```bash
sed -n '1,260p' AGENTS.md
sed -n '1,260p' CLAUDE.md
sed -n '1,260p' CLAUDE_MEMORY.md
sed -n '1,260p' HPACTOR_PROJECT_OUTLINE.md
sed -n '1,360p' \
  docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md
sed -n '1,300p' \
  docs/architecture/production/production-reliability-plane.md
```

Expected: no newer repository rule conflicts with this plan. Newer rules take
precedence and any plan adjustment is recorded in the PR.

- [ ] **Step 4: Configure and build the focused baseline**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build hpactor_lib test_unit_mailbox test_unit_msg \
  test_integration_mailbox test_integration_actor test_integration_msg \
  test_integration_net test_integration_config test_integration_sched
```

Expected: all targets build.

- [ ] **Step 5: Run focused baseline tests**

```bash
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='*DeadLetter*:*Dedup*:*OutboundTracker*:*Pressure*:*Reservation*'
./build/tests/unit/msg/test_unit_msg \
  --gtest_filter='*OutboundDeliveryTracker*:*Delivery*'
./build/tests/integration/mailbox/test_integration_mailbox
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*Delivery*:*Backpressure*:*ActorSystem*'
./build/tests/integration/net/test_integration_net \
  --gtest_filter='*Transport*:*Outbound*'
./build/tests/integration/sched/test_integration_sched \
  --gtest_filter='*Expired*:*Scheduler*:*Worker*'
```

Expected: all selected tests pass. Record exact counts. If a filter matches
zero tests, use `--gtest_list_tests` and replace it with exact existing suites
before production edits.

- [ ] **Step 6: Inventory current ownership and dataflow**

Extend
`docs/architecture/actor/actor-system-phase1-lifetime-inventory.md` with:

- every construction/access/destruction site for the seven messaging-owned
  components;
- every `DeliveryPipeline::Config` callback and captured object;
- every late dependency setter;
- every full- and fast-delivery caller and its message class;
- every reliable ACK/NACK mutation and retry timer callback;
- every DLQ pointer consumer, including scheduler and operations views;
- all topology reload mutations; and
- required outlives edges among directory, scheduler, messaging, network,
  telemetry, timers, and facade.

Use codebase-memory graph discovery first. Then use literal search as a
completeness check:

```bash
rg -n "DeliveryPipeline|LocalDeliveryEngine|BackpressureCoordinator|DedupCache" \
  src include tests
rg -n "OutboundDeliveryTracker|mailbox::OutboundTracker|dead_letter_queue" \
  src include tests
rg -n "try_deliver_local_fast|deliver_remote|set_metrics|set_transport" \
  src include tests
```

Expected: every match is classified as owner, borrower, adapter, test, or stale
code. Commit only the inventory:

```bash
git add docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "docs: inventory MessagingRuntime dependencies"
```

---

### Task 1: Characterize delivery, metadata, identity, and fast-path behavior

**Deliverable:** Failing-on-intent characterization tests that freeze the Phase
3 compatibility boundary before structural edits.

**Files:**

- Modify: existing delivery pipeline tests in their merged location.
- Modify: `tests/integration/mailbox/test_dead_letter_queue.cpp`
- Modify: `tests/integration/mailbox/test_reliable_messaging.cpp`
- Modify: `tests/integration/actor/test_remote_delivery_result.cpp`
- Create: `tests/integration/actor/test_actor_system_messaging_runtime.cpp`
- Modify: `tests/integration/actor/CMakeLists.txt`

- [ ] **Step 1: Add a metadata parity matrix**

Cover local full delivery and decoded ordinary remote delivery for:

- sender and receiver address;
- type tag and payload;
- trace context and trace-present flag;
- priority and EDF flag;
- explicit deadline and default TTL;
- delivery mode, message id, ACK-requested flag; and
- accepted, missing actor, duplicate, expired, circuit-open, and mailbox-full
  results.

Assert mailbox envelope, DLQ record, reliable tracker, ACK arguments, and metric
event fields where each outcome applies.

- [ ] **Step 2: Add stable-identity characterization**

Cache pointers returned by DLQ, dedup, reliable tracker, and compatibility
tracker accessors. Exercise supported topology reload and assert the addresses
and continued behavior remain valid. Assert scheduler expiry and delivery
rejection are visible through the same cached DLQ.

- [ ] **Step 3: Characterize fast-path callers**

Add tests proving current stream data/ACK/close/error delivery continues through
the fast path and that ordinary remote data performs full TTL/dedup/DLQ policy.
Record every approved production fast caller in the test description.

- [ ] **Step 4: Characterize tracker separation and retry gap**

Prove ACK/NACK affects the current reliable tracker and does not mutate
`mailbox::OutboundTracker`. Prove the current timer/retry callback behavior,
including the absence of completed transport resend if that remains the merged
implementation.

- [ ] **Step 5: Run the tests**

```bash
ninja -C build test_integration_mailbox test_integration_actor
./build/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='*Reliable*:*DeadLetter*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*MessagingRuntimeCharacterization*:*RemoteDelivery*:*Stream*'
```

Expected: characterization of existing behavior passes. Tests expressing the
new runtime boundary may fail because no `MessagingRuntime` exists; record the
exact RED assertions.

- [ ] **Step 6: Commit the characterization tests**

```bash
git add tests/integration/mailbox tests/integration/actor
git commit -m "test: characterize ActorSystem messaging boundaries"
```

---

### Task 2: Introduce fixed messaging network-control ports

**Deliverable:** Allocation-free, nullable control ports bound to stable
transitional network state, with byte/argument parity and no facade capture.

**Files:**

- Create: `src/runtime/messaging_network_ports.hpp`
- Create: `tests/unit/mailbox/test_messaging_network_ports.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`
- Modify: `src/runtime/actor_system_impl.hpp`
- Modify: `src/runtime/actor_system_impl.cpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Write failing port tests**

Test:

- null context/function is a safe no-op or returns the documented unavailable
  result;
- reliable ACK forwards target, acker, message id, status, and retry delay
  exactly once;
- backpressure forwards target and encoded bytes exactly once;
- port values are trivially copyable or otherwise allocation-free; and
- bound context is stable while the underlying transport transitions from
  unavailable to started to stopped.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='MessagingNetworkPortsTest.*'
```

Expected: compile/test failure because the ports do not exist.

- [ ] **Step 3: Implement the minimum port types**

Add `ReliableAckPort`, `BackpressureWirePort`, and `MessagingNetworkPorts` with
function pointer plus `void*` context. Mark calls `noexcept` where the existing
control path is non-throwing. Do not use `std::function` or a virtual interface.

- [ ] **Step 4: Bind ports to stable Phase 1 network state**

Add static/free adapter functions operating only on `NetworkRuntimeState` (or
the merged equivalent). Keep existing frame encoding and transport send code
there. Do not bind `ActorSystem*` or `Impl*`.

- [ ] **Step 5: Run GREEN and wire-byte regression tests**

```bash
ninja -C build test_unit_mailbox test_integration_actor test_integration_net
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='MessagingNetworkPortsTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*Reliable*:*RemoteBackpressure*'
./build/tests/integration/net/test_integration_net \
  --gtest_filter='*Outbound*:*Transport*'
```

Expected: all pass and encoded control bytes match baseline.

- [ ] **Step 6: Commit**

```bash
git add src/runtime src/actor/actor_system.cpp tests/unit/mailbox
git commit -m "refactor: add fixed messaging network ports"
```

---

### Task 3: Give `DeliveryPipeline` concrete in-process dependencies

**Deliverable:** The pipeline directly uses `ActorDirectory`, DLQ, dedup,
backpressure, reliable tracker, metrics, and `ReliableAckPort`; it stores no
facade callback and has no late metrics setter.

**Files:**

- Modify: `include/hpactor/mailbox/delivery_pipeline.hpp`
- Modify: `src/mailbox/delivery_pipeline.cpp`
- Modify: existing delivery pipeline unit tests.
- Modify: `src/actor/actor_system.cpp` temporarily for new construction.

- [ ] **Step 1: Write failing direct-dependency tests**

Construct a real `ActorDirectory`, queue, dedup cache, coordinator, tracker, and
capturing fixed ACK port. Verify lookup, mailbox delivery, local/remote pressure,
and ACK behavior without an `ActorSystem` fixture.

Add a compile-time/source fitness assertion that `DeliveryPipeline::Config`
contains no `std::function` actor lookup, mailbox lookup, pressure emitter, or
ACK emitter.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='DeliveryPipelineDependenciesTest.*'
```

Expected: compile/test failure against callback-based configuration.

- [ ] **Step 3: Replace actor/mailbox lookup callbacks**

Inject `ActorDirectory&` and call its established actor/mailbox lookup APIs.
Return/copy handles under the directory's current lock policy, then release any
lock before mailbox/context/control actions.

- [ ] **Step 4: Replace sibling callbacks**

Inject `BackpressureCoordinator&` and `ReliableAckPort`. Retain direct DLQ,
dedup, tracker, metrics, endpoint, and TTL dependencies. Prefer references for
required dependencies and explicit nullable pointers only where disabled state
is already valid.

- [ ] **Step 5: Remove late metrics mutation**

Delete `DeliveryPipeline::set_metrics()`. Reorder transitional construction so
the stable metrics pointer is available before pipeline construction. Do not
start producers until all dependencies exist.

- [ ] **Step 6: Run focused GREEN and policy regression**

```bash
ninja -C build hpactor_lib test_unit_mailbox test_integration_mailbox \
  test_integration_actor
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='DeliveryPipeline*:*DeadLetter*:*Dedup*:*Pressure*'
./build/tests/integration/mailbox/test_integration_mailbox
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*Delivery*:*Backpressure*'
```

Expected: all pass; policy-order and metadata tests are unchanged.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/mailbox/delivery_pipeline.hpp \
  src/mailbox/delivery_pipeline.cpp src/actor/actor_system.cpp tests
git commit -m "refactor: inject concrete delivery dependencies"
```

---

### Task 4: Remove late wiring from `BackpressureCoordinator`

**Deliverable:** Fixed directory, metrics, endpoint, and remote-output
dependencies; no production transport/metrics setter race.

**Files:**

- Modify: `include/hpactor/mailbox/backpressure_coordinator.hpp`
- Modify: `src/mailbox/backpressure_coordinator.cpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/actor/test_backpressure_signals.cpp`
- Modify: `tests/integration/actor/test_remote_backpressure_signals.cpp`
- Modify: `tests/integration/actor/test_actor_system_backpressure.cpp`

- [ ] **Step 1: Add failing construction/lifecycle tests**

Cover local delivery through a fixed `ActorDirectory&`, remote output through
`BackpressureWirePort`, metrics present/absent, network unavailable, and
network stopped. Add a TSAN-oriented test for signal emission while shutdown
makes the underlying network state unavailable.

- [ ] **Step 2: Define the test sink publication rule**

Choose one based on merged test usage:

- if every sink is installed pre-start, document/assert pre-start-only; or
- if runtime replacement is required, protect only `wire_sink_for_test_` with a
  coordinator-local mutex and copy it before invocation.

Never call the sink while holding that mutex.

- [ ] **Step 3: Run RED**

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*BackpressureCoordinatorDependencies*:*Backpressure*'
```

Expected: failure because late setters/raw transport are still required.

- [ ] **Step 4: Implement fixed dependencies**

Replace raw transport with `BackpressureWirePort`, use a typed metrics pointer
if header dependencies permit it, require a valid directory reference, and
delete production `set_metrics_ring_buffer()` and `set_transport()`.

- [ ] **Step 5: Verify no callback crosses a lock/reservation**

Add assertions/test hooks as practical. Review source ordering: directory
lookup and mailbox result must complete before actor context, metrics, encoding,
port, or test sink invocation.

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build hpactor_lib test_integration_actor test_integration_mailbox
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*Backpressure*'
./build/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='*Backpressure*'
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/mailbox/backpressure_coordinator.hpp \
  src/mailbox/backpressure_coordinator.cpp src/actor/actor_system.cpp \
  tests/integration/actor
git commit -m "refactor: fix backpressure dependency wiring"
```

---

### Task 5: Introduce `MessagingRuntime` and move component ownership

**Deliverable:** A private cohesive owner with stable component addresses and
correct reverse destruction, initially reached through existing facade methods.

**Files:**

- Create: `src/runtime/messaging_runtime.hpp`
- Create: `src/runtime/messaging_runtime.cpp`
- Create: `tests/unit/mailbox/test_messaging_runtime.cpp`
- Modify: `tests/unit/mailbox/CMakeLists.txt`
- Modify: `src/runtime/CMakeLists.txt`
- Modify: `src/runtime/actor_system_impl.hpp`
- Modify: `src/runtime/actor_system_impl.cpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Write failing ownership and identity tests**

Construct `MessagingRuntime` with a real directory and fixed test ports. Assert:

- all seven components are present through narrow methods;
- repeated access returns the same address;
- scheduler/dead-letter sink can retain the DLQ address;
- disabled telemetry/network is valid;
- construction performs no start/transport side effect; and
- destruction after producer/timer stop triggers no callback/use-after-free.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_mailbox
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='MessagingRuntimeTest.*'
```

Expected: compile failure because the component does not exist.

- [ ] **Step 3: Implement the minimal component**

Declare members in dependency order:

1. DLQ;
2. dedup cache;
3. reliable tracker;
4. compatibility outbound tracker;
5. backpressure coordinator;
6. delivery pipeline; and
7. local delivery engine.

Inject `ActorDirectory&`, metrics, endpoint, and fixed network ports. Add
forwarding/accessor methods only for existing behavior. Do not add a public
header or start/stop lifecycle yet.

- [ ] **Step 4: Move ownership from Phase 1 messaging state**

Replace the individual owners in `ActorSystem::Impl` with one
`MessagingRuntime` owner. Temporary facade code may forward to it. Remove old
owners in the same commit so there is one source of truth.

- [ ] **Step 5: Connect scheduler to the stable messaging DLQ**

Build Phase 2 `ActorExecutionDependencies` using
`messaging.dead_letters()`. Prove the directory and messaging pointee addresses
remain stable through any ownership transfer used during construction.

- [ ] **Step 6: Run GREEN plus lifecycle checks**

```bash
ninja -C build hpactor_lib test_unit_mailbox test_integration_actor \
  test_integration_mailbox test_integration_sched
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='MessagingRuntimeTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*ActorSystem*:*Delivery*:*Shutdown*'
./build/tests/integration/mailbox/test_integration_mailbox
./build/tests/integration/sched/test_integration_sched \
  --gtest_filter='*Expired*:*Scheduler*'
```

Expected: all pass and cached DLQ identity assertions hold.

- [ ] **Step 7: Commit**

```bash
git add src/runtime src/actor/actor_system.cpp tests/unit/mailbox \
  tests/integration/sched
git commit -m "refactor: add MessagingRuntime ownership"
```

---

### Task 6: Route facade delivery, DLQ, dedup, and backpressure APIs

**Deliverable:** Public APIs are direct adapters and contain no duplicated
messaging policy.

**Files:**

- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `src/runtime/messaging_runtime.hpp`
- Modify: `src/runtime/messaging_runtime.cpp`
- Modify: `tests/integration/actor/test_actor_system_messaging_runtime.cpp`
- Modify: existing DLQ/backpressure tests.

- [ ] **Step 1: Add facade parity tests**

Exercise every overload/default for:

- `try_deliver_local`;
- `deliver_with_result`;
- `deliver_local`;
- `deliver_local_edf`;
- dead-letter push/pop/snapshot/accessors;
- dedup and tracker accessors; and
- local/remote backpressure emission/handling.

Assert exact result, metadata, metrics, and pointer identity.

- [ ] **Step 2: Run RED architecture/parity assertions**

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemMessagingRuntimeTest.*'
```

Expected: behavior may pass, but new policy-free/ownership assertions fail
until all methods route through the component.

- [ ] **Step 3: Replace method bodies with forwards**

Move any remaining policy calculation into an existing messaging method.
Facade adapters may construct `DeliveryOptions` for a public overload, but may
not inspect directory/mailbox pressure, update trackers, or push DLQ records.

- [ ] **Step 4: Preserve source compatibility**

Keep inline public accessors only if they can delegate through the PImpl without
exposing private types. If Phase 1 already moved them out of line, retain that
structure. Do not change method signatures to references/results solely for
internal elegance.

- [ ] **Step 5: Run GREEN**

```bash
ninja -C build hpactor_lib test_integration_actor test_integration_mailbox \
  test_integration_ref test_integration_tracing
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemMessagingRuntimeTest.*:*Delivery*:*Backpressure*'
./build/tests/integration/mailbox/test_integration_mailbox
./build/tests/integration/ref/test_integration_ref \
  --gtest_filter='*DeadLetter*'
./build/tests/integration/tracing/test_integration_tracing \
  --gtest_filter='*ActorSystem*:*Message*'
```

Expected: all selected tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/actor/actor_system.hpp src/actor/actor_system.cpp \
  src/runtime tests/integration
git commit -m "refactor: route facade messaging APIs"
```

---

### Task 7: Restrict and classify fast delivery

**Deliverable:** Every fast delivery has an explicit reason and every ordinary
ingress remains on the full policy path.

**Files:**

- Modify: `src/runtime/messaging_runtime.hpp`
- Modify: `src/runtime/messaging_runtime.cpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: stream handler tests in `tests/integration/actor/`.
- Modify: `tests/integration/actor/test_actor_system_messaging_runtime.cpp`

- [ ] **Step 1: Write failing reason/allowlist tests**

Require `MessagingRuntime::try_deliver_fast()` to receive
`FastDeliveryReason`. Cover `StreamProtocol` and `CompatibilityExplicit`.
Assert ordinary local and decoded ordinary remote paths exercise full TTL,
dedup, circuit, DLQ, and pressure policy.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_mailbox test_integration_actor
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='MessagingRuntimeFastDeliveryTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*FastDelivery*:*Stream*:*RemoteDelivery*'
```

Expected: compile/assertion failure because reason classification is absent.

- [ ] **Step 3: Add the private reason enum and forwarding rule**

Keep the public `try_deliver_local_fast()` signature. Its implementation passes
`CompatibilityExplicit`. Existing stream handlers pass `StreamProtocol` until
Phase 4 moves them. Do not add an ordinary-delivery reason.

- [ ] **Step 4: Add source fitness allowlist**

Prepare an architecture assertion that direct `LocalDeliveryEngine` calls occur
only inside `MessagingRuntime`, and runtime fast calls occur only in the facade
compatibility adapter/current stream handlers.

- [ ] **Step 5: Run GREEN and mailbox stress**

```bash
ninja -C build test_unit_mailbox test_integration_actor
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='MessagingRuntimeFastDeliveryTest.*:*Reservation*:*Prearm*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*FastDelivery*:*Stream*:*RemoteDelivery*'
```

Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/runtime src/actor/actor_system.cpp tests
git commit -m "refactor: classify restricted fast delivery"
```

---

### Task 8: Move reliable control and retry interaction behind messaging

**Deliverable:** The facade parses Phase 3 wire fields but cannot mutate or
drive reliable tracker policy directly.

**Files:**

- Modify: `src/runtime/messaging_runtime.hpp`
- Modify: `src/runtime/messaging_runtime.cpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/unit/msg/test_outbound_delivery_tracker.cpp`
- Modify: `tests/integration/mailbox/test_reliable_messaging.cpp`
- Modify: `tests/integration/actor/test_actor_system_messaging_runtime.cpp`

- [ ] **Step 1: Write failing typed-handler tests**

Test `on_reliable_ack`, `on_reliable_nack`, and `process_retries` against the
real reliable tracker. Verify exact endpoint/message/reason/retry values and
that the compatibility tracker remains unchanged.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_mailbox test_unit_msg test_integration_mailbox
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='MessagingRuntimeReliableControlTest.*'
```

Expected: compile failure because typed runtime handlers are absent.

- [ ] **Step 3: Implement typed messaging handlers**

Add narrow methods that translate only into existing tracker APIs. Keep control
flag constants, protobuf field extraction, malformed-frame behavior, and stream
classification in `ActorSystem::deliver_remote()` for Phase 4.

- [ ] **Step 4: Route the retry timer through the component**

Replace raw tracker capture/access with a narrow messaging call. Ensure the
timer is cancelled/joined before `MessagingRuntime` destruction. Preserve the
current resend callback behavior exactly and leave a code comment/reference to
the reliability gap rather than inventing a resend implementation.

- [ ] **Step 5: Route outgoing ACK through the fixed port**

Ensure pipeline rejection/duplicate ACK and handler accepted ACK use the same
network port semantics. Keep public `send_reliable_ack()` as a compatibility
adapter to that port or the narrow network state; it must not become the
pipeline callback target.

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build hpactor_lib test_unit_msg test_unit_mailbox \
  test_integration_mailbox test_integration_actor
./build/tests/unit/msg/test_unit_msg \
  --gtest_filter='*OutboundDeliveryTracker*'
./build/tests/unit/mailbox/test_unit_mailbox \
  --gtest_filter='MessagingRuntimeReliableControlTest.*:*OutboundTracker*'
./build/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='*Reliable*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*Reliable*:*MessagingRuntime*'
```

Expected: all pass, including tracker-separation and current retry-gap tests.

- [ ] **Step 7: Commit**

```bash
git add src/runtime src/actor/actor_system.cpp tests/unit/msg \
  tests/unit/mailbox tests/integration
git commit -m "refactor: encapsulate reliable delivery control"
```

---

### Task 9: Converge decoded remote ordinary ingress and stable reconfigure

**Deliverable:** Remote ordinary messages use the same full pipeline and
supported reload changes preserve all messaging object identities.

**Files:**

- Modify: `src/runtime/messaging_runtime.hpp`
- Modify: `src/runtime/messaging_runtime.cpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: topology/config application code identified after Phase 2 merge.
- Modify: `tests/integration/actor/test_remote_delivery_result.cpp`
- Modify: `tests/integration/config/test_bootstrap_engine.cpp`
- Modify: `tests/integration/actor/test_actor_system_messaging_runtime.cpp`

- [ ] **Step 1: Add remote convergence tests**

For an ordinary decoded frame, assert the same outcomes and metadata as direct
full local delivery for accepted, duplicate, expired, missing actor,
circuit-open, and mailbox-full cases. Assert stream/control frames do not enter
that path.

- [ ] **Step 2: Add reconfigure identity/atomicity tests**

Cache every public messaging-owned pointer, apply supported live changes, and
assert addresses are unchanged. Submit an invalid multi-field delta and assert
no field is partially applied. Submit immutable/restart-required changes and
assert the established rejection.

- [ ] **Step 3: Run RED**

```bash
ninja -C build test_integration_actor test_integration_config
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*RemoteDeliveryConvergence*:*MessagingRuntimeReload*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='*Messaging*:*TopologyReload*:*Bootstrap*'
```

Expected: new convergence/identity or atomicity assertions fail.

- [ ] **Step 4: Forward ordinary decoded data once**

Keep frame decode and option assembly in the transitional facade, then call one
full `MessagingRuntime` entry. Remove any remote-specific mailbox admission or
DLQ policy duplicate.

- [ ] **Step 5: Implement narrow stable reconfigure**

Validate the entire delta, then call in-place component reconfigure APIs. Never
replace the DLQ, dedup cache, tracker, coordinator, pipeline, or engine.
Do not add TOML parsing or edit the monolithic parser beyond routing the already
validated effective change.

- [ ] **Step 6: Run GREEN**

```bash
ninja -C build hpactor_lib test_integration_actor test_integration_config \
  test_integration_mailbox
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='*RemoteDelivery*:*MessagingRuntimeReload*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='*Messaging*:*TopologyReload*:*Bootstrap*'
./build/tests/integration/mailbox/test_integration_mailbox
```

Expected: all pass and object identity remains stable.

- [ ] **Step 7: Commit**

```bash
git add src/runtime src/actor/actor_system.cpp tests/integration \
  src/config include/hpactor/config
git commit -m "refactor: converge ordinary messaging ingress"
```

Only add config paths that actually changed; do not stage unrelated files.

---

### Task 10: Add architecture fitness checks and close ownership gaps

**Deliverable:** Automated checks prevent the God Class responsibilities from
creeping back into the facade.

**Files:**

- Create: `tests/architecture/assert_messaging_runtime_boundaries.cmake`
- Modify: `tests/architecture/CMakeLists.txt`
- Modify: source files only for violations revealed by the checks.

- [ ] **Step 1: Add failing architecture assertions**

Enforce:

- construction of each messaging component occurs only in
  `src/runtime/messaging_runtime.cpp` and tests;
- delivery/mailbox code stores no `ActorSystem*`, `ActorSystem&`, or `Impl*`;
- no production lambda/callback captures `ActorSystem` for delivery, pressure,
  ACK, or retry;
- deleted late dependency setter names do not occur in production;
- `LocalDeliveryEngine` is directly called only by `MessagingRuntime`;
- approved `try_deliver_fast` callers match the exact allowlist;
- facade local delivery, DLQ, dedup, tracker, and backpressure methods contain
  only translation/forwarding code; and
- no `dynamic_cast`, `typeid`, `throw`, or public include of private runtime
  headers appears in Phase 3 files.

- [ ] **Step 2: Run RED**

```bash
cmake --build build --target test_architecture
ctest --test-dir build -R 'architecture.*messaging' --output-on-failure
```

Expected: checks fail on any remaining old owner/callback/policy copy.

- [ ] **Step 3: Remove every remaining violation**

Classify matches rather than broad text replacement. Test fixtures and public
compatibility declarations may use explicit narrow allowlists; production
policy/ownership violations may not.

- [ ] **Step 4: Run GREEN**

```bash
cmake --build build --target test_architecture
ctest --test-dir build -R 'architecture' --output-on-failure
```

Expected: all architecture checks pass.

- [ ] **Step 5: Commit**

```bash
git add tests/architecture src include/hpactor
git commit -m "test: enforce MessagingRuntime boundaries"
```

---

### Task 11: Verify concurrency, lifecycle, documentation, and phase boundary

**Deliverable:** Complete normal/sanitizer evidence, updated memory/lifetime
docs, and no accidental Phase 4/5 work.

**Files:**

- Modify: `docs/architecture/actor/actor-system-phase1-lifetime-inventory.md`
- Modify: `CLAUDE_MEMORY.md`
- Modify: tests/source only for verified defects.

- [ ] **Step 1: Update the final lifetime inventory**

Record:

- final owner/borrower graph;
- member declaration and reverse destruction order;
- start/stop order for ingress, retry timers, scheduler, messaging, directory,
  telemetry, and network state;
- stable DLQ/accessor identity contract;
- test-sink publication rule;
- full versus fast path allowlist; and
- explicit Phase 4 frame-routing and Phase 5 network-lifecycle handoff.

- [ ] **Step 2: Run focused normal verification from a clean build graph**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build hpactor_lib test_unit_mailbox test_unit_msg \
  test_integration_mailbox test_integration_actor test_integration_msg \
  test_integration_net test_integration_config test_integration_sched \
  test_integration_ref test_integration_tracing test_architecture
ctest --test-dir build -R \
  'unit_(mailbox|msg)|integration_(mailbox|actor|msg|net|config|sched|ref|tracing)|architecture' \
  --output-on-failure
```

Expected: all selected tests pass. Record exact count and elapsed time.

- [ ] **Step 3: Run targeted stress/repeat tests**

```bash
ctest --test-dir build -R \
  'Backpressure|Reliable|DeadLetter|Delivery|Reservation|Prearm' \
  --repeat until-fail:50 --output-on-failure
```

Expected: no failure. If CTest names differ, derive exact names with
`ctest --test-dir build -N` and record the replacement expression.

- [ ] **Step 4: Run ASan lifecycle and identity coverage**

```bash
cmake -S . -B build-asan -GNinja -DENABLE_ASAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-asan test_integration_actor test_integration_mailbox \
  test_integration_config test_integration_sched
ctest --test-dir build-asan -R \
  'MessagingRuntime|DeadLetter|Reliable|Delivery|Shutdown|TopologyReload|Scheduler' \
  --output-on-failure
```

Expected: all selected tests pass with no leak/use-after-free, including cached
DLQ use, retry cancellation, failed start, and destructor-only cleanup.

- [ ] **Step 5: Run focused TSAN concurrency coverage**

```bash
cmake -S . -B build-tsan -GNinja -DENABLE_TSAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-tsan test_unit_mailbox test_unit_msg \
  test_integration_mailbox test_integration_actor
ctest --test-dir build-tsan -R \
  'MessagingRuntime|Backpressure|Reliable|Dedup|Reservation|Mailbox' \
  --output-on-failure
```

Expected: all selected tests pass with no race, especially during pressure
emission, tracker ACK/timer overlap, live reconfigure, and shutdown.

- [ ] **Step 6: Run full verification because public headers and runtime
composition changed**

```bash
ninja -C build
ctest --test-dir build --output-on-failure
```

Expected: full build and test suite pass. Record exact count and elapsed time.

- [ ] **Step 7: Review the Phase 3 boundary**

```bash
git diff origin/main...HEAD --stat
git diff origin/main...HEAD -- \
  src/actor/actor_system.cpp src/runtime include/hpactor/mailbox \
  tests/architecture docs/architecture/actor CLAUDE_MEMORY.md
rg -n "InboundFrameRouter|class StreamRuntime|class NetworkRuntime" \
  src/runtime src/actor include/hpactor
```

Expected: no new Phase 4 router/stream owner or Phase 5 network owner. Existing
references are allowed; newly introduced implementations are not.

- [ ] **Step 8: Update project memory and commit documentation**

Add implementation summary, exact test evidence, known reliable-resend gap, and
Phase 4 handoff to `CLAUDE_MEMORY.md`.

```bash
git add docs/architecture/actor/actor-system-phase1-lifetime-inventory.md \
  CLAUDE_MEMORY.md
git commit -m "docs: record MessagingRuntime ownership"
```

- [ ] **Step 9: Request review and verify clean state**

Invoke `superpowers:requesting-code-review`, address technically valid findings
with RED -> GREEN, then invoke `superpowers:verification-before-completion`.

```bash
git status --short
git log --oneline --decorate origin/main..HEAD
```

Expected: empty status and a reviewable sequence of focused commits.

## Completion Evidence Checklist

Before opening or updating the implementation PR, attach:

- [ ] merged prerequisite commit/PR references;
- [ ] baseline and final focused test counts;
- [ ] metadata parity matrix results;
- [ ] stable DLQ/accessor identity results;
- [ ] full-versus-fast path allowlist;
- [ ] reliable retry characterization and explicit resend-gap note;
- [ ] architecture fitness results;
- [ ] ASan and TSAN commands/results;
- [ ] full build/test count;
- [ ] final owner/outlives/start-stop diagram or inventory link; and
- [ ] confirmation that no Phase 4/5 implementation entered the branch.

## Final Acceptance Checklist

- [ ] One `MessagingRuntime` owns all seven planned messaging components.
- [ ] No delivery component stores/captures `ActorSystem` or `Impl`.
- [ ] No production late dependency setter remains.
- [ ] Ordinary local and decoded ordinary-remote ingress converge on the full
      delivery pipeline.
- [ ] Every fast delivery has an approved explicit reason.
- [ ] Facade messaging methods are compatibility forwards/adapters.
- [ ] DLQ and public messaging-owned object identities remain stable.
- [ ] Scheduler expiry uses the messaging-owned DLQ and outlives it correctly.
- [ ] Reliable ACK/NACK and retry interaction are encapsulated without changing
      wire/retry semantics or merging trackers.
- [ ] Metadata, result, DLQ, metric, pressure, ACK, and tracker parity passes.
- [ ] Mailbox concurrency and shutdown order pass stress/ASan/TSAN.
- [ ] Architecture checks prevent responsibility from returning to
      `ActorSystem`.
- [ ] Frame/stream routing and network lifecycle remain assigned to Phase 4/5.
