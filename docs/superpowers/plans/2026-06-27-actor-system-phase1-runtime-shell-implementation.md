# ActorSystem Phase 1 Runtime Shell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:executing-plans` to execute this plan task-by-task, invoke the
> repository `.claude/skills/tddflow-development/` skill before production
> edits, and use `superpowers:verification-before-completion` before commits or
> completion claims. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `ActorSystem` a source-compatible facade whose only owned runtime
state is `std::unique_ptr<Impl>`, while preserving current construction,
startup, actor spawn, delivery, callback, shutdown, and destruction behavior.

**Architecture:** Introduce a private `ActorSystem::Impl` in `src/runtime/` and
migrate state into named ownership groups: core, actor services, messaging,
network, operations, and cluster. Keep algorithms and policy behavior-neutral.
Use a private non-template adoption bridge for public `spawn<T>()`; do not
introduce the Phase 2 `ActorSpawner` or `SpawnSpec` yet. Guard each ownership
move with a failing architecture fitness test and focused regression tests.

**Tech Stack:** C++20, CMake, Ninja, GoogleTest 1.14, CTest script tests, ASan,
TSAN, existing HPActor `result<T>`, no RTTI, no exception-based control flow.

## Global Constraints

- Do not start until Phase 0 is merged and all prerequisites in
  `docs/superpowers/specs/2026-06-27-actor-system-phase1-runtime-shell-design.md`
  are satisfied.
- Execute in
  `.claude/worktrees/actor-system-runtime-shell/` on branch
  `refactor/actor-system-runtime-shell`, created from the branch containing
  merged Phase 0 plus the approved Phase 1 documents.
- Before every write, verify `pwd` ends in
  `.claude/worktrees/actor-system-runtime-shell` and
  `git branch --show-current` prints
  `refactor/actor-system-runtime-shell`.
- Follow RED -> GREEN -> REFACTOR for every production refactor slice. The RED
  signal for ownership-only tasks is an architecture fitness test that finds
  the still-unmoved state; behavior tests protect semantic parity.
- Keep all public `ActorSystem` signatures, constness, `noexcept`, default
  arguments, pointer stability, and enablement behavior source-compatible.
- Preserve current construction and explicit stop/destruction order. Moving a
  field is not permission to reorder it.
- Preserve every current atomic type and memory order, mutex scope, mailbox
  MPSC rule, scheduler ready-gate transition, and actor thread-confinement
  contract.
- Do not introduce `ActorSpawner`, `SpawnSpec`, `RuntimeBuilder`, immutable
  configuration, final runtime component APIs, a DI container, a service
  locator, mixins, `dynamic_cast`, `typeid`, exceptions, or public
  implementation types.
- Do not change protobuf schemas, wire formats, `TypeTag` assignments, actor
  lifecycle order, mailbox admission, retry/DLQ semantics, or shutdown phases.
- Do not add a broad mutex around `Impl`.
- Keep `src/runtime/actor_system_impl.hpp` private to `hpactor_lib`; never add it
  under `include/` or to an installed interface.
- Use only worktree-local `build/`, `build-asan/`, and `build-tsan/` directories.
- Stop immediately on a baseline failure or a semantic regression that cannot
  be explained by the current task. Do not weaken a test to make the refactor
  pass.

## Design References

- `docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`
- `docs/superpowers/specs/2026-06-27-actor-system-phase1-runtime-shell-design.md`
- `docs/superpowers/plans/2026-06-27-actor-system-phase0-correctness-stabilization.md`
- `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- `docs/architecture/production/production-reliability-plane.md`

## Expected File Set

**Create:**

- `src/runtime/CMakeLists.txt`
- `src/runtime/actor_system_impl.hpp`
- `src/runtime/actor_system_impl.cpp`
- `tests/architecture/CMakeLists.txt`
- `tests/architecture/assert_file_excludes.cmake`
- `tests/architecture/test_actor_system_public_header.cpp`
- `tests/integration/actor/test_actor_system_lifecycle.cpp`
- `docs/architecture/actor/actor-system-phase1-lifetime-inventory.md`

**Modify:**

- `src/CMakeLists.txt`
- `src/actor/actor_system.cpp`
- `include/hpactor/actor/actor_system.hpp`
- `tests/CMakeLists.txt`
- `tests/integration/actor/CMakeLists.txt`
- `tests/integration/actor/test_actor_system.cpp`
- public-header dependencies identified by the compiler after inline bodies
  move out of line
- `CLAUDE_MEMORY.md` after verification

The exact Phase 0 stream-owner and registry-view filenames are discovered in
Task 0 and substituted where this plan uses their conceptual names.

---

## Task 0: Establish the Phase 0 prerequisite and baseline

**Purpose:** Prove the implementation starts from the stabilized ownership
model and capture the baseline before any PImpl change.

- [ ] **Step 1: Create the required implementation worktree**

Run from the main checkout after Phase 0 and Phase 1 docs are available in the
same base commit:

```bash
git worktree add -b refactor/actor-system-runtime-shell \
  .claude/worktrees/actor-system-runtime-shell <phase-0-merged-base>
cd .claude/worktrees/actor-system-runtime-shell
pwd
git branch --show-current
git status --short
```

Expected: the path and branch match the Global Constraints, and status is
clean.

- [ ] **Step 2: Read the mandatory repository and concurrency guidance**

```bash
sed -n '1,240p' AGENTS.md
sed -n '1,240p' CLAUDE.md
sed -n '1,240p' CLAUDE_MEMORY.md
sed -n '1,240p' HPACTOR_PROJECT_OUTLINE.md
sed -n '1,280p' \
  docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md
```

Expected: the executing agent records any newer rule that supersedes a command
in this plan.

- [ ] **Step 3: Verify Phase 0 exit criteria in code**

Inspect the current code and tests using the repository knowledge graph first,
then targeted `rg` only for literal field/config names. Confirm:

- only `ActorDirectory` stores actor names;
- the delivery pipeline's DLQ address has stable lifetime;
- configured/template spawn parity tests exist;
- stream mappings have one synchronized owner;
- focused startup/shutdown characterization exists.

Run:

```bash
git log --oneline --decorate -20
rg -n "class StreamRegistry|stream_registry_|register_name|dead_letters_" \
  include src tests
```

Expected: all prerequisites are visible. If not, stop; finish Phase 0 rather
than folding fixes into this refactor.

- [ ] **Step 4: Configure and build the baseline**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build hpactor_lib test_unit_actor test_integration_actor \
  test_integration_config test_integration_mailbox test_integration_net
```

Expected: all targets build.

- [ ] **Step 5: Run the focused baseline tests**

Use the actual post-Phase 0 test names discovered in Step 3:

```bash
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorDirectoryTest.*:StreamRegistryTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemTest.*:ShutdownCoordinatorTest.*:DeliverySemanticsTest.*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.*'
./build/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='DeadLetterQueueTest.*'
```

Expected: all selected tests pass. Save the commands and counts in the PR
description or implementation notes.

- [ ] **Step 6: Inventory fields, callbacks, and raw pointers**

Create
`docs/architecture/actor/actor-system-phase1-lifetime-inventory.md` with three
tables:

1. every `ActorSystem` private field after Phase 0, selected state group, and
   declaration/destruction dependency;
2. every constructor-installed callback/thread, capture target, callback
   owner, and quiescence event;
3. every injected raw pointer/reference, owning field, consumer, and guarantee
   that the owner outlives the consumer.

At minimum include scheduler, directory, mailboxes, delivery pipeline, DLQ,
metrics ring, logger, transport, event loop, discovery callback, timers,
network thread, RPC, ask/passivation managers, shutdown coordinator, and
cluster deleters.

Run a completeness check:

```bash
rg -n "\[this\]|\.get\(\)|set_.*\(|std::thread|run_every|on_member_change" \
  src/actor/actor_system.cpp
```

Expected: every relevant match is represented in the inventory or explicitly
marked as non-owning-free/value-only.

- [ ] **Step 7: Commit the baseline inventory**

```bash
git add docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "docs: inventory ActorSystem runtime lifetimes"
```

---

## Task 1: Add architecture fitness infrastructure and empty `Impl`

**Files:**

- Create: `tests/architecture/assert_file_excludes.cmake`
- Create: `tests/architecture/CMakeLists.txt`
- Create: `tests/architecture/test_actor_system_public_header.cpp`
- Create: `src/runtime/CMakeLists.txt`
- Create: `src/runtime/actor_system_impl.hpp`
- Create: `src/runtime/actor_system_impl.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

**Invariant:** this task adds the opaque boundary only. Existing state and
behavior remain in `ActorSystem` until guarded group moves begin.

- [ ] **Step 1: Add the public-header compile test first**

Create `tests/architecture/test_actor_system_public_header.cpp` containing a
minimal consumer that includes only `<hpactor/actor/actor_system.hpp>`, checks
that `ActorSystem` is non-copyable, and compiles representative construction,
spawn declaration, const accessors, and shutdown expressions without including
private headers.

Add a `test_actor_system_public_header` executable in
`tests/architecture/CMakeLists.txt`, link it to `hpactor`, and add
`add_subdirectory(architecture)` to `tests/CMakeLists.txt`.

- [ ] **Step 2: Run the initial compile characterization**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build test_actor_system_public_header
```

Expected: it passes before PImpl. This is characterization, not the RED signal.

- [ ] **Step 3: Add a failing opaque-boundary assertion**

Add this assertion to the architecture test:

```cpp
static_assert(sizeof(hpactor::ActorSystem) <= 2 * sizeof(void*),
              "ActorSystem must be an opaque runtime facade");
```

Guard it with the CMake option
`HPACTOR_ACTOR_SYSTEM_FINAL_PIMPL_CHECK`, defaulting to `OFF`. Record RED by
enabling the option:

```bash
cmake -S . -B build -GNinja \
  -DHPACTOR_ACTOR_SYSTEM_FINAL_PIMPL_CHECK=ON
ninja -C build test_actor_system_public_header
```

Expected RED: compilation fails because the current facade owns the complete
runtime graph. Reconfigure with the option `OFF` after recording the failure so
intermediate commits remain green. Task 7 enables it permanently for the final
verification configuration.

```bash
cmake -S . -B build -GNinja \
  -DHPACTOR_ACTOR_SYSTEM_FINAL_PIMPL_CHECK=OFF
```

- [ ] **Step 4: Add the reusable field-exclusion script**

Implement `assert_file_excludes.cmake` to require `INPUT_FILE` and a CMake list
`FORBIDDEN`, read the file, and fail with the first forbidden token found. Add
one smoke CTest that checks a token known not to exist.

Do not parse C++; the test is an architectural tripwire with deliberately
specific private field tokens.

Run:

```bash
ctest --test-dir build -R ActorSystemArchitectureScript --output-on-failure
```

Expected: pass.

- [ ] **Step 5: Introduce an empty private `Impl`**

In the public header:

```cpp
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    // Existing fields temporarily remain below this line.
```

Define an initially empty, non-copyable `ActorSystem::Impl` in
`src/runtime/actor_system_impl.hpp`, and construct it first in
`ActorSystem::ActorSystem`. Keep `ActorSystem::~ActorSystem()` out of line.

Wire `src/runtime` into `hpactor_lib` through `src/CMakeLists.txt` and
`src/runtime/CMakeLists.txt`.

- [ ] **Step 6: Build and run the focused parity suite**

```bash
ninja -C build hpactor_lib test_actor_system_public_header \
  test_integration_actor
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemTest.*:ActorIntegrationFinalTest.*'
```

Expected GREEN: build and focused tests pass; `Impl` owns no behavioral object
yet.

- [ ] **Step 7: Commit the opaque seam**

```bash
git add src/CMakeLists.txt src/runtime include/hpactor/actor/actor_system.hpp \
  src/actor/actor_system.cpp tests/CMakeLists.txt tests/architecture
git commit -m "refactor: introduce ActorSystem implementation seam"
```

---

## Task 2: Move messaging and stream ownership into named state

**Files:**

- Modify: `src/runtime/actor_system_impl.hpp`
- Modify: `src/runtime/actor_system_impl.cpp`
- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/architecture/CMakeLists.txt`
- Modify: relevant Phase 0 stream and delivery tests

**Produces:** `MessagingRuntimeState` and ownership of the complete Phase 0
stream registry (as its own member or migration state).

**Must preserve:** stable DLQ identity, pipeline callbacks, delivery tracker
identity, stream synchronization, backpressure behavior, and metrics injection.

- [ ] **Step 1: Add the failing messaging-field exclusion test**

Add CTest `ActorSystemFacadeExcludesMessagingState`, passing these post-Phase 0
tokens, adjusted only if Phase 0 renamed their single owners:

```text
local_delivery_engine_
delivery_pipeline_
backpressure_coordinator_
dead_letters_
outbound_tracker_
reliable_tracker_
dedup_cache_
stream_registry_
```

If Phase 0 retained synchronized maps rather than a `StreamRegistry` type, use
the exact sender/receiver/counter owner tokens instead.

Run:

```bash
cmake -S . -B build -GNinja
ctest --test-dir build -R ActorSystemFacadeExcludesMessagingState \
  --output-on-failure
```

Expected RED: the first messaging field remains in the public header.

- [ ] **Step 2: Define the state group in safe declaration order**

Add `MessagingRuntimeState` to the private implementation. Start from the
Phase 0 lifetime inventory rather than alphabetizing fields. A representative
shape is:

```cpp
struct MessagingRuntimeState final {
    std::unique_ptr<mailbox::DeadLetterQueue> dead_letters;
    std::unique_ptr<msg::OutboundDeliveryTracker> outbound_tracker;
    std::unique_ptr<mailbox::OutboundTracker> reliable_tracker;
    std::unique_ptr<adt::DedupCache> dedup_cache;
    std::unique_ptr<LocalDeliveryEngine> local_delivery_engine;
    std::unique_ptr<BackpressureCoordinator> backpressure;
    std::unique_ptr<mailbox::DeliveryPipeline> delivery_pipeline;
};
```

Use actual dependencies to determine order. Do not change pointer types or
construct replacement objects.

- [ ] **Step 3: Move one owner at a time and update references mechanically**

For each field:

1. move its declaration into the group;
2. update constructor initialization/assignment to `impl_->messaging...`;
3. update accessors and method uses;
4. build `hpactor_lib`;
5. run the narrow delivery/DLQ test that covers the field;
6. confirm pointer identity/lifetime table remains true.

Use compiler errors to find references; do not add facade getters solely to
make the move easier.

- [ ] **Step 4: Move the complete stream owner atomically**

Move the Phase 0 synchronized stream registry as one object. Do not split its
mutex from maps/counter. Run its focused concurrency and routing tests.

```bash
ninja -C build hpactor_lib test_unit_actor test_integration_actor \
  test_integration_mailbox
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='StreamRegistryTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='DeliverySemanticsTest.*:ActorSystemBackpressureTest.*:*Stream*'
./build/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='DeadLetterQueueTest.*:*Reliable*:*Dedup*'
```

Expected GREEN: behavior tests pass.

- [ ] **Step 5: Run the architecture test**

```bash
ctest --test-dir build -R ActorSystemFacadeExcludesMessagingState \
  --output-on-failure
```

Expected GREEN: no messaging/stream owner token remains in the public facade.

- [ ] **Step 6: Refactor naming only after green**

Normalize group member names without trailing underscores if that is the
chosen internal convention. Do not move delivery policy methods yet. Update
the lifetime inventory with final names and verify all raw pointers still refer
to the same owned objects.

- [ ] **Step 7: Commit**

```bash
git add src/runtime include/hpactor/actor/actor_system.hpp \
  src/actor/actor_system.cpp tests/architecture \
  docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "refactor: move ActorSystem messaging state behind pimpl"
```

---

## Task 3: Move network ownership and audit callback lifetimes

**Files:** same core files as Task 2, plus network-focused tests and
`tests/integration/actor/test_actor_system_lifecycle.cpp`.

**Produces:** `NetworkRuntimeState`.

**Must preserve:** network-disabled null accessors, discovery selection,
transport/listen timing, timer intervals, network thread loop, handler routing,
and stop/join order.

- [ ] **Step 1: Add network lifecycle characterization before moving state**

Create `test_actor_system_lifecycle.cpp` and add it to
`test_integration_actor`. Cover deterministic scenarios:

- construction/destruction with networking disabled;
- `event_loop()`, `transport()`, and `registrar()` nullability;
- explicit `shutdown()` followed by destruction;
- repeated `shutdown()` preserving the documented result/phase;
- networking enabled with `tcp_port = 0`, avoiding external port timing.

Use scheduler threads `0` where possible and condition-based synchronization;
do not add sleeps.

Run:

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemLifecycleTest.*'
```

Expected: tests pass before the move and characterize current behavior.

- [ ] **Step 2: Add the failing network-field exclusion test**

Add `ActorSystemFacadeExcludesNetworkState` for the actual tokens corresponding
to:

```text
transport_
backpressure_signal_wire_sink_for_test_
registrar_
discovery_
location_cache_
cache_purge_timer_
retry_timer_
network_loop_
network_thread_
rpc_channel_
http_client_
```

Run it and confirm RED for a field still in `actor_system.hpp`.

- [ ] **Step 3: Define and populate `NetworkRuntimeState`**

Move the owners without changing concrete types. Keep the event loop and
transport callbacks installed in the same constructor phase. Any call back to
the facade uses `impl_->facade`, never a captured constructor parameter.

Audit these callbacks explicitly against the lifetime inventory:

- `IServiceDiscovery::on_member_change`;
- cache purge timer;
- outbound retry timer;
- transport RPC handler;
- transport actor-message handler;
- network thread lambda;
- test backpressure wire sink.

For each callback, document capture, owner, and quiescence event.

- [ ] **Step 4: Preserve stop-before-destroy order**

Keep the explicit sequence:

```text
running false -> event loop stop -> network thread join
-> transport stop listening -> discovery stop -> resource destruction
```

Do not rely solely on `NetworkRuntimeState` member destruction. Do not detach
the thread.

- [ ] **Step 5: Build and run network plus lifecycle tests**

```bash
ninja -C build hpactor_lib test_integration_actor test_integration_net \
  test_integration_rpc
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemLifecycleTest.*:ActorSystemTest.*:*Remote*:*Backpressure*'
ctest --test-dir build -R 'Network|Transport|Rpc' --output-on-failure
```

If `test_integration_rpc` is not the generated target name, use the target
listed by `ninja -C build -t targets | rg 'rpc'`.

Expected GREEN: tests pass and shutdown does not hang.

- [ ] **Step 6: Run the architecture check and commit**

```bash
ctest --test-dir build -R ActorSystemFacadeExcludesNetworkState \
  --output-on-failure
git add src/runtime include/hpactor/actor/actor_system.hpp \
  src/actor/actor_system.cpp tests/architecture tests/integration/actor \
  docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "refactor: move ActorSystem network state behind pimpl"
```

---

## Task 4: Move operations and cluster ownership

**Produces:** `OperationsRuntimeState` and `ClusterRuntimeState`.

**Must preserve:** telemetry object identity, enablement defaults, metric/log
injection, tracing application, fault-controller installation, and type-erased
cluster deleters.

- [ ] **Step 1: Add the failing operations/cluster exclusion test**

Add `ActorSystemFacadeExcludesOperationsState` for the actual fields
corresponding to:

```text
metrics_config_
metrics_ring_buffer_
metrics_actor_
logging_config_
log_manager_
logger_
tracing_config_
trace_manager_
fault_controller_
cluster_enabled_
cluster_failure_model_
singleton_manager_
route_invalidation_
```

Run and confirm RED.

- [ ] **Step 2: Add identity/accessor characterization**

Extend existing metrics/log/tracing tests or `ActorSystemLifecycleTest` to
check, under their existing enablement configuration:

- repeated accessor calls return the same pointer;
- disabled accessors preserve current null/non-null behavior;
- spawned actor/mailbox telemetry pointers still target the system-owned
  buffer/logger;
- shutdown does not destroy telemetry while scheduler workers may still emit.

Run these tests before moving fields; expected pass.

- [ ] **Step 3: Move operations state in dependency order**

Move config values with their managers, but do not reparse or reapply config.
Keep raw aliases (`metrics_actor`, `logger`) explicitly non-owning and declared
after/beside their owners with comments. Preserve the stop/flush order from the
lifetime inventory.

- [ ] **Step 4: Move cluster state without changing the bridge**

Move the enablement flag and the three `unique_ptr<void, cleanup_fn>` owners as
one group. Preserve each current deleter exactly. Do not introduce a typed
cluster interface in this phase.

- [ ] **Step 5: Verify**

```bash
ninja -C build hpactor_lib test_integration_actor test_integration_metrics \
  test_integration_log test_integration_tracing test_unit_cluster
ctest --test-dir build \
  -R 'ActorSystemFacadeExcludesOperationsState|Metrics|Logging|Tracing|Cluster' \
  --output-on-failure
```

Use actual target names if CMake names differ. Expected GREEN.

- [ ] **Step 6: Commit**

```bash
git add src/runtime include/hpactor/actor/actor_system.hpp \
  src/actor/actor_system.cpp tests docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "refactor: move ActorSystem operations state behind pimpl"
```

---

## Task 5: Move actor-service state and introduce the template spawn bridge

**Produces:** `ActorServiceState` and
`ActorSystem::adopt_preconstructed_actor()`.

**Must preserve:** Phase 0 name ownership, directory insertion, actor address,
mailbox/context wiring, dispatch-policy registration, activation/lifecycle
order, log/metric emission, and return handle.

- [ ] **Step 1: Add spawn-bridge behavioral characterization**

Extend `tests/integration/actor/test_actor_system.cpp` with focused fixtures for
each `spawn<T>()` post-construction contract:

- address uses the configured endpoint and a nonzero allocated id;
- type name uses `T::kActorTypeName` when present and `"unknown"` otherwise;
- context, mailbox, scheduler, logger, and metrics wiring match current
  enablement;
- directory entry is complete before activation-observable work;
- cooperative, dedicated-thread, and dedicated-pool actors register exactly
  once with the scheduler;
- `on_activate()` precedes the lifecycle transition exactly as characterized
  in Phase 0;
- actor-spawn log/metric behavior is unchanged.

Where internal state is not publicly observable, use the existing test actors
and scheduler test driver rather than adding public hooks.

Run before production edits; expected pass.

- [ ] **Step 2: Add the failing actor-state exclusion test**

Add `ActorSystemFacadeExcludesActorState` for the actual post-Phase 0 tokens:

```text
registry_
actor_directory_
actor_types_
system_actor_
actor_type_registry_
ask_manager_
passivation_manager_
http_gateway_actor_
cli_actor_
receptionist_
```

Run and confirm RED.

- [ ] **Step 3: Introduce the non-template compatibility helper**

Declare privately:

```cpp
Actor adopt_preconstructed_actor(std::shared_ptr<AbstractActor> actor,
                                 std::string_view type_name);
```

Change only `spawn<T>()` to construct `T`, select the type-name string, and call
the helper. Move the existing remaining body line-for-line into the out-of-line
helper before refactoring names.

Do not route configured actors, spawn receiver, metrics actor special handling,
or other manually adopted system actors through this helper. That is Phase 2.

- [ ] **Step 4: Run spawn tests immediately**

```bash
ninja -C build hpactor_lib test_integration_actor test_integration_spawn
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemSpawnBridgeTest.*:ActorSystemTest.*:*DispatchPolicy*:*Lifecycle*'
ctest --test-dir build -R Spawn --output-on-failure
```

Expected GREEN: all characterized steps remain in the same order.

- [ ] **Step 5: Define `ActorServiceState` and move its owners**

Move the directory and its Phase 0 registry compatibility view together so the
view never points at a moved or temporary directory. Then move type maps,
system actor, actor-type registry, ask/passivation services, and actor handles.

If any state constructor needs `ActorSystem&`, inject the stable
`impl_->facade`; do not make the group own the facade.

- [ ] **Step 6: Verify actor/config/system behavior**

```bash
ninja -C build hpactor_lib test_unit_actor test_integration_actor \
  test_integration_config test_integration_spawn test_system
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorDirectoryTest.*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.*'
ctest --test-dir build \
  -R 'ActorSystemFacadeExcludesActorState|Actor|Spawn|Bootstrap' \
  --output-on-failure
```

Expected GREEN.

- [ ] **Step 7: Commit**

```bash
git add src/runtime include/hpactor/actor/actor_system.hpp \
  src/actor/actor_system.cpp tests docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "refactor: move ActorSystem actor state behind pimpl"
```

---

## Task 6: Move core/scheduler state and startup/shutdown wiring into `Impl`

**Produces:** `CoreRuntimeState`, thin facade constructor/destructor, and the
complete named ownership shell.

**Must preserve:** config copy, endpoint, start time, atomics/memory orders,
scheduler construction/start, process initialization timing, proto registry,
readiness, shutdown phases, and explicit resource quiescence.

- [ ] **Step 1: Add the failing core-state exclusion test**

Add `ActorSystemFacadeExcludesCoreState` for the actual tokens corresponding to:

```text
Config config_;
EndPoint endpoint_;
Clock clock_;
start_time_
running_
shutdown_phase_
is_ready_
scheduler_
proto_registry_
```

Run and confirm RED.

- [ ] **Step 2: Move the core values without moving startup policy yet**

Define `CoreRuntimeState` in dependency order and move the fields. Keep the
facade constructor body temporarily operating on `impl_->core`, other groups,
and existing methods. Re-run lifecycle, config, scheduler, and proto tests.

- [ ] **Step 3: Move constructor wiring into `Impl`**

Implement:

```cpp
ActorSystem::Impl::Impl(ActorSystem& facade, const Config& config);
```

Move the existing constructor body in original sequence. Use the lifetime
inventory as a checklist. Inside asynchronous callbacks, `[this]` means
`Impl*`; calls requiring compatibility methods use `facade` stored as a member.
Never capture the constructor parameter by reference.

After the move, the facade constructor is only:

```cpp
ActorSystem::ActorSystem(const Config& config)
    : impl_(std::make_unique<Impl>(*this, config)) {}
```

- [ ] **Step 4: Move explicit teardown into `Impl::~Impl()`**

Copy the current facade destructor's stop sequence before simplifying it.
Confirm scheduler producers cannot use metrics/logger/mailbox state after those
owners begin destruction. Confirm all callback owners are stopped before
captured state is destroyed.

Define `ActorSystem::~ActorSystem()` out of line as default only after the
`Impl` destructor contains the complete explicit quiescence sequence.

- [ ] **Step 5: Run focused lifecycle and subsystem tests**

```bash
ninja -C build hpactor_lib test_integration_actor test_integration_sched \
  test_integration_net test_integration_config test_integration_metrics \
  test_integration_log test_integration_tracing
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemLifecycleTest.*:ActorSystemTest.*:ShutdownCoordinatorTest.*'
ctest --test-dir build \
  -R 'ActorSystemFacadeExcludesCoreState|Scheduler|Network|Config|Metrics|Logging|Tracing' \
  --output-on-failure
```

Expected GREEN, with no hang or pointer identity change.

- [ ] **Step 6: Check every callback and raw pointer again**

Run:

```bash
rg -n "\[this\]|\[&facade\]|\.get\(\)|std::thread|run_every|on_member_change" \
  src/runtime/actor_system_impl.cpp src/actor/actor_system.cpp
```

Expected:

- no `[&facade]` asynchronous capture;
- every match is checked in the lifetime inventory;
- no facade runtime field remains as a second owner.

- [ ] **Step 7: Commit**

```bash
git add src/runtime include/hpactor/actor/actor_system.hpp \
  src/actor/actor_system.cpp tests docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "refactor: move ActorSystem lifecycle behind pimpl"
```

---

## Task 7: Move inline accessors out of line and enforce pointer-only facade

**Must preserve:** all public declarations, pointer stability, nullability,
constness, `noexcept`, and template source compatibility.

- [ ] **Step 1: Re-enable the final size assertion and record RED**

Enable `HPACTOR_ACTOR_SYSTEM_FINAL_PIMPL_CHECK` for
`test_actor_system_public_header` and run:

```bash
cmake -S . -B build -GNinja \
  -DHPACTOR_ACTOR_SYSTEM_FINAL_PIMPL_CHECK=ON
ninja -C build test_actor_system_public_header
```

Expected RED if any remaining facade state or accidental padding exceeds the
documented two-pointer allowance. If it unexpectedly passes, continue with
the field-exclusion checks; size is a guard, not proof of ownership quality.

- [ ] **Step 2: Convert remaining private-state inline accessors**

For every public inline accessor that dereferences a moved owner:

1. leave the exact declaration in `actor_system.hpp`;
2. move its body to `src/actor/actor_system.cpp`;
3. preserve const overload, `noexcept`, and null behavior;
4. build the public-header test;
5. run the narrow subsystem test.

Keep `spawn<T>()` inline but ensure it touches only public/template-visible
types, `*this`, and `adopt_preconstructed_actor()`.

- [ ] **Step 3: Remove all remaining runtime fields from the facade**

The private runtime state must now be exactly:

```cpp
class Impl;
std::unique_ptr<Impl> impl_;
```

plus private method/type declarations that own no runtime state. Remove
temporary migration comments and disabled checks.

- [ ] **Step 4: Trim implementation-only public includes**

Remove one include at a time, build the standalone public-header test after
each removal, and retain includes required by `Config`, public return types,
base classes, or `spawn<T>()`.

Do not forward-declare a type where a public inline/value declaration requires
completeness. Do not redesign `Config` here.

- [ ] **Step 5: Run final architecture checks**

```bash
ninja -C build test_actor_system_public_header hpactor_lib
ctest --test-dir build -R '^ActorSystemFacade|ActorSystemArchitecture' \
  --output-on-failure
```

Expected GREEN:

- size assertion passes;
- all state-group exclusion tests pass;
- public-header consumer compiles;
- private implementation header is not required by the consumer.

- [ ] **Step 6: Run representative source-compatibility builds**

```bash
ninja -C build examples apps
```

If aggregate targets differ, list targets and build all enabled example/app
targets that use `ActorSystem`. Expected: no call-site edits outside internals
are required.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/actor/actor_system.hpp src/actor/actor_system.cpp \
  src/runtime tests/architecture
git commit -m "refactor: make ActorSystem a pointer-only facade"
```

---

## Task 8: Sanitizer verification and lifecycle hardening

**Purpose:** Validate that moving ownership did not create use-after-free,
callback races, leaked threads, or shutdown ordering regressions.

- [ ] **Step 1: Configure focused ASan build**

```bash
cmake -S . -B build-asan -GNinja -DENABLE_ASAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-asan hpactor_lib test_integration_actor \
  test_integration_mailbox test_integration_net
```

- [ ] **Step 2: Run ASan lifetime scenarios**

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ./build-asan/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemLifecycleTest.*:ActorSystemTest.*:*Delivery*:*Shutdown*'
ctest --test-dir build-asan -R 'DeadLetter|Transport|Network' \
  --output-on-failure
```

Expected: no use-after-free, double free, leak caused by Phase 1, or leaked
joinable thread.

- [ ] **Step 3: Configure focused TSAN build**

```bash
cmake -S . -B build-tsan -GNinja -DENABLE_TSAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-tsan hpactor_lib test_unit_actor test_integration_actor \
  test_integration_net
```

- [ ] **Step 4: Run targeted TSAN scenarios**

```bash
TSAN_OPTIONS=halt_on_error=1 \
  ./build-tsan/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemLifecycleTest.*:*Stream*:*Remote*:*Shutdown*'
ctest --test-dir build-tsan -R 'StreamRegistry|Network' \
  --output-on-failure
```

Expected: no new race. If a pre-existing race was captured in Task 0, report it
separately; do not conceal it with a broad `Impl` lock.

- [ ] **Step 5: Add a regression test for any discovered Phase 1 defect**

If a sanitizer finds a defect caused by the move, follow a fresh RED -> GREEN
cycle with the narrowest deterministic regression. Update the lifetime
inventory with the missed dependency.

- [ ] **Step 6: Commit test-only hardening if needed**

```bash
git add tests docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "test: harden ActorSystem pimpl lifecycle coverage"
```

Skip this commit when no files changed.

---

## Task 9: Full verification, documentation sync, and review handoff

- [ ] **Step 1: Reconfigure and run the normal full build**

This phase changes a broad public header and composition root, so full
verification is required:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DHPACTOR_ACTOR_SYSTEM_FINAL_PIMPL_CHECK=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

Expected: full build and suite pass. Record exact test count and duration.

- [ ] **Step 2: Run static architecture inspection**

```bash
rg -n "^  private:|std::unique_ptr<Impl> impl_|class Impl" \
  include/hpactor/actor/actor_system.hpp
rg -n "actor_system_impl.hpp" include src tests
rg -n "dynamic_cast|typeid|throw |catch \(" \
  src/runtime include/hpactor/actor/actor_system.hpp src/actor/actor_system.cpp
```

Expected:

- public facade has one runtime owner;
- only internal source files/tests intentionally reference the private header;
- no forbidden RTTI/exception mechanism was introduced.

- [ ] **Step 3: Reconcile the lifetime inventory with final code**

Review every table row against final declaration and explicit stop order.
Mark the document as the verified Phase 1 state and add the commit hash after
the final implementation commit is known, or reference the PR if the project
avoids mutable commit hashes in docs.

- [ ] **Step 4: Update project memory**

Update `CLAUDE_MEMORY.md` with:

- Phase 1 status and branch/PR;
- new private runtime file locations;
- facade invariant;
- spawn compatibility helper status;
- exact test/sanitizer evidence;
- explicit statement that final component extraction begins in Phase 2 and is
  not implemented by Phase 1.

- [ ] **Step 5: Review the diff for scope creep**

```bash
git status --short
git diff --stat <phase-0-merged-base>...HEAD
git diff <phase-0-merged-base>...HEAD -- \
  include/hpactor/actor/actor_system.hpp src/actor/actor_system.cpp src/runtime
```

Reject or split any change that modifies behavior, wire/config contracts, or
introduces a Phase 2 abstraction.

- [ ] **Step 6: Commit documentation sync**

```bash
git add CLAUDE_MEMORY.md \
  docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "docs: record ActorSystem runtime shell architecture"
```

- [ ] **Step 7: Request code review before integration**

Use `superpowers:requesting-code-review`. Ask reviewers to focus on:

1. declaration and explicit destruction order;
2. callback captures and quiescence;
3. raw pointer identity/lifetime;
4. template spawn parity;
5. public API/source compatibility;
6. whether `Impl` remained a named ownership shell rather than gaining policy.

- [ ] **Step 8: Push and open the implementation PR**

```bash
git push -u origin refactor/actor-system-runtime-shell
gh pr create \
  --base main \
  --head refactor/actor-system-runtime-shell \
  --title "refactor: introduce ActorSystem runtime ownership shell" \
  --body-file <prepared-pr-body>
```

The PR body must link issue #379, the umbrella design, the Phase 1 design,
Phase 0 implementation evidence, and exact normal/ASan/TSAN commands/results.

## Completion Checklist

- [ ] Phase 0 prerequisites were verified before editing production code.
- [ ] `ActorSystem` owns only `std::unique_ptr<Impl>` as runtime state.
- [ ] `Impl` contains named state groups with documented dependency order.
- [ ] Public signatures/defaults/constness/`noexcept` remain compatible.
- [ ] `spawn<T>()` uses only the temporary non-template adoption bridge.
- [ ] Configured/system/remote spawn paths were not unified prematurely.
- [ ] Startup, readiness, shutdown, and destruction behavior match baseline.
- [ ] Every callback and raw pointer has an owner and quiescence entry.
- [ ] No private implementation type leaked into public headers.
- [ ] Architecture checks, focused tests, full suite, ASan, and TSAN pass.
- [ ] No RTTI, exceptions, DI container, service locator, or broad `Impl` lock
  was added.
- [ ] `CLAUDE_MEMORY.md` clearly separates implemented Phase 1 state from the
  Phase 2 target.

## Explicit Stop Point

Stop after the Phase 1 PR is review-ready. Do not begin `ActorRuntime`,
`ActorSpawner`, `SpawnSpec`, unified adoption, scheduler capability ports, or
any other Phase 2 implementation on this branch. Those require the Phase 2
design and a new implementation plan/worktree.
