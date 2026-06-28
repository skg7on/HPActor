# ActorSystem Phase 2 ActorRuntime and Unified Spawning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Invoke the
> repository `.claude/skills/tddflow-development/` skill before production
> edits and `superpowers:verification-before-completion` before commits or
> completion claims. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Phase 1 actor-service storage with a cohesive `ActorRuntime`
and route template, configured, reserved-system, and remote-factory actors
through one transactional `ActorSpawner` without changing public actor APIs.

**Architecture:** `ActorRuntime` owns `ActorDirectory`, `ActorSpawner`, actor
type registration, passivation, and fixed system actor identities/handles.
`SpawnSpec` carries fully resolved synchronous adoption policy;
`ActorDirectory::publish()` atomically commits id plus optional name; and an
actor-local admission gate prevents execution before activation finishes.
Scheduler execution stores a concrete `ActorExecutionDependencies` bundle
instead of `ActorSystem&`.

**Tech Stack:** C++20, CMake, Ninja, GoogleTest 1.14, CTest architecture
scripts, HPActor `result<T>`, ASan, TSAN, existing protobuf contracts, no RTTI,
no exception-based control flow.

## Global Constraints

- Start only after the Phase 0 stabilization and Phase 1 runtime-shell PRs are
  merged into `origin/main` and their verification evidence is available.
- Execute in `.claude/worktrees/actor-system-actor-runtime/` on branch
  `refactor/actor-system-actor-runtime`, created from the updated
  `origin/main`. This follows `.claude/rules` category/description and worktree
  naming requirements.
- Before every write, verify `pwd` ends in
  `.claude/worktrees/actor-system-actor-runtime` and
  `git branch --show-current` prints
  `refactor/actor-system-actor-runtime`.
- Follow RED -> GREEN -> REFACTOR for every production change. Run and record
  each stated RED command before editing its production files.
- Preserve all existing public `ActorSystem`, actor factory,
  `ActorTypeRegistry`, and `HybridScheduler(ActorSystem&, ...)` signatures,
  defaults, constness, and `noexcept` guarantees.
- `ActorSpawner` is the only production code allowed to construct and publish
  `ActorDirectoryEntry` by the end of the phase.
- Never hold the directory mutex while invoking actor code, scheduler methods,
  mailbox delivery, telemetry, logging, fault injection, or user callbacks.
- Keep mailbox MPSC, actor-state CAS, ready-gate, lost-wakeup, reservation, and
  single-consumer rules unchanged except for the explicit pre-activation
  admission gate defined by the Phase 2 design.
- Use release when opening spawn admission and acquire when scheduler admission
  observes it.
- Keep the existing `ActorState` values and transitions unchanged; the spawn
  admission gate is separate.
- Do not introduce `dynamic_cast`, `typeid`, exception control flow,
  `std::function` on scheduler lookup paths, generic service lookup, a DI
  container, a broad runtime mutex, or public runtime-internal headers.
- Do not change protobuf schemas, `TypeTag` assignments, remote frame formats,
  established `spawn_errors` values, delivery semantics, DLQ ownership,
  backpressure, streams, network lifecycle, or configuration parsing.
- Treat activation and scheduler registration as infallible under their current
  `void` contracts. Roll back only reported/injected failures before actor code
  runs; do not invent compensating `on_exit()` behavior.
- Use the worktree's own `build/`, `build-asan/`, and `build-tsan/` directories.
- Stop at the explicit Phase 3 boundary. Do not create `MessagingRuntime` or
  move delivery policy on this branch.

## Design References

- `docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`
- `docs/superpowers/specs/2026-06-27-actor-system-phase1-runtime-shell-design.md`
- `docs/superpowers/specs/2026-06-27-actor-system-phase2-actor-runtime-design.md`
- `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
- `docs/superpowers/specs/2026-05-29-scheduler-decoupling-design.md`

## Expected File Structure

**Create:**

- `src/runtime/spawn_spec.hpp` — internal resolved adoption value contract.
- `src/runtime/actor_spawner.hpp` — private spawner interface/dependencies.
- `src/runtime/actor_spawner.cpp` — sole adoption/publication policy.
- `src/runtime/actor_runtime.hpp` — private actor component interface/owner.
- `src/runtime/actor_runtime.cpp` — actor lookup/registry facade implementation.
- `include/hpactor/sched/actor_execution_dependencies.hpp` — fixed concrete
  scheduler dependency bundle and compatibility adapter declaration.
- `tests/unit/actor/test_actor_spawner.cpp` — validation, rollback, telemetry.
- `tests/integration/actor/test_actor_spawn_unification.cpp` — all spawn-path
  parity and activation ordering.
- `tests/integration/sched/test_actor_execution_dependencies.cpp` — scheduler
  dependency and compatibility-constructor coverage.
- `tests/architecture/assert_search_allowlist.cmake` — enforce sole production
  ownership of selected source patterns.

**Modify:**

- `src/runtime/CMakeLists.txt`
- `src/runtime/actor_system_impl.hpp`
- `src/runtime/actor_system_impl.cpp`
- `include/hpactor/actor/actor_system.hpp`
- `src/actor/actor_system.cpp`
- `include/hpactor/actor/abstract_actor.hpp`
- `src/actor/abstract_actor.cpp`
- `include/hpactor/actor/local_actor.hpp`
- `include/hpactor/actor/actor_directory.hpp`
- `src/actor/actor_directory.cpp`
- `include/hpactor/actor/spawn.hpp`
- `include/hpactor/actor/actor_type_registry.hpp`
- `src/actor/actor_type_registry.cpp`
- `include/hpactor/sched/actor_ready_gate.hpp`
- `src/sched/actor_ready_gate.cpp`
- `include/hpactor/sched/actor_execution_engine.hpp`
- `src/sched/actor_execution_engine.cpp`
- `include/hpactor/sched/scheduler.hpp`
- `src/sched/scheduler.cpp`
- `tests/unit/actor/CMakeLists.txt`
- `tests/unit/actor/test_actor_directory.cpp`
- `tests/integration/actor/CMakeLists.txt`
- `tests/integration/config/test_bootstrap_engine.cpp`
- `tests/integration/spawn/test_actor_type_registry.cpp`
- `tests/integration/sched/CMakeLists.txt`
- `tests/architecture/CMakeLists.txt`
- `docs/architecture/actor/actor-system-phase1-lifetime-inventory.md`
- `CLAUDE_MEMORY.md`

Private runtime headers remain under `src/runtime`; add
`${CMAKE_SOURCE_DIR}/src` only as a `PRIVATE` include directory for the focused
unit target that tests them.

---

### Task 0: Create the implementation worktree and verify prerequisites

**Deliverable:** A clean Phase 2 worktree with Phase 0/1 contracts and a passing
focused baseline.

- [ ] **Step 1: Update remote state and create the required worktree**

Run from the main checkout:

```bash
git fetch origin
git worktree add -b refactor/actor-system-actor-runtime \
  .claude/worktrees/actor-system-actor-runtime origin/main
cd .claude/worktrees/actor-system-actor-runtime
pwd
git branch --show-current
git status --short
```

Expected: correct path/branch and empty status.

- [ ] **Step 2: Verify Phase 1 is actually present**

```bash
test -f src/runtime/actor_system_impl.hpp
test -f docs/superpowers/specs/2026-06-27-actor-system-phase1-runtime-shell-design.md
test -f docs/superpowers/specs/2026-06-27-actor-system-phase2-actor-runtime-design.md
rg -n "std::unique_ptr<Impl> impl_" include/hpactor/actor/actor_system.hpp
rg -n "ActorServiceState|MessagingRuntimeState|NetworkRuntimeState" \
  src/runtime/actor_system_impl.hpp
```

Expected: all files and Phase 1 ownership-shell markers exist. If not, stop;
do not retrofit Phase 2 onto pre-Phase-1 code.

- [ ] **Step 3: Read mandatory guidance**

```bash
sed -n '1,240p' AGENTS.md
sed -n '1,240p' CLAUDE.md
sed -n '1,240p' CLAUDE_MEMORY.md
sed -n '1,240p' HPACTOR_PROJECT_OUTLINE.md
sed -n '1,320p' \
  docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md
```

Expected: no newer repository rule conflicts with this plan. Newer rules take
precedence and must be recorded in the PR.

- [ ] **Step 4: Configure and build the focused baseline**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build hpactor_lib test_unit_actor test_integration_actor \
  test_integration_config test_integration_spawn test_integration_sched
```

Expected: all targets build.

- [ ] **Step 5: Run the focused baseline**

```bash
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorDirectoryTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemTest.*:ActorSystemLifecycleTest.*:*Spawn*:*Lifecycle*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.*'
./build/tests/integration/spawn/test_integration_spawn
./build/tests/integration/sched/test_integration_sched \
  --gtest_filter='*ReadyGate*:*ExecutionEngine*:*HybridScheduler*'
```

Expected: all selected tests pass. Record exact test counts. If a filter matches
zero tests, list the binary's tests and replace it with the exact existing suite
name before production edits.

- [ ] **Step 6: Refresh the lifetime/publication inventory**

Extend
`docs/architecture/actor/actor-system-phase1-lifetime-inventory.md` with:

- every production `ActorDirectoryEntry` construction site;
- every directory insert/name registration site;
- every spawn variant and its current step order;
- scheduler/ready-gate/execution-runner uses of `ActorSystem`;
- directory, scheduler, DLQ, metrics, logger, passivation, and actor-handle
  outlives relations.

Use codebase-memory graph discovery first, then literal search as a completeness
check:

```bash
rg -n "ActorDirectoryEntry|\.insert\(|register_name|spawn_configured|SpawnReceiver" \
  src include/hpactor/actor
rg -n "ActorSystem&|system_" src/sched include/hpactor/sched
```

Expected: every match is classified. Commit the inventory before structural
moves:

```bash
git add docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "docs: inventory ActorRuntime publication dependencies"
```

---

### Task 1: Add atomic directory publication

**Files:**

- Modify: `include/hpactor/actor/actor_directory.hpp`
- Modify: `src/actor/actor_directory.cpp`
- Modify: `tests/unit/actor/test_actor_directory.cpp`

**Interfaces:**

- Produces:
  `DirectoryPublishStatus ActorDirectory::publish(ActorDirectoryEntry,
  std::optional<std::string_view>)`.
- Preserves: `bool ActorDirectory::insert(ActorDirectoryEntry)` as a wrapper.
- Invariant: duplicate id or name publishes neither map entry.

- [ ] **Step 1: Write failing duplicate-name atomicity tests**

Add a fixture helper that creates an `EventBasedActor`, assigns the requested
address, and returns a complete entry using scheduler threads `0`. Add:

```cpp
TEST_F(ActorDirectoryTest, PublishCommitsEntryAndNameTogether) {
    auto entry = make_entry(ActorId{41});
    auto status = directory_.publish(std::move(entry), "worker-41");

    EXPECT_EQ(status, DirectoryPublishStatus::Published);
    ASSERT_TRUE(directory_.find(ActorId{41}).has_value());
    ASSERT_TRUE(directory_.resolve_name("worker-41").has_value());
    EXPECT_EQ(directory_.resolve_name("worker-41")->id, ActorId{41});
}

TEST_F(ActorDirectoryTest, DuplicateNamePublishesNoOrphanEntry) {
    ASSERT_EQ(directory_.publish(make_entry(ActorId{41}), "worker"),
              DirectoryPublishStatus::Published);

    EXPECT_EQ(directory_.publish(make_entry(ActorId{42}), "worker"),
              DirectoryPublishStatus::DuplicateName);
    EXPECT_FALSE(directory_.find(ActorId{42}).has_value());
    EXPECT_EQ(directory_.size(), 1U);
}

TEST_F(ActorDirectoryTest, DuplicateIdPublishesNoSecondName) {
    ASSERT_EQ(directory_.publish(make_entry(ActorId{41}), "first"),
              DirectoryPublishStatus::Published);

    EXPECT_EQ(directory_.publish(make_entry(ActorId{41}), "second"),
              DirectoryPublishStatus::DuplicateActorId);
    EXPECT_FALSE(directory_.resolve_name("second").has_value());
}
```

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_unit_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorDirectoryTest.PublishCommitsEntryAndNameTogether:ActorDirectoryTest.DuplicateNamePublishesNoOrphanEntry:ActorDirectoryTest.DuplicateIdPublishesNoSecondName'
```

Expected RED: compile fails because `DirectoryPublishStatus` and `publish()` do
not exist.

- [ ] **Step 3: Add the status and minimal transaction**

Declare:

```cpp
enum class DirectoryPublishStatus : uint8_t {
    Published,
    DuplicateActorId,
    DuplicateName,
};

DirectoryPublishStatus
publish(ActorDirectoryEntry entry,
        std::optional<std::string_view> name = std::nullopt);
```

Implement under one `mutex_` lock:

```cpp
DirectoryPublishStatus
ActorDirectory::publish(ActorDirectoryEntry entry,
                        std::optional<std::string_view> name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ActorId id = entry.actor.id();
    if (entries_.contains(id)) {
        return DirectoryPublishStatus::DuplicateActorId;
    }
    if (name.has_value() && names_.contains(std::string{*name})) {
        return DirectoryPublishStatus::DuplicateName;
    }

    const ActorAddress address = entry.actor.address();
    entries_.emplace(id, std::move(entry));
    if (name.has_value()) {
        names_.emplace(std::string{*name}, address);
    }
    return DirectoryPublishStatus::Published;
}
```

Implement `insert()` as
`publish(std::move(entry)) == DirectoryPublishStatus::Published`.

- [ ] **Step 4: Run GREEN**

```bash
ninja -C build test_unit_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorDirectoryTest.*'
```

Expected: all directory tests pass.

- [ ] **Step 5: Verify no external call occurs under the lock**

Inspect `publish()` and confirm all actor/address reads needed for insertion are
performed before or are simple value reads; no scheduler, actor callback,
logging, or telemetry call is present.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/actor/actor_directory.hpp \
  src/actor/actor_directory.cpp tests/unit/actor/test_actor_directory.cpp
git commit -m "refactor: add atomic actor directory publication"
```

---

### Task 2: Add safe context binding

**Files:**

- Modify: `include/hpactor/actor/abstract_actor.hpp`
- Modify: `src/actor/abstract_actor.cpp`
- Modify: `include/hpactor/actor/local_actor.hpp`
- Modify: `tests/integration/actor/test_local_actor.cpp`

**Interfaces:**

- Produces: `virtual bool AbstractActor::bind_context(ActorContext*) noexcept`
  and `virtual void AbstractActor::activate_after_spawn()`.
- Preserves: `LocalActor::set_context(ActorContext*)`.
- Invariant: unsupported actor modes reject binding without RTTI or unchecked
  casts.

- [ ] **Step 1: Write failing capability tests**

Add:

```cpp
TEST(LocalActorTest, BindContextUsesExistingContextStorage) {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system{config};
    TestLocalActor actor{nullptr, system};
    ActorContext context{Actor{}, &system};

    EXPECT_TRUE(actor.bind_context(&context));
    EXPECT_EQ(actor.context(), &context);
}

TEST(LocalActorTest, AbstractActorDefaultRejectsContextBinding) {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system{config};
    TestNonLocalActor actor{ActorId{}, ActorType{}, system};
    ActorContext context{Actor{}, &system};

    EXPECT_FALSE(actor.bind_context(&context));
}
```

`TestNonLocalActor` derives directly from `AbstractActor` and implements only
the required `receive()` method.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='LocalActorTest.BindContextUsesExistingContextStorage:LocalActorTest.AbstractActorDefaultRejectsContextBinding'
```

Expected RED: `bind_context` is undefined.

- [ ] **Step 3: Implement the capability**

In `AbstractActor`:

```cpp
virtual bool bind_context(ActorContext* context) noexcept;
virtual void activate_after_spawn();
```

Default implementation ignores the pointer and returns false. In `LocalActor`:

```cpp
bool bind_context(ActorContext* context) noexcept override {
    set_context(context);
    return true;
}

void activate_after_spawn() override {
    on_activate();
}
```

The default activation hook is a no-op; successful adoption requires context
binding first, so only a supported local actor reaches it. Do not add any type
query or cast.

- [ ] **Step 4: Run GREEN and regression**

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='LocalActorTest.*:ActorSystemTest.*'
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/actor/abstract_actor.hpp \
  src/actor/abstract_actor.cpp include/hpactor/actor/local_actor.hpp \
  tests/integration/actor/test_local_actor.cpp
git commit -m "refactor: add RTTI-free actor context binding"
```

---

### Task 3: Implement `SpawnSpec`, `ActorSpawner`, and activation admission

**Files:**

- Create: `src/runtime/spawn_spec.hpp`
- Create: `src/runtime/actor_spawner.hpp`
- Create: `src/runtime/actor_spawner.cpp`
- Create: `tests/unit/actor/test_actor_spawner.cpp`
- Create: `tests/integration/actor/test_actor_spawn_unification.cpp`
- Modify: `src/runtime/CMakeLists.txt`
- Modify: `include/hpactor/actor/abstract_actor.hpp`
- Modify: `include/hpactor/sched/actor_ready_gate.hpp`
- Modify: `src/sched/actor_ready_gate.cpp`
- Modify: `include/hpactor/actor/spawn.hpp`
- Modify: `include/hpactor/metrics/metrics_event.hpp`
- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`
- Modify: `tests/integration/actor/CMakeLists.txt`

**Interfaces:**

- Produces: `SpawnOrigin`, `SpawnSpec`, `ActorSpawner::Dependencies`, and
  `result<Actor> ActorSpawner::adopt(...) noexcept`.
- Consumes: `ActorDirectory::publish()` and `AbstractActor::bind_context()`.
- Preserves: public `ActorSystem::spawn<T>() -> Actor`.

- [ ] **Step 1: Write the deterministic activation-race test**

Create an actor whose `on_activate()` stores its id, sends itself one message,
signals `activation_entered`, and waits on `activation_release`. Its `receive()`
sets `received_before_activation_return` when the release flag is still false.

Add a test with `scheduler_start_paused = true`:

```cpp
TEST(ActorSpawnUnificationTest, ActorCannotRunBeforeActivationReturns) {
    ActivationProbe::reset();
    Config config;
    config.scheduler_threads = 1;
    config.scheduler_start_paused = true;
    ActorSystem system{config};

    std::thread spawning([&] { system.spawn<ActivationProbe>(); });
    ActivationProbe::activation_entered.wait();

    EXPECT_FALSE(system.scheduler()->run_one_ready());
    EXPECT_FALSE(ActivationProbe::received_before_activation_return.load());

    ActivationProbe::activation_release.store(true, std::memory_order_release);
    ActivationProbe::release_waiter.notify_all();
    spawning.join();

    EXPECT_TRUE(system.scheduler()->run_one_ready());
    EXPECT_FALSE(ActivationProbe::received_before_activation_return.load());
}
```

Implement the probe with `std::mutex`, `std::condition_variable`, and atomic
flags local to the fixture; do not use a sleep or a timing-only assertion.

Add `${CMAKE_SOURCE_DIR}/src` to `test_unit_actor` with
`target_include_directories(test_unit_actor PRIVATE ...)` so the focused
spawner unit tests can include private runtime headers. Do not add this path to
`hpactor_lib`'s public interface.

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSpawnUnificationTest.ActorCannotRunBeforeActivationReturns'
```

Expected RED: current spawn notifies readiness before `on_activate()` returns,
so paused deterministic execution can run the actor.

- [ ] **Step 3: Define `SpawnSpec` exactly as the design**

Add the internal `SpawnOrigin` enum and `SpawnSpec` fields from the Phase 2
design. Keep string views synchronous and non-owning. Add `static_assert` checks
that the origin and directory status enums retain `uint8_t` underlying types.

- [ ] **Step 4: Add the admission gate and ready result**

Add to `AbstractActor`:

```cpp
bool spawn_admission_open() const noexcept {
    return spawn_admission_open_.load(std::memory_order_acquire);
}
```

Give only `ActorSpawner` access to:

```cpp
void set_spawn_admission(bool open) noexcept {
    spawn_admission_open_.store(open, std::memory_order_release);
}
```

The atomic defaults to true. Add `ReadyAdmissionCode::ActorStarting`, and make
`ActorReadyGate::try_mark_ready()` return it before actor-mode checks whenever
the actor exists but its spawn admission is closed.

- [ ] **Step 5: Write spawner validation/rollback unit tests**

Add focused tests for:

```cpp
TEST_F(ActorSpawnerTest, RejectsNullActorWithoutPublication);
TEST_F(ActorSpawnerTest, RejectsActorThatCannotBindContext);
TEST_F(ActorSpawnerTest, RejectsReservedIdForNonSystemActor);
TEST_F(ActorSpawnerTest, DuplicateNameLeavesActorCountUnchanged);
TEST_F(ActorSpawnerTest, InjectedPostPublishFailureErasesEntryAndName);
TEST_F(ActorSpawnerTest, SuccessfulAdoptionWiresCompleteEntry);
```

Each assertion checks both directory id and optional name. The successful test
checks address, context, mailbox, scheduler pointer, lifecycle state, and
admission-open state.

- [ ] **Step 6: Run the spawner tests RED**

```bash
cmake -S . -B build -GNinja
ninja -C build test_unit_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorSpawnerTest.*'
```

Expected RED: `ActorSpawner` does not exist.

- [ ] **Step 7: Add the adoption error codes used by the spawner**

Append these constants after existing values in
`include/hpactor/actor/spawn.hpp` without renumbering values `0` through `5`:

```cpp
constexpr uint32_t invalid_actor = 6;
constexpr uint32_t invalid_reserved_id = 7;
constexpr uint32_t duplicate_actor_id = 8;
constexpr uint32_t duplicate_actor_name = 9;
constexpr uint32_t publication_failed = 10;
```

Map invalid actor/reserved id to `FailureReason::RejectedByPolicy`, duplicates
to `FailureReason::Duplicate`, and publication failure to
`FailureReason::SpawnFailed`. Do not add or renumber a canonical failure enum
in this task.

Append `MetricEventType::kActorSpawnFailed = 70` after the current final metric
event without renumbering values `0` through `69`. Successful spawn events use
`code = static_cast<uint8_t>(SpawnOrigin)`; failed events use
`code = static_cast<uint8_t>(FailureReason)` and
`aux = static_cast<uint8_t>(SpawnOrigin)`.

- [ ] **Step 8: Implement the minimal adoption state machine**

Implement the 16-step order in the design. The core structure must be:

```cpp
result<Actor> ActorSpawner::adopt(std::shared_ptr<AbstractActor> actor,
                                  const SpawnSpec& spec) noexcept {
    if (!actor) {
        return result<Actor>::make(
            error(spawn_errors::invalid_actor, "null actor"));
    }

    actor->set_spawn_admission(false);
    // validate identity, assign address/type, allocate mailbox/context,
    // bind/inject, apply quarantine, build entry

    auto status = dependencies_.directory.publish(
        std::move(entry), spec.registered_name);
    if (status != DirectoryPublishStatus::Published) {
        actor->set_spawn_admission(true);
        return result<Actor>::make(map_publish_error(status));
    }

    FAULT_INJECT("hpactor.actor.spawn.after_publish.fail") {
        rollback_publication(id, *actor);
        return result<Actor>::make(
            error(spawn_errors::publication_failed, "injected spawn failure"));
    }

    actor->activate_after_spawn();
    if (auto* lifecycle = actor->as_lifecycle()) {
        lifecycle->transition(LifecycleState::kActive);
    }
    actor->set_spawn_admission(true);
    register_dispatch(*actor, id, spec);
    emit_success(*actor, spec);
    return result<Actor>::make(Actor{std::move(actor)});
}
```

Use named private helpers for validation, effective address, dispatch, error
mapping, rollback, and terminal telemetry. Do not hold the directory lock
outside `publish()`/`erase()`.

Define reserved ids as the inclusive range `0xFFFF0000` through `0xFFFFFFFF`.
Reject a reserved request outside that range, reject any reserved request for
an actor whose `is_system_actor()` is false, and ensure automatic allocation
never returns a value in that range.

Before constructing the spawner, split Phase 1 telemetry initialization into
resource creation and actor startup: create the stable metrics ring buffer and
logger first, construct the spawner with those final pointers, then spawn the
MetricsActor and other early system actors through the spawner. Do not mutate
the spawner's dependency pointers later.

- [ ] **Step 9: Route the Phase 1 template bridge through the spawner**

Keep `spawn<T>()` construction unchanged. Change the private Phase 1
`adopt_preconstructed_actor()` implementation to build a default `SpawnSpec`
from current mailbox/actor dispatch values and call the spawner. On error, emit
no second log and return `Actor{}`.

Remove the old directory/mailbox/lifecycle body from the Phase 1 bridge.

- [ ] **Step 10: Run GREEN**

```bash
ninja -C build hpactor_lib test_unit_actor test_integration_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorSpawnerTest.*:ActorDirectoryTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSpawnUnificationTest.*:ActorSystemTest.*:*DispatchPolicy*:*Lifecycle*'
```

Expected: all selected tests pass; the deterministic race test observes no
execution before activation returns.

- [ ] **Step 11: Commit**

```bash
git add src/runtime include/hpactor/actor include/hpactor/sched \
  src/actor/actor_system.cpp src/sched/actor_ready_gate.cpp \
  tests/unit/actor tests/integration/actor
git commit -m "refactor: unify template actor adoption"
```

---

### Task 4: Route configured topology actors through `ActorSpawner`

**Files:**

- Modify: `src/actor/actor_system.cpp`
- Modify: `src/runtime/actor_spawner.cpp`
- Modify: `tests/integration/actor/test_actor_spawn_unification.cpp`
- Modify: `tests/integration/config/test_bootstrap_engine.cpp`
- Modify: `tests/system/test_system_actor_deep_workflow.cpp`

**Interfaces:**

- Consumes: `ActorSpawner::adopt()` and `SpawnSpec`.
- Produces: configured adapter with atomic topology-name publication.
- Invariant: no separate topology registry write follows adoption.

- [ ] **Step 1: Add failing configured parity tests**

Add tests that load a one-actor topology and assert:

- name resolves immediately after successful load;
- lifecycle is active;
- mailbox/context/scheduler are non-null;
- configured quarantine is applied;
- actor/definition dispatch precedence matches Phase 0;
- one spawn metric/log is emitted with origin `Configured`.

Add a duplicate-name configured-adapter test by calling the existing public
`spawn_configured()` adapter twice with two fresh registered factory actors and
the same `ActorDef::id`:

```cpp
TEST_F(ActorSpawnUnificationTest,
       DuplicateConfiguredNamePublishesNoOrphanActor) {
    const auto before = system.actor_count();
    config::ActorDef def;
    def.id = "duplicate-worker";
    def.behavior = "bootstrap-test";

    auto& factories = config::ActorFactoryRegistry::instance();
    auto first = system.spawn_configured(
        factories.get_factory(def.behavior)(nullptr, system), def);
    ASSERT_TRUE(static_cast<bool>(first));

    auto second = system.spawn_configured(
        factories.get_factory(def.behavior)(nullptr, system), def);

    EXPECT_FALSE(static_cast<bool>(second));
    EXPECT_EQ(system.actor_count(), before + 1U);
    ASSERT_TRUE(static_cast<bool>(system.resolve_actor("duplicate-worker")));
    EXPECT_EQ(system.resolve_actor("duplicate-worker").id(), first.id());
}
```

- [ ] **Step 2: Run RED**

```bash
ninja -C build test_integration_config test_integration_actor
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.*Configured*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSpawnUnificationTest.Configured*:ActorSpawnUnificationTest.DuplicateConfiguredNamePublishesNoOrphanActor'
```

Expected RED: configured spawn still contains manual adoption and/or separate
name publication.

- [ ] **Step 3: Convert `spawn_configured()` into a spec adapter**

Retain the fault-injection behavior at the adapter boundary, then calculate:

```cpp
SpawnSpec spec{
    .type_name = def.behavior,
    .registered_name = def.id,
    .mailbox = mailbox_config_for_actor_def(def),
    .dispatch_policy = effective_dispatch_policy(*actor, def),
    .dispatch_hints = actor->dispatch_hints(),
    .quarantine = def.quarantine.enabled
                      ? std::optional{def.quarantine}
                      : std::nullopt,
    .reserved_id = std::nullopt,
    .actor_type_override = std::nullopt,
    .origin = SpawnOrigin::Configured,
};
```

Delegate once. Delete manual id, mailbox, context, directory, dispatch,
quarantine, and activation code.

- [ ] **Step 4: Remove the separate topology name write**

Delete `registry_.put(...)`/`register_actor(...)` after configured adoption.
Check the returned actor before adding its id to the SystemInit vector. On
adoption error, return the same `result<void>` failure style used by topology
loading and do not send SystemInit to failed actors.

- [ ] **Step 5: Run GREEN**

```bash
ninja -C build test_integration_actor test_integration_config test_system
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSpawnUnificationTest.Configured*:*Lifecycle*:*Quarantine*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.*'
ctest --test-dir build -R 'SystemTopology|ActorDeepWorkflow' \
  --output-on-failure
```

Expected: configured parity and topology tests pass.

- [ ] **Step 6: Prove manual configured publication is gone**

```bash
rg -n "ActorDirectoryEntry|\.insert\(|\.publish\(" \
  src/actor/actor_system.cpp
```

Expected: no configured/manual directory-entry construction or publication
remains in the facade file.

- [ ] **Step 7: Commit**

```bash
git add src/actor/actor_system.cpp src/runtime/actor_spawner.cpp \
  tests/integration/actor tests/integration/config tests/system
git commit -m "refactor: unify configured actor adoption"
```

---

### Task 5: Unify reserved SpawnReceiver and harden remote factories

**Files:**

- Modify: `src/runtime/actor_system_impl.cpp`
- Modify: `src/runtime/actor_spawner.cpp`
- Modify: `include/hpactor/actor/spawn.hpp`
- Modify: `include/hpactor/actor/actor_type_registry.hpp`
- Modify: `src/actor/actor_type_registry.cpp`
- Modify: `tests/integration/actor/test_spawn_receiver_construct.cpp`
- Modify: `tests/integration/spawn/test_actor_type_registry.cpp`
- Modify: `tests/unit/spawn/test_spawn_unit.cpp`

**Interfaces:**

- Consumes: `SpawnSpec::reserved_id`, `actor_type_override`, and unified adopt.
- Produces: canonical invalid-factory errors without wire-schema changes.

- [ ] **Step 1: Add failing reserved receiver parity test**

Construct a network-enabled system with `tcp_port = 0` and assert:

```cpp
TEST(SpawnReceiverConstructTest, ReservedReceiverUsesCompleteAdoption) {
    Config config;
    config.scheduler_threads = 0;
    config.enable_network = true;
    config.tcp_port = 0;
    ActorSystem system{config};

    auto actor = system.get_actor(SpawnReceiverId);
    ASSERT_NE(actor, nullptr);
    EXPECT_EQ(actor->address().id, SpawnReceiverId);
    EXPECT_EQ(actor->address().type, SystemActorType);
    EXPECT_TRUE(actor->is_system_actor());
    EXPECT_NE(system.get_mailbox(SpawnReceiverId), nullptr);
    EXPECT_NE(system.get_context(SpawnReceiverId), nullptr);
    EXPECT_TRUE(actor->spawn_admission_open());
}
```

- [ ] **Step 2: Add failing invalid factory test**

```cpp
TEST(ActorTypeRegistryIntegrationTest, EmptyFactoryResultIsSpawnError) {
    ActorTypeRegistry registry;
    registry.register_factory(
        "empty", [](ActorSystem&, const StreamBuffer&, TypeTag) { return Actor{}; });

    auto result = registry.spawn(system_, "empty", {}, TypeTag::User);

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error().code(), spawn_errors::factory_returned_empty);
}
```

- [ ] **Step 3: Run RED**

```bash
ninja -C build test_integration_actor test_integration_spawn test_unit_spawn
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='SpawnReceiverConstructTest.ReservedReceiverUsesCompleteAdoption'
./build/tests/integration/spawn/test_integration_spawn \
  --gtest_filter='ActorTypeRegistryIntegrationTest.EmptyFactoryResultIsSpawnError'
```

Expected RED: receiver lacks full adoption parity and registry does not return
the new typed error.

- [ ] **Step 4: Add the invalid-factory error without renumbering existing codes**

Append the factory-specific constant after the Phase 2 adoption values:

```cpp
constexpr uint32_t factory_returned_empty = 11;
```

Extend `failure_reason()` mappings. Do not change values `0` through `5` or any
protobuf field.

- [ ] **Step 5: Route receiver creation through the spawner**

After constructing `SpawnReceiver`, build:

```cpp
SpawnSpec spec{
    .type_name = "SpawnReceiver",
    .registered_name = std::nullopt,
    .mailbox = facade.mailbox_config_for_spawn(),
    .dispatch_policy = spawn_receiver->dispatch_policy(),
    .dispatch_hints = spawn_receiver->dispatch_hints(),
    .quarantine = std::nullopt,
    .reserved_id = SpawnReceiverId,
    .actor_type_override = SystemActorType,
    .origin = SpawnOrigin::System,
};
```

Delegate to the spawner. If adoption fails, rely on the spawner's terminal
failure log/metric, store readiness and running false with release ordering,
stop the network loop, join its thread when joinable, stop transport listening,
stop discovery, leave the receiver handle empty, and return from the
remote-startup helper. Delete the manual address, mailbox, context, and
directory-entry code.

- [ ] **Step 6: Validate empty factory handles**

Change default `ActorTypeRegistry::register_type<T>()` factories to construct
`T` and call a private `ActorSystem::adopt_remote_factory_actor()` helper. Make
`ActorTypeRegistry` a friend of the facade; do not expose `SpawnOrigin` or the
private runtime header publicly. The helper builds the same default spec as
template spawn with `SpawnOrigin::RemoteFactory`.

Declare the facade boundary exactly as:

```cpp
friend class ActorTypeRegistry;
Actor adopt_remote_factory_actor(std::shared_ptr<AbstractActor> actor,
                                 std::string_view type_name);
```

Then, in `ActorTypeRegistry::spawn()`, validate the result after any default or
custom factory call:

```cpp
Actor actor = it->second.factory(system, args, args_type);
if (!actor) {
    return result<ActorAddress>::make(error(
        spawn_errors::factory_returned_empty,
        "actor factory returned an empty actor"));
}
return result<ActorAddress>::make(actor.address());
```

Keep `SpawnFactory` unchanged.

- [ ] **Step 7: Run GREEN**

```bash
ninja -C build test_integration_actor test_integration_spawn test_unit_spawn
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='SpawnReceiverConstructTest.*:ActorSpawnUnificationTest.*System*'
./build/tests/integration/spawn/test_integration_spawn
./build/tests/unit/spawn/test_unit_spawn
```

Expected: pass.

- [ ] **Step 8: Commit**

```bash
git add src/runtime/actor_system_impl.cpp src/runtime/actor_spawner.cpp \
  include/hpactor/actor/spawn.hpp src/actor/actor_type_registry.cpp \
  tests/integration/actor tests/integration/spawn tests/unit/spawn
git commit -m "refactor: unify reserved and remote actor spawning"
```

---

### Task 6: Replace `ActorServiceState` with `ActorRuntime`

**Files:**

- Create: `src/runtime/actor_runtime.hpp`
- Create: `src/runtime/actor_runtime.cpp`
- Modify: `src/runtime/CMakeLists.txt`
- Modify: `src/runtime/actor_system_impl.hpp`
- Modify: `src/runtime/actor_system_impl.cpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/architecture/CMakeLists.txt`
- Modify: `tests/integration/actor/test_actor_system.cpp`

**Interfaces:**

- Produces: `ActorRuntime` ownership and lookup/name/type/passivation forwards.
- Consumes: `ActorSpawner`, scheduler and stable telemetry references, and a
  transferred `std::unique_ptr<ActorDirectory>`.
- Invariant: the directory object address does not change during ownership
  transfer.

- [ ] **Step 1: Add failing architecture checks**

Add CTest `ActorSystemPhase2NoActorServiceState` that rejects these tokens from
`src/runtime/actor_system_impl.hpp` outside the `ActorRuntime` member:

```text
ActorServiceState
ActorDirectory actor_directory
ActorTypeRegistry actor_type_registry
PassivationManager passivation_manager
```

Create `tests/architecture/assert_search_allowlist.cmake`. It recursively reads
the explicitly supplied `SEARCH_ROOTS`, finds files containing `PATTERN`, and
fails unless each matching relative path is in `ALLOWLIST`. Add
`ActorSystemPhase2SingleDirectoryPublisher` with production roots `src` and
`include`, patterns `ActorDirectoryEntry entry` and `.publish(`, and allow only
`src/runtime/actor_spawner.cpp`,
`include/hpactor/actor/actor_directory.hpp`, and
`src/actor/actor_directory.cpp`.

Run:

```bash
cmake -S . -B build -GNinja
ctest --test-dir build -R 'ActorSystemPhase2(NoActorServiceState|SingleDirectoryPublisher)' \
  --output-on-failure
```

Expected RED: Phase 1 storage and/or manual sites remain.

- [ ] **Step 2: Add facade-forward parity tests**

Extend `ActorSystemTest` to check that after spawning/naming actors:

- `get_actor`, `get_mailbox`, and `get_context` return the same owned objects;
- `actor_count`, `for_each_actor`, and shutdown snapshot counts match;
- register/resolve/unregister name behavior is unchanged;
- `actor_type_registry()` returns a stable reference;
- `passivation_manager()` and fixed system actor accessors preserve current
  nullability/identity.

Run before ownership movement; expected pass.

- [ ] **Step 3: Implement `ActorRuntime`**

Use the exact interface in the design. It owns:

```cpp
std::unique_ptr<ActorDirectory> directory_;
ActorSpawner spawner_;
std::unique_ptr<ActorTypeRegistry> actor_type_registry_;
std::unique_ptr<PassivationManager> passivation_manager_;
std::unordered_map<ActorType, ActorTypeDef> actor_types_;
Actor system_actor_;
SystemActorHandles system_handles_;
```

`SystemActorHandles` is a fixed struct with named fields, not a keyed service
map:

```cpp
struct SystemActorHandles final {
    Actor spawn_receiver;
    Actor http_gateway;
    std::shared_ptr<cli::CliActor> cli;
    std::shared_ptr<receptionist::Receptionist> receptionist;
    metrics::MetricsActor* metrics{nullptr};
};
```

Document `metrics` as a non-owning alias whose actor instance is retained by
the directory; the `Actor`/`shared_ptr` fields are explicit co-owners matching
their Phase 1 handle types.

- [ ] **Step 4: Construct without a directory/scheduler cycle**

In the composition root:

```cpp
auto directory = std::make_unique<ActorDirectory>();
ActorDirectory& directory_ref = *directory;

core.scheduler = std::make_unique<sched::HybridScheduler>(
    facade, core.config.scheduler_threads, 4, core.config.timer_backend,
    core.config.scheduler_start_paused);

actors = std::make_unique<ActorRuntime>(
    ActorRuntime::Dependencies{
        .facade = facade,
        .endpoint = core.endpoint,
        .scheduler = *core.scheduler,
        .metrics = operations.metrics_ring_buffer.get(),
        .logger = operations.logger,
    },
    std::move(directory));
```

Do not start scheduler threads before the directory ownership transfer and
spawner construction complete. Task 7 replaces this compatibility constructor
with the narrow directory/DLQ/value constructor; keeping it here makes the
ActorRuntime ownership move independently reviewable.

- [ ] **Step 5: Replace facade actor methods with forwards**

Forward lookup, count, iteration, names, actor types, passivation, system actor,
and the private adoption helper to `impl_->actors`. Do not expose an
`ActorRuntime*` public accessor.

- [ ] **Step 6: Move fixed handles and update construction users**

Store/retrieve metrics, CLI, receptionist, HTTP gateway, and SpawnReceiver
handles through fixed ActorRuntime methods. Keep telemetry/network components as
their true resource owners; these are typed aliases only where the directory
owns the actor shared pointer.

- [ ] **Step 7: Run GREEN**

```bash
ninja -C build hpactor_lib test_unit_actor test_integration_actor \
  test_integration_config test_integration_spawn test_system
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemTest.*:ActorSpawnUnificationTest.*:*Shutdown*:*Passivation*'
ctest --test-dir build \
  -R 'ActorSystemPhase2(NoActorServiceState|SingleDirectoryPublisher)|Bootstrap|Spawn|SystemActor' \
  --output-on-failure
```

Expected: all checks pass.

- [ ] **Step 8: Update lifetime inventory and commit**

Record directory transfer, scheduler/spawner references, destruction order, and
fixed handle ownership.

```bash
git add src/runtime src/actor/actor_system.cpp tests \
  docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "refactor: extract ActorRuntime ownership"
```

---

### Task 7: Narrow scheduler execution dependencies

**Files:**

- Create: `include/hpactor/sched/actor_execution_dependencies.hpp`
- Create: `tests/integration/sched/test_actor_execution_dependencies.cpp`
- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `include/hpactor/sched/actor_ready_gate.hpp`
- Modify: `src/sched/actor_ready_gate.cpp`
- Modify: `include/hpactor/sched/actor_execution_engine.hpp`
- Modify: `src/sched/actor_execution_engine.cpp`
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `src/sched/scheduler.cpp`
- Modify: `tests/integration/sched/CMakeLists.txt`
- Modify: `tests/architecture/CMakeLists.txt`

**Interfaces:**

- Produces: `sched::ActorExecutionDependencies` and new `HybridScheduler`
  constructor.
- Preserves: old `HybridScheduler(ActorSystem&, ...)` as delegating adapter.
- Invariant: scheduler production objects store no `ActorSystem&`.

- [ ] **Step 1: Add compatibility and narrow-constructor tests**

Test both constructors:

```cpp
TEST(ActorExecutionDependenciesTest, NarrowConstructorExecutesDirectoryActor);
TEST(ActorExecutionDependenciesTest, ActorSystemConstructorRemainsCompatible);
TEST(ActorExecutionDependenciesTest, ExpiredMessageUsesInjectedDeadLetterQueue);
TEST(ActorExecutionDependenciesTest, CoroutineModeIsCapturedAsImmutableValue);
```

The narrow test creates one directory, stable DLQ, scheduler, and adopted actor,
then uses paused deterministic execution. The expired-message test injects an
expired envelope and checks the exact DLQ instance.

- [ ] **Step 2: Add failing architecture check**

Add `ActorSystemPhase2SchedulerHasNoFacadeDependency` rejecting these patterns
from production scheduler headers/sources:

```text
ActorSystem& system_
ActorSystem& system;
system_.get_actor
system_.get_mailbox
system_.dead_letter_queue
system_.use_coroutines
```

Exclude the delegating constructor body and
`ActorExecutionDependencies::from()` adapter by checking stored fields and
method bodies separately.

Run:

```bash
ctest --test-dir build -R ActorSystemPhase2SchedulerHasNoFacadeDependency \
  --output-on-failure
```

Expected RED: scheduler classes still store/use the facade.

- [ ] **Step 3: Define the dependency bundle**

```cpp
struct ActorExecutionDependencies final {
    ActorDirectory& actors;
    mailbox::DeadLetterQueue* dead_letters;
    bool use_coroutines;

    static ActorExecutionDependencies from(ActorSystem& system) noexcept;
};
```

Implement `from()` at the facade boundary and grant this fixed adapter
friendship in `ActorSystem`; do not add a public directory getter.

- [ ] **Step 4: Add and use the new scheduler constructor**

```cpp
HybridScheduler(ActorExecutionDependencies dependencies,
                uint32_t num_workers,
                uint32_t num_priorities = 4,
                TimerBackend timer_backend = TimerBackend::TimingWheel,
                bool start_paused = false);
```

Delegate the old constructor to
`ActorExecutionDependencies::from(system)`. Store directory reference, DLQ
pointer, and coroutine bool only.

- [ ] **Step 5: Narrow ready gate and execution runners**

- `ActorReadyGate` stores `ActorDirectory&` and performs actor lookup there.
- `BehaviorActorRunner` stores `ActorDirectory&`, stable DLQ pointer, and ready
  gate reference.
- `ActorExecutionEngine` stores immutable coroutine mode.
- `CoroutineActorRunner` removes its unused `ActorSystem&` field.
- `HybridScheduler::execute_actor()` performs directory lookup and passes no
  facade.

Retain the old `ActorSystem&` constructor overloads for `ActorReadyGate`,
`BehaviorActorRunner`, `CoroutineActorRunner`, and `ActorExecutionEngine`; each
delegates through `ActorExecutionDependencies::from()` or ignores the facade
where it was already unused, and none stores it.

Do not move expired-message DLQ policy; substitute the injected pointer for the
same existing behavior line-for-line.

- [ ] **Step 6: Run focused GREEN tests**

```bash
ninja -C build hpactor_lib test_integration_sched test_integration_actor \
  test_integration_mailbox
./build/tests/integration/sched/test_integration_sched \
  --gtest_filter='ActorExecutionDependenciesTest.*:*ReadyGate*:*ExecutionEngine*:*HybridScheduler*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSpawnUnificationTest.*:*Delivery*:*Lifecycle*'
```

Expected: pass with the same ready-state and expiry behavior.

- [ ] **Step 7: Run architecture GREEN**

```bash
ctest --test-dir build -R ActorSystemPhase2SchedulerHasNoFacadeDependency \
  --output-on-failure
rg -n "ActorSystem& system_|system_\.get_actor|system_\.get_mailbox" \
  src/sched include/hpactor/sched
```

Expected: architecture test passes; literal search finds no stored facade actor
lookup in scheduler production code.

- [ ] **Step 8: Commit**

```bash
git add include/hpactor/sched include/hpactor/actor/actor_system.hpp \
  src/sched src/actor/actor_system.cpp tests/integration/sched \
  tests/architecture
git commit -m "refactor: narrow scheduler actor dependencies"
```

---

### Task 8: Architecture closure and regression coverage

**Files:**

- Modify: `tests/architecture/CMakeLists.txt`
- Modify: `tests/integration/actor/test_actor_spawn_unification.cpp`
- Modify: `tests/integration/config/test_bootstrap_engine.cpp`
- Modify: `tests/integration/spawn/test_actor_type_registry.cpp`
- Modify: `docs/architecture/actor/actor-system-phase1-lifetime-inventory.md`

**Deliverable:** Enforced Phase 2 boundaries and complete behavior matrix.

- [ ] **Step 1: Add the final spawn-origin matrix test**

Build a parameterized table covering:

| Origin | Id | Name | Dispatch | Quarantine | Expected |
|---|---|---|---|---|---|
| Programmatic | automatic | none | actor-declared | none | success |
| Configured | automatic | topology id | effective override | optional | success |
| System | reserved | none | actor-declared | none | success |
| RemoteFactory | automatic | none | actor-declared | none | success |
| Configured duplicate | automatic | duplicate | any | any | typed failure/no orphan |
| Invalid reserved | reserved | none | any | none | typed failure/no entry |

For each success, assert complete directory entry, activation before dispatch,
one telemetry event, and open admission. For each failure, assert no id/name and
one failure event.

- [ ] **Step 2: Run matrix RED/GREEN as test coverage closure**

Run before any final production adjustment:

```bash
ninja -C build test_integration_actor test_integration_config \
  test_integration_spawn
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSpawnOriginMatrixTest.*'
```

Expected: if any row fails, make the minimal adapter/spawner correction and
rerun until all rows pass. Do not alter unrelated delivery/network code.

- [ ] **Step 3: Enforce sole publication ownership**

Add final CTest checks that production `ActorDirectoryEntry` construction and
`ActorDirectory::publish()` invocation exist only in the spawner/directory
files. Permit declarations, tests, and documentation.

```bash
ctest --test-dir build -R '^ActorSystemPhase2' --output-on-failure
```

Expected: all Phase 2 architecture tests pass.

- [ ] **Step 4: Verify forbidden mechanisms**

```bash
rg -n "dynamic_cast|typeid|throw |catch \(" \
  src/runtime/actor_runtime.cpp src/runtime/actor_spawner.cpp \
  src/runtime/spawn_spec.hpp include/hpactor/sched \
  src/sched/actor_ready_gate.cpp src/sched/actor_execution_engine.cpp \
  src/sched/scheduler.cpp
```

Expected: no matches introduced by Phase 2.

- [ ] **Step 5: Commit coverage closure**

```bash
git add tests docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "test: enforce unified actor spawning boundaries"
```

---

### Task 9: Sanitizers, full verification, and documentation handoff

**Files:**

- Modify: `CLAUDE_MEMORY.md`
- Modify: `docs/architecture/actor/actor-system-phase1-lifetime-inventory.md`

- [ ] **Step 1: Configure and build ASan targets**

```bash
cmake -S . -B build-asan -GNinja -DENABLE_ASAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-asan hpactor_lib test_unit_actor test_integration_actor \
  test_integration_config test_integration_spawn test_integration_sched
```

- [ ] **Step 2: Run ASan adoption/lifetime scenarios**

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ./build-asan/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorSpawnerTest.*:ActorDirectoryTest.*'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ./build-asan/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSpawnUnificationTest.*:ActorSpawnOriginMatrixTest.*:SpawnReceiverConstructTest.*:*Shutdown*'
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ./build-asan/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.*'
```

Expected: no use-after-free, leak, double publication, or leaked actor/thread.

- [ ] **Step 3: Configure and build TSAN targets**

```bash
cmake -S . -B build-tsan -GNinja -DENABLE_TSAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-tsan hpactor_lib test_unit_actor test_integration_actor \
  test_integration_sched
```

- [ ] **Step 4: Run focused TSAN scenarios**

```bash
TSAN_OPTIONS=halt_on_error=1 \
  ./build-tsan/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSpawnUnificationTest.ActorCannotRunBeforeActivationReturns:ActorSpawnOriginMatrixTest.*:*ConcurrentSpawn*'
TSAN_OPTIONS=halt_on_error=1 \
  ./build-tsan/tests/integration/sched/test_integration_sched \
  --gtest_filter='ActorExecutionDependenciesTest.*:*ReadyGate*'
```

Expected: no race in admission gate, directory publication, id allocation, or
scheduler lookup.

- [ ] **Step 5: Run full normal verification**

This phase changes actor and scheduler public headers and runtime construction,
so full verification is required:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

Expected: full build and suite pass. Record exact test count and duration.

- [ ] **Step 6: Review diff scope and architecture**

```bash
git diff --check origin/main...HEAD
git diff --stat origin/main...HEAD
rg -n "ActorDirectoryEntry" src include | sort
rg -n "ActorSystem& system_|system_\.get_actor|system_\.get_mailbox" \
  src/sched include/hpactor/sched
```

Expected:

- sole production entry construction in `actor_spawner.cpp`;
- no stored scheduler facade dependency;
- no Phase 3 messaging/network files changed beyond the stable dependency
  wiring required here;
- no whitespace errors.

- [ ] **Step 7: Update project memory**

Record in `CLAUDE_MEMORY.md`:

- Phase 2 status, branch, and PR;
- `ActorRuntime`, `ActorSpawner`, and `SpawnSpec` file locations;
- atomic directory publication and activation gate contracts;
- scheduler dependency bundle and compatibility constructor;
- exact normal/ASan/TSAN evidence;
- explicit statement that messaging ownership remains Phase 3 work.

Reconcile the lifetime inventory with final member order and stop sequence.

- [ ] **Step 8: Commit documentation sync**

```bash
git add CLAUDE_MEMORY.md \
  docs/architecture/actor/actor-system-phase1-lifetime-inventory.md
git commit -m "docs: record ActorRuntime architecture"
```

- [ ] **Step 9: Request focused code review**

Invoke `superpowers:requesting-code-review`. Ask reviewers to verify:

1. atomic directory/name publication;
2. activation-gate acquire/release ordering;
3. no actor/user callback under directory lock;
4. spawn-origin parity and error translation;
5. reserved-id and rollback behavior;
6. scheduler dependency lifetime and lack of facade storage; and
7. strict Phase 3 scope boundary.

- [ ] **Step 10: Push and create the implementation PR**

```bash
git push -u origin refactor/actor-system-actor-runtime
gh pr create \
  --base main \
  --head refactor/actor-system-actor-runtime \
  --title "refactor: extract ActorRuntime and unify spawning" \
  --body "Closes #379

Implements ActorSystem refactor Phase 2: ActorRuntime ownership, transactional
ActorSpawner/SpawnSpec adoption, activation admission ordering, atomic actor
id/name publication, reserved SpawnReceiver adoption, and narrow scheduler
execution dependencies.

Design: docs/superpowers/specs/2026-06-27-actor-system-phase2-actor-runtime-design.md
Plan: docs/superpowers/plans/2026-06-27-actor-system-phase2-actor-runtime-implementation.md

Verification evidence is included in the commit/PR checklist."
```

## Completion Checklist

- [ ] Phase 0 and Phase 1 prerequisites were verified from merged code.
- [ ] `ActorRuntime` solely owns the actor directory, spawner, type registry,
  passivation service, and established system identities/handles.
- [ ] `ActorSpawner` is the sole production directory-entry publisher.
- [ ] Template, configured, reserved receiver, and remote default-factory paths
  use unified adoption.
- [ ] Id and optional name publication is atomic.
- [ ] Duplicate/invalid adoption leaves no directory entry or name.
- [ ] Context binding uses an RTTI-free capability.
- [ ] Actor execution cannot begin before activation/lifecycle completion.
- [ ] Self-messages queued during activation are processed after admission opens.
- [ ] Success/failure telemetry occurs exactly once per attempt.
- [ ] Existing public factory, facade, registry, and scheduler constructor APIs
  remain source-compatible.
- [ ] Scheduler execution stores only concrete narrow dependencies.
- [ ] No actor callback or external subsystem call occurs under directory lock.
- [ ] Focused/full tests and required ASan/TSAN scenarios pass.
- [ ] No RTTI, exceptions, service locator, broad lock, wire change, or Phase 3
  messaging extraction was introduced.

## Explicit Stop Point

Stop when the Phase 2 PR is review-ready. Do not begin `MessagingRuntime`, move
DLQ/delivery/backpressure/tracker ownership, alter ordinary delivery ingress, or
extract frame/stream/network behavior. Those require the Phase 3 design and a
new `refactor/...` implementation worktree.
