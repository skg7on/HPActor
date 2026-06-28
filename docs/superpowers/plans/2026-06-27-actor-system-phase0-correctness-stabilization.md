# ActorSystem Phase 0 Correctness Stabilization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stabilize ActorSystem's current ownership boundaries by fixing the dangling DLQ dependency, consolidating actor names in `ActorDirectory`, aligning configured-spawn lifecycle behavior, and making stream routing state thread-safe before structural extraction begins.

**Architecture:** Keep all public `ActorSystem` APIs source-compatible while removing duplicated state and making lifetimes explicit. `ActorDirectory` becomes the sole actor-name store, `DeadLetterQueue` is reconfigured without replacing its object, configured spawn mirrors the established template-spawn contract, and a focused `StreamRegistry` owns synchronized stream-id mappings. This phase deliberately does not introduce PImpl, `Runtime`, or the final component graph.

**Tech Stack:** C++20, CMake, Ninja, GoogleTest 1.14, HPActor `result<T>`, ASan, TSAN, existing protobuf `TypedMessage`, no exceptions, no RTTI.

## Global Constraints

- Execute this plan in `.claude/worktrees/actor-system-phase0-stabilization/` on branch `fix/actor-system-phase0-stabilization`, created from the branch that contains the approved design and this plan.
- Before every write, verify `pwd` ends in `.claude/worktrees/actor-system-phase0-stabilization` and `git branch --show-current` is `fix/actor-system-phase0-stabilization`.
- Follow RED -> GREEN -> REFACTOR for every production behavior change; run the stated RED command before editing production code.
- Preserve existing public `ActorSystem` method signatures, including `registry()`, `load_topology()`, spawn APIs, stream APIs, and DLQ accessors.
- Use `ActorDirectory` as the only source of truth for actor ids, entries, and names.
- Do not replace an owned object while another component retains its raw pointer.
- Do not introduce `dynamic_cast`, `typeid`, exception-based control flow, public `toml++`, a DI container, or a runtime service locator.
- Do not change protobuf schemas, TypeTag assignments, mailbox admission semantics, scheduler ready-state semantics, or wire formats.
- Never hold the directory, DLQ, or stream-registry mutex while invoking actor code, mailbox delivery, scheduler operations, transport operations, logging, or user callbacks.
- Preserve bounded-resource and observability behavior: DLQ loss counters, delivery results, metrics, logs, and lifecycle transitions remain visible.
- Use worktree-local `build/`, `build-asan/`, and `build-tsan/` directories only.
- Prefer the narrowest command after each step; run the full build and test suite only in the final task because this phase changes broad public headers.

## Design Reference

Implement only Phase 0 from:

`docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`

Do not begin the PImpl/runtime-component work from Phase 1. This plan must leave
the existing facade structurally recognizable and buildable.

## Setup and Baseline

- [ ] **Step 1: Create the implementation worktree using the repository rule**

Run from the main checkout after the design branch is available locally:

```bash
git worktree add -b fix/actor-system-phase0-stabilization \
  .claude/worktrees/actor-system-phase0-stabilization \
  docs/actor-system-refactor-design
cd .claude/worktrees/actor-system-phase0-stabilization
```

Expected: `HEAD` contains the approved design and this plan, and the current
branch is `fix/actor-system-phase0-stabilization`.

- [ ] **Step 2: Configure the worktree-local baseline build**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build test_unit_actor test_integration_actor \
  test_integration_config test_integration_mailbox
```

Expected: configuration and all four targets succeed.

- [ ] **Step 3: Run the focused baseline tests**

Run:

```bash
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorDirectoryTest.*:StreamHandleTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemTest.*:ActorIntegrationFinalTest.*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.*'
./build/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='DeadLetterQueueTest.*'
```

Expected: all selected tests pass before modifications. If they do not, stop
and report the pre-existing failures instead of proceeding.

---

### Task 1: Make `ActorDirectory` the sole actor-name registry

**Files:**

- Modify: `include/hpactor/actor/actor_directory.hpp`
- Modify: `src/actor/actor_directory.cpp`
- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/unit/actor/test_actor_directory.cpp`
- Modify: `tests/integration/actor/test_actor_system.cpp`
- Modify: `tests/integration/config/test_bootstrap_engine.cpp`
- Modify: `tests/system/test_system_actor_deep_workflow.cpp`

**Interfaces:**

- Consumes: `ActorDirectory::register_name`, `resolve_name`, `resolve_actor`, `erase`.
- Produces: `bool ActorDirectory::unregister_name(const std::string& name)` and a source-compatible `ActorSystem::ActorRegistry` view backed by `ActorDirectory`.
- Invariant: `ActorDirectory::names_` is the only name map; duplicate registration keeps the first mapping, matching current `resolve_actor()` behavior.

- [ ] **Step 1: Add facade-level failing tests for unregister and topology lookup**

Append to `tests/integration/actor/test_actor_system.cpp`:

```cpp
TEST(ActorSystemTest, UnregisterActorRemovesNameFromResolution) {
    hpactor::Config config;
    config.scheduler_threads = 0;
    hpactor::ActorSystem system{config};

    auto actor = system.spawn<hpactor::EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(actor));
    system.register_actor("ephemeral-worker", actor);
    ASSERT_TRUE(static_cast<bool>(system.resolve_actor("ephemeral-worker")));

    system.unregister_actor("ephemeral-worker");

    EXPECT_FALSE(static_cast<bool>(system.resolve_actor("ephemeral-worker")));
    EXPECT_EQ(system.registry().get("ephemeral-worker").id,
              hpactor::ActorId{0});
}

TEST(ActorSystemTest, DuplicateNameKeepsFirstActor) {
    hpactor::Config config;
    config.scheduler_threads = 0;
    hpactor::ActorSystem system{config};

    auto first = system.spawn<hpactor::EventBasedActor>();
    auto second = system.spawn<hpactor::EventBasedActor>();
    system.register_actor("stable-name", first);
    system.register_actor("stable-name", second);

    auto resolved = system.resolve_actor("stable-name");
    ASSERT_TRUE(static_cast<bool>(resolved));
    EXPECT_EQ(resolved.id(), first.id());
    EXPECT_EQ(system.registry().get("stable-name").id, first.id());
}
```

Extend `BootstrapEngineTest.SingleActor` in
`tests/integration/config/test_bootstrap_engine.cpp`:

```cpp
    auto resolved = system.resolve_actor("my_actor");
    ASSERT_TRUE(static_cast<bool>(resolved));
    EXPECT_EQ(resolved.address(), addr);
```

- [ ] **Step 2: Run the RED tests**

Run:

```bash
ninja -C build test_integration_actor test_integration_config
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemTest.UnregisterActorRemovesNameFromResolution:ActorSystemTest.DuplicateNameKeepsFirstActor'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.SingleActor'
```

Expected before the fix:

- `UnregisterActorRemovesNameFromResolution` fails because `resolve_actor()`
  still finds the directory mapping.
- `BootstrapEngineTest.SingleActor` fails because topology bootstrap writes the
  legacy registry but not `ActorDirectory`.
- The duplicate-name test documents the required keep-first policy.

- [ ] **Step 3: Add directory-level tests before implementation**

Append to `tests/unit/actor/test_actor_directory.cpp`:

```cpp
TEST(ActorDirectoryTest, UnregisterNameRemovesMapping) {
    ActorDirectory directory;
    ActorAddress address{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{7}, 0};
    ASSERT_TRUE(directory.register_name("service", address));

    EXPECT_TRUE(directory.unregister_name("service"));
    EXPECT_FALSE(directory.resolve_name("service").has_value());
    EXPECT_FALSE(directory.unregister_name("service"));
}

TEST(ActorDirectoryTest, DuplicateNameKeepsFirstAddress) {
    ActorDirectory directory;
    ActorAddress first{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{7}, 0};
    ActorAddress second{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{8}, 0};

    EXPECT_TRUE(directory.register_name("service", first));
    EXPECT_FALSE(directory.register_name("service", second));
    ASSERT_TRUE(directory.resolve_name("service").has_value());
    EXPECT_EQ(directory.resolve_name("service")->id, first.id);
}

TEST(ActorDirectoryTest, EraseActorRemovesAllNamesForActor) {
    ActorDirectory directory;
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system{config};
    auto instance = std::make_shared<DirectoryTestActor>(nullptr, system);
    instance->set_address(
        ActorAddress{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{9}, 0});
    Actor actor{instance};
    ASSERT_TRUE(directory.insert({actor, instance, nullptr, nullptr}));
    ASSERT_TRUE(directory.register_name("primary", actor.address()));
    ASSERT_TRUE(directory.register_name("alias", actor.address()));

    EXPECT_TRUE(directory.erase(actor.id()));
    EXPECT_FALSE(directory.resolve_name("primary").has_value());
    EXPECT_FALSE(directory.resolve_name("alias").has_value());
}
```

Expected if built now: compilation fails because `unregister_name` is not yet
declared. This is part of the same observed RED cycle.

- [ ] **Step 4: Add `ActorDirectory::unregister_name` and name cleanup**

Add to the public section of `ActorDirectory` in
`include/hpactor/actor/actor_directory.hpp`:

```cpp
    /// Remove a name-to-address mapping.
    /// Thread-safe. Returns false when the name is not registered.
    bool unregister_name(const std::string& name);
```

Implement in `src/actor/actor_directory.cpp` and update `erase`:

```cpp
bool ActorDirectory::unregister_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return names_.erase(name) > 0;
}

bool ActorDirectory::erase(ActorId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool erased = entries_.erase(id) > 0;
    for (auto it = names_.begin(); it != names_.end();) {
        if (it->second.id == id) {
            it = names_.erase(it);
        } else {
            ++it;
        }
    }
    return erased;
}
```

Do not call `resolve_name`, `find`, or any callback from inside `erase`; they
would acquire the same mutex or violate the no-callback-under-lock contract.

- [ ] **Step 5: Convert the public nested registry into a directory-backed compatibility view**

Replace the map-owning body of `ActorSystem::ActorRegistry` in
`include/hpactor/actor/actor_system.hpp` with:

```cpp
    class ActorRegistry {
      public:
        explicit ActorRegistry(ActorDirectory& directory)
            : directory_(directory) {}

        void put(const std::string& name, ActorAddress addr) {
            (void)directory_.register_name(name, std::move(addr));
        }

        ActorAddress get(const std::string& name) const {
            auto address = directory_.resolve_name(name);
            return address.has_value() ? address.value() : ActorAddress{};
        }

        void erase(const std::string& name) {
            (void)directory_.unregister_name(name);
        }

      private:
        ActorDirectory& directory_;
    };
```

In the private member list, construct the directory before the view:

```cpp
    ActorDirectory actor_directory_;
    ActorRegistry registry_;
```

In the constructor initializer in `src/actor/actor_system.cpp`, make this exact
one-line replacement and leave the later initializers byte-for-byte intact:

```diff
-    : config_(config), endpoint_(config.endpoint), registry_(endpoint_),
+    : config_(config), endpoint_(config.endpoint), registry_(actor_directory_),
```

Reduce `register_actor()` to one write:

```cpp
void ActorSystem::register_actor(const std::string& name, Actor actor) {
    registry_.put(name, actor.address());
}
```

`unregister_actor()` and topology's existing `registry_.put()` calls now
delegate to `ActorDirectory`; no additional topology branch is needed.

- [ ] **Step 6: Correct the stale system-test expectation**

In `tests/system/test_system_actor_deep_workflow.cpp`, replace the expectation
that an unregistered name remains resolvable with:

```cpp
    system.unregister_actor("county");
    auto removed = system.resolve_actor("county");
    EXPECT_EQ(removed.get(), nullptr);
```

Keep the subsequent registration under `county_v2`; it verifies the actor can
be registered again under a new name.

- [ ] **Step 7: Run GREEN verification for Task 1**

Run:

```bash
ninja -C build test_unit_actor test_integration_actor \
  test_integration_config test_system
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorDirectoryTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemTest.*:ActorIntegrationFinalTest.*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.*'
./build/tests/system/test_system \
  --gtest_filter='*ActorDeepWorkflow*'
```

Expected: all selected tests pass and both registry access surfaces return the
same mapping.

- [ ] **Step 8: Commit Task 1**

```bash
git add include/hpactor/actor/actor_directory.hpp \
  src/actor/actor_directory.cpp \
  include/hpactor/actor/actor_system.hpp \
  src/actor/actor_system.cpp \
  tests/unit/actor/test_actor_directory.cpp \
  tests/integration/actor/test_actor_system.cpp \
  tests/integration/config/test_bootstrap_engine.cpp \
  tests/system/test_system_actor_deep_workflow.cpp
git commit -m "fix: unify actor name registry ownership"
```

---

### Task 2: Preserve DLQ identity across topology configuration

**Files:**

- Modify: `include/hpactor/msg/dead_letter_record.hpp`
- Modify: `src/mailbox/dead_letter_queue.cpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/mailbox/test_dead_letter_queue.cpp`
- Modify: `tests/integration/config/test_bootstrap_engine.cpp`

**Interfaces:**

- Consumes: existing `DeadLetterQueue`, `DeadLetterConfig`, and `ActorSystem::dead_letter_queue()`.
- Produces: `void DeadLetterQueue::reconfigure(DeadLetterConfig config) noexcept`.
- Invariant: the queue address remains stable; counters are preserved; capacity shrink drops oldest records and increments `total_lost`.

- [ ] **Step 1: Add the ActorSystem-level RED test**

Append to `tests/integration/config/test_bootstrap_engine.cpp`:

```cpp
TEST(BootstrapEngineTest, DeadLetterReconfigurePreservesQueueIdentity) {
    Config config;
    config.scheduler_threads = 0;
    config.dead_letters.capacity = 8;
    ActorSystem system(config);
    auto* original = system.dead_letter_queue();
    ASSERT_NE(original, nullptr);

    const std::string toml = R"(
[system]
version = "1.0"

[system.dead_letters]
enabled = true
capacity = 2
max_payload_sample_bytes = 16
overflow_policy = "drop_oldest"
store_payload = true
)";
    auto result = system.load_topology(write_temp(toml, "dlq_identity"));
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(system.dead_letter_queue(), original);

    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    auto delivery = system.try_deliver_local(ActorId{999999}, std::move(msg));
    EXPECT_EQ(delivery.code, mailbox::EnqueueResultCode::ActorNotFound);
    auto snapshot = system.dead_letter_snapshot();
    EXPECT_EQ(snapshot.capacity, 2u);
    EXPECT_EQ(snapshot.depth, 1u);
}
```

- [ ] **Step 2: Run the ActorSystem RED test**

Run:

```bash
ninja -C build test_integration_config
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.DeadLetterReconfigurePreservesQueueIdentity'
```

Expected before the fix: pointer equality fails. With ASan, the subsequent
delivery may additionally report use-after-free through the old pipeline DLQ
pointer.

- [ ] **Step 3: Add direct queue reconfiguration tests**

Append to `tests/integration/mailbox/test_dead_letter_queue.cpp`:

```cpp
TEST(DeadLetterQueueTest, ReconfigureShrinksInPlaceAndPreservesCounters) {
    DeadLetterConfig initial;
    initial.capacity = 3;
    DeadLetterQueue queue(initial);
    for (uint64_t id = 1; id <= 3; ++id) {
        DeadLetterRecord record;
        record.message_id = id;
        ASSERT_TRUE(queue.try_push(std::move(record)));
    }

    DeadLetterConfig updated = initial;
    updated.capacity = 2;
    updated.max_payload_sample_bytes = 8;
    queue.reconfigure(updated);

    auto snapshot = queue.snapshot();
    EXPECT_EQ(snapshot.capacity, 2u);
    EXPECT_EQ(snapshot.depth, 2u);
    EXPECT_EQ(snapshot.total_pushed, 3u);
    EXPECT_EQ(snapshot.total_lost, 1u);

    DeadLetterRecord first;
    ASSERT_TRUE(queue.try_pop(first));
    EXPECT_EQ(first.message_id, 2u);
}

TEST(DeadLetterQueueTest, ReconfigureTrimsExistingPayloads) {
    DeadLetterConfig initial;
    initial.capacity = 2;
    initial.max_payload_sample_bytes = 16;
    DeadLetterQueue queue(initial);
    DeadLetterRecord record;
    record.payload_sample = StreamBuffer{1, 2, 3, 4};
    ASSERT_TRUE(queue.try_push(std::move(record)));

    DeadLetterConfig updated = initial;
    updated.max_payload_sample_bytes = 2;
    queue.reconfigure(updated);

    auto records = queue.snapshot_records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().payload_sample.size(), 2u);
}
```

Expected if built now: compilation fails because `reconfigure` does not exist.

- [ ] **Step 4: Implement synchronized in-place reconfiguration**

Add to `DeadLetterQueue` in `include/hpactor/msg/dead_letter_record.hpp`:

```cpp
    /// Apply a new queue policy without changing this object's address.
    /// Existing counters are preserved. Capacity shrink evicts oldest records.
    void reconfigure(DeadLetterConfig config) noexcept;
```

In `src/mailbox/dead_letter_queue.cpp`, move the `try_push` lock before all
configuration reads:

```cpp
bool DeadLetterQueue::try_push(DeadLetterRecord&& record) noexcept {
    FAULT_INJECT("hpactor.mailbox.dlq.push.drop") {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) {
        return false;
    }

    trim_payload(record);
    if (records_.size() >= config_.capacity) {
        switch (config_.overflow_policy) {
            case DeadLetterOverflowPolicy::DropOldestRecord:
                records_.pop_front();
                ++total_lost_;
                break;
            case DeadLetterOverflowPolicy::DropNewestRecord:
                ++total_lost_;
                return false;
            case DeadLetterOverflowPolicy::MetadataOnly:
                record.payload_sample.clear();
                if (records_.size() >= config_.capacity) {
                    records_.pop_front();
                    ++total_lost_;
                }
                break;
        }
    }

    records_.push_back(std::move(record));
    ++total_pushed_;
    return true;
}
```

Implement reconfiguration in the same source file:

```cpp
void DeadLetterQueue::reconfigure(DeadLetterConfig config) noexcept {
    if (config.capacity == 0) {
        config.capacity = 4096;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    config_ = std::move(config);
    for (auto& record : records_) {
        if (!config_.store_payload) {
            record.payload_sample.clear();
        } else if (record.payload_sample.size() >
                   config_.max_payload_sample_bytes) {
            record.payload_sample.resize(config_.max_payload_sample_bytes);
        }
    }
    while (records_.size() > config_.capacity) {
        records_.pop_front();
        ++total_lost_;
    }
}
```

`trim_payload()` is now called only while `mutex_` is held by `try_push()` or
`reconfigure()`. Do not add a second lock inside `trim_payload()`.

- [ ] **Step 5: Stop replacing the queue in `load_topology()`**

Replace the `dead_letters_ = std::make_unique...` block in
`src/actor/actor_system.cpp` with:

```cpp
    config_.dead_letters = model.system.dead_letters;
    if (dead_letters_) {
        dead_letters_->reconfigure(config_.dead_letters);
    }
```

Do not modify `pipeline_cfg.dlq`; it continues pointing to the same stable
queue object.

- [ ] **Step 6: Run GREEN verification for Task 2**

Run:

```bash
ninja -C build test_integration_mailbox test_integration_config
./build/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='DeadLetterQueueTest.*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.DeadLetterReconfigurePreservesQueueIdentity:BootstrapEngineTest.SingleActor'
```

Expected: all selected tests pass; the queue pointer is unchanged and the
delivery pipeline writes into the queue visible through `ActorSystem`.

- [ ] **Step 7: Commit Task 2**

```bash
git add include/hpactor/msg/dead_letter_record.hpp \
  src/mailbox/dead_letter_queue.cpp \
  src/actor/actor_system.cpp \
  tests/integration/mailbox/test_dead_letter_queue.cpp \
  tests/integration/config/test_bootstrap_engine.cpp
git commit -m "fix: preserve dead letter queue lifetime"
```

---

### Task 3: Align configured-spawn lifecycle and instrumentation wiring

**Files:**

- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/config/test_bootstrap_engine.cpp`

**Interfaces:**

- Consumes: existing `ActorSystem::spawn<T>()` behavior and `spawn_configured()`.
- Produces: configured actors enter `LifecycleState::kActive` and receive the same metrics/logger wiring and spawn event emission as template-spawned actors.
- Boundary: this task mirrors behavior only; it does not introduce `ActorSpawner` or deduplicate the two implementations.

- [ ] **Step 1: Add a failing spawn-parity test**

Append to `tests/integration/config/test_bootstrap_engine.cpp`:

```cpp
TEST(BootstrapEngineTest, ConfiguredSpawnMatchesTemplateLifecycle) {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto direct = system.spawn<BootstrapTestActor>();
    auto configured_instance =
        std::make_shared<BootstrapTestActor>(nullptr, system);
    ActorDef def;
    def.behavior = "BootstrapTestActor";
    auto configured =
        system.spawn_configured(std::move(configured_instance), def);

    ASSERT_TRUE(static_cast<bool>(direct));
    ASSERT_TRUE(static_cast<bool>(configured));
    ASSERT_NE(direct.get()->as_lifecycle(), nullptr);
    ASSERT_NE(configured.get()->as_lifecycle(), nullptr);
    EXPECT_EQ(direct.get()->as_lifecycle()->state(), LifecycleState::kActive);
    EXPECT_EQ(configured.get()->as_lifecycle()->state(),
              LifecycleState::kActive);
    EXPECT_NE(system.get_mailbox(configured.id()), nullptr);
}
```

- [ ] **Step 2: Run the RED spawn-parity test**

Run:

```bash
ninja -C build test_integration_config
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.ConfiguredSpawnMatchesTemplateLifecycle'
```

Expected before the fix: the configured actor remains in
`LifecycleState::kStarting` while the direct actor is active.

- [ ] **Step 3: Mirror the established template-spawn wiring**

In `ActorSystem::spawn_configured()` after `set_mailbox()` and before dispatch
registration, add:

```cpp
    if (metrics_ring_buffer_) [[unlikely]] {
        mbox->set_metrics_ring_buffer(metrics_ring_buffer_.get());
        actor->set_metrics_ring_buffer(metrics_ring_buffer_.get());
    }

    if (logger_) [[unlikely]] {
        mbox->set_logger(logger_);
        actor->set_logger(logger_);
    }
```

After `local->on_activate()`, add the same lifecycle and observability steps
used by `spawn<T>()`:

```cpp
    if (auto* lifecycle = actor->as_lifecycle()) {
        lifecycle->transition(LifecycleState::kActive);
    }

    HPACTOR_LOG_INFO(log::LogCategory::kActor, id,
                     static_cast<uint32_t>(log::LogEventId::kActorSpawned),
                     "actor spawned",
                     log::field_lit("type", actor->type_name().data()));

    if (metrics_ring_buffer_) [[unlikely]] {
        metrics::MetricEvent event{};
        event.actor_id = id;
        event.event_type = metrics::MetricEventType::kActorSpawned;
        event.value_hi = 1;
        metrics_ring_buffer_->try_push(event);
    }
```

Do not move or combine the spawn implementations in this task. Phase 2 of the
approved design introduces the single adoption pipeline after these behaviors
are characterized.

- [ ] **Step 4: Remove stale manual lifecycle transitions from touched tests**

In `tests/system/test_system_topology_bootstrap.cpp`, remove only comments and
branches that manually transition newly configured actors from `kStarting` to
`kActive`. Replace them with assertions:

```cpp
    system.for_each_actor([&](ActorId, AbstractActor& actor) {
        if (auto* lifecycle = actor.as_lifecycle()) {
            EXPECT_EQ(lifecycle->state(), LifecycleState::kActive);
            lifecycle->set_drain_config(
                DrainConfig{DrainPolicy::ImmediateStop,
                            std::chrono::milliseconds{500}});
        }
    });
```

Do not edit unrelated lifecycle tests that deliberately construct actors in
other states.

- [ ] **Step 5: Run GREEN verification for Task 3**

Run:

```bash
ninja -C build test_integration_config test_system
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.*'
./build/tests/system/test_system \
  --gtest_filter='TopologyBootstrap.*:GracefulShutdown.*'
```

Expected: configured actors are active immediately after adoption and the
selected topology/shutdown tests pass without manual state repair.

- [ ] **Step 6: Commit Task 3**

```bash
git add src/actor/actor_system.cpp \
  tests/integration/config/test_bootstrap_engine.cpp \
  tests/system/test_system_topology_bootstrap.cpp
git commit -m "fix: align configured actor activation"
```

---

### Task 4: Introduce a synchronized stream registry

**Files:**

- Create: `include/hpactor/actor/stream_registry.hpp`
- Create: `src/actor/stream_registry.cpp`
- Modify: `src/actor/CMakeLists.txt`
- Modify: `include/hpactor/actor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Create: `tests/unit/actor/test_stream_registry.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`

**Interfaces:**

- Consumes: `ActorId`, stream ids, and existing ActorSystem stream APIs.
- Produces: `StreamRegistry::register_sender`, `register_receiver`, `find_sender`, `find_receiver`, `take`, `sender_count`, and `receiver_count`.
- Concurrency contract: registry methods may be called from network, actor, and external threads; the mutex protects maps only and is released before message delivery or callbacks.

- [ ] **Step 1: Write the missing-component RED test**

Create `tests/unit/actor/test_stream_registry.cpp`:

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

#include <hpactor/actor/stream_registry.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace hpactor;

TEST(StreamRegistryTest, TakeReturnsAndRemovesBothRoutes) {
    StreamRegistry registry;
    registry.register_sender(17, ActorId{1});
    registry.register_receiver(17, ActorId{2});

    auto routes = registry.take(17);
    ASSERT_TRUE(routes.sender.has_value());
    ASSERT_TRUE(routes.receiver.has_value());
    EXPECT_EQ(routes.sender.value(), ActorId{1});
    EXPECT_EQ(routes.receiver.value(), ActorId{2});
    EXPECT_FALSE(registry.find_sender(17).has_value());
    EXPECT_FALSE(registry.find_receiver(17).has_value());
}

TEST(StreamRegistryTest, ConcurrentUniqueRegistrationAndRemovalIsConsistent) {
    StreamRegistry registry;
    constexpr uint64_t kThreads = 8;
    constexpr uint64_t kPerThread = 256;
    std::vector<std::thread> workers;
    std::atomic<uint32_t> missing_routes{0};

    for (uint64_t thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&, thread] {
            for (uint64_t offset = 0; offset < kPerThread; ++offset) {
                uint64_t stream = thread * kPerThread + offset + 1;
                registry.register_sender(stream, ActorId{stream});
                registry.register_receiver(stream, ActorId{stream + 10'000});
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(registry.sender_count(), kThreads * kPerThread);
    EXPECT_EQ(registry.receiver_count(), kThreads * kPerThread);

    workers.clear();
    for (uint64_t thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&, thread] {
            for (uint64_t offset = 0; offset < kPerThread; ++offset) {
                uint64_t stream = thread * kPerThread + offset + 1;
                auto routes = registry.take(stream);
                if (!routes.sender.has_value() ||
                    !routes.receiver.has_value()) {
                    missing_routes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(missing_routes.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(registry.sender_count(), 0u);
    EXPECT_EQ(registry.receiver_count(), 0u);
}
```

Add `test_stream_registry.cpp` to `test_unit_actor` in
`tests/unit/actor/CMakeLists.txt`.

- [ ] **Step 2: Run the RED build**

Run:

```bash
ninja -C build test_unit_actor
```

Expected before implementation: compilation fails because
`hpactor/actor/stream_registry.hpp` does not exist.

- [ ] **Step 3: Define the focused registry interface**

Create `include/hpactor/actor/stream_registry.hpp`:

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

#include <hpactor/core/actor_id.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace hpactor {

struct StreamRoutes {
    std::optional<ActorId> sender;
    std::optional<ActorId> receiver;
};

class StreamRegistry {
  public:
    void register_sender(uint64_t stream_id, ActorId actor_id);
    void register_receiver(uint64_t stream_id, ActorId actor_id);

    std::optional<ActorId> find_sender(uint64_t stream_id) const;
    std::optional<ActorId> find_receiver(uint64_t stream_id) const;

    StreamRoutes take(uint64_t stream_id);

    std::size_t sender_count() const;
    std::size_t receiver_count() const;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, ActorId> senders_;
    std::unordered_map<uint64_t, ActorId> receivers_;
};

} // namespace hpactor
```

- [ ] **Step 4: Implement lock-scoped map operations**

Create `src/actor/stream_registry.cpp`:

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

#include <hpactor/actor/stream_registry.hpp>

namespace hpactor {

void StreamRegistry::register_sender(uint64_t stream_id, ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    senders_[stream_id] = actor_id;
}

void StreamRegistry::register_receiver(uint64_t stream_id, ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    receivers_[stream_id] = actor_id;
}

std::optional<ActorId> StreamRegistry::find_sender(uint64_t stream_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = senders_.find(stream_id);
    return it == senders_.end() ? std::nullopt
                                : std::optional<ActorId>{it->second};
}

std::optional<ActorId> StreamRegistry::find_receiver(uint64_t stream_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = receivers_.find(stream_id);
    return it == receivers_.end() ? std::nullopt
                                  : std::optional<ActorId>{it->second};
}

StreamRoutes StreamRegistry::take(uint64_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    StreamRoutes routes;
    if (auto it = senders_.find(stream_id); it != senders_.end()) {
        routes.sender = it->second;
        senders_.erase(it);
    }
    if (auto it = receivers_.find(stream_id); it != receivers_.end()) {
        routes.receiver = it->second;
        receivers_.erase(it);
    }
    return routes;
}

std::size_t StreamRegistry::sender_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return senders_.size();
}

std::size_t StreamRegistry::receiver_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return receivers_.size();
}

} // namespace hpactor
```

Add `stream_registry.cpp` to `src/actor/CMakeLists.txt`.

- [ ] **Step 5: Replace ActorSystem's raw stream maps**

In `include/hpactor/actor/actor_system.hpp`:

```cpp
#include <hpactor/actor/stream_registry.hpp>
```

Replace `stream_senders_` and `stream_receivers_` with:

```cpp
    StreamRegistry stream_registry_;
    std::atomic<uint64_t> stream_counter_{0};
```

Update the facade methods in `src/actor/actor_system.cpp`:

```cpp
void ActorSystem::register_stream_sender(uint64_t stream_id, ActorId actor_id) {
    stream_registry_.register_sender(stream_id, actor_id);
}

void ActorSystem::register_stream_receiver(uint64_t stream_id, ActorId actor_id) {
    stream_registry_.register_receiver(stream_id, actor_id);
}

void ActorSystem::unregister_stream(uint64_t stream_id) {
    (void)stream_registry_.take(stream_id);
}
```

Replace the data handler with this complete method:

```cpp
void ActorSystem::deliver_remote_stream_data(const net::WireFrame& frame) {
    const auto& data = frame.pb_envelope.stream_data();
    auto receiver = stream_registry_.find_receiver(data.stream_id());
    if (!receiver.has_value())
        return;

    const auto& payload_string = data.payload();
    auto payload = StreamBuffer::from_data(
        reinterpret_cast<const uint8_t*>(payload_string.data()),
        payload_string.size());
    TypedMessage msg(stream::StreamDataTag, std::move(payload));
    (void)try_deliver_local_fast(receiver.value(), std::move(msg));
}
```

Replace the ACK handler with this complete method:

```cpp
void ActorSystem::deliver_remote_stream_ack(const net::WireFrame& frame) {
    const auto& ack = frame.pb_envelope.stream_ack();
    auto sender = stream_registry_.find_sender(ack.stream_id());
    if (!sender.has_value())
        return;

    auto payload = StreamBuffer::from_data(
        reinterpret_cast<const uint8_t*>(&ack), sizeof(ack));
    TypedMessage msg(stream::StreamAckTag, std::move(payload));
    (void)try_deliver_local_fast(sender.value(), std::move(msg));
}
```

Replace the close handler with this complete method:

```cpp
void ActorSystem::deliver_remote_stream_close(const net::WireFrame& frame) {
    const auto& close = frame.pb_envelope.stream_close();
    auto routes = stream_registry_.take(close.stream_id());
    if (routes.sender.has_value()) {
        TypedMessage msg(stream::StreamClosedTag, StreamBuffer{});
        (void)try_deliver_local_fast(routes.sender.value(), std::move(msg));
    }
    if (routes.receiver.has_value()) {
        TypedMessage msg(stream::StreamClosedTag, StreamBuffer{});
        (void)try_deliver_local_fast(routes.receiver.value(), std::move(msg));
    }
}
```

Replace the error handler with this complete method:

```cpp
void ActorSystem::deliver_remote_stream_error(const net::WireFrame& frame) {
    const auto& error = frame.pb_envelope.stream_error();
    auto routes = stream_registry_.take(error.stream_id());
    if (routes.sender.has_value()) {
        TypedMessage msg(stream::StreamErrorTag, StreamBuffer{});
        (void)try_deliver_local_fast(routes.sender.value(), std::move(msg));
    }
    if (routes.receiver.has_value()) {
        TypedMessage msg(stream::StreamErrorTag, StreamBuffer{});
        (void)try_deliver_local_fast(routes.receiver.value(), std::move(msg));
    }
}
```

Do not hold the registry mutex while calling `try_deliver_local_fast()`;
`find_*()` and `take()` return copied ids before delivery begins.

- [ ] **Step 6: Run GREEN verification for Task 4**

Run:

```bash
ninja -C build test_unit_actor test_integration_actor
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='StreamRegistryTest.*:StreamHandleTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemTest.*'
```

Expected: all selected tests pass.

- [ ] **Step 7: Commit Task 4**

```bash
git add include/hpactor/actor/stream_registry.hpp \
  src/actor/stream_registry.cpp \
  src/actor/CMakeLists.txt \
  include/hpactor/actor/actor_system.hpp \
  src/actor/actor_system.cpp \
  tests/unit/actor/test_stream_registry.cpp \
  tests/unit/actor/CMakeLists.txt
git commit -m "fix: synchronize actor stream registry"
```

---

### Task 5: Sanitizer, integration, documentation, and full verification

**Files:**

- Modify: `CLAUDE_MEMORY.md`

**Interfaces:**

- Consumes: all Task 1-4 changes.
- Produces: verified Phase 0 baseline and updated project memory.
- Gate: do not start Phase 1 until every command in this task succeeds or its failure is documented and resolved.

- [ ] **Step 1: Run the complete focused Phase 0 test matrix**

Run:

```bash
ninja -C build test_unit_actor test_integration_actor \
  test_integration_config test_integration_mailbox test_system
./build/tests/unit/actor/test_unit_actor \
  --gtest_filter='ActorDirectoryTest.*:StreamRegistryTest.*:StreamHandleTest.*'
./build/tests/integration/actor/test_integration_actor \
  --gtest_filter='ActorSystemTest.*:ActorIntegrationFinalTest.*:DeliverySemanticsTest.*'
./build/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.*:MailboxConfigTest.*'
./build/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='DeadLetterQueueTest.*'
./build/tests/system/test_system \
  --gtest_filter='TopologyBootstrap.*:GracefulShutdown.*:*ActorDeepWorkflow*'
```

Expected: every selected test passes.

- [ ] **Step 2: Verify the DLQ lifetime under ASan**

Run:

```bash
cmake -S . -B build-asan -GNinja -DENABLE_ASAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-asan test_integration_config test_integration_mailbox
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 \
  ./build-asan/tests/integration/config/test_integration_config \
  --gtest_filter='BootstrapEngineTest.DeadLetterReconfigurePreservesQueueIdentity'
ASAN_OPTIONS=halt_on_error=1:detect_leaks=1 \
  ./build-asan/tests/integration/mailbox/test_integration_mailbox \
  --gtest_filter='DeadLetterQueueTest.*'
```

Expected: tests pass with no ASan or leak report.

- [ ] **Step 3: Verify stream-registry synchronization under TSAN**

Run:

```bash
cmake -S . -B build-tsan -GNinja -DENABLE_TSAN=ON \
  -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-tsan test_unit_actor
TSAN_OPTIONS=halt_on_error=1 \
  ./build-tsan/tests/unit/actor/test_unit_actor \
  --gtest_filter='StreamRegistryTest.*'
```

Expected: tests pass with no TSAN report.

- [ ] **Step 4: Update project memory with the completed phase**

Add this entry near the current feature-status history in `CLAUDE_MEMORY.md`:

```markdown
**ActorSystem Refactor Phase 0: Correctness Stabilization** ✅ Complete (2026-06-27)
- Consolidated actor names in `ActorDirectory`; `ActorSystem::ActorRegistry` is now a compatibility view.
- Preserved `DeadLetterQueue` object identity during topology configuration and added synchronized in-place reconfiguration.
- Aligned configured-spawn lifecycle, logger, metrics, and spawn-event behavior with template spawning.
- Added a synchronized `StreamRegistry` with atomic route removal and TSAN coverage.
- Verified focused actor/config/mailbox/system coverage plus ASan DLQ lifetime and TSAN stream-registry checks.
```

Do not claim the later PImpl, runtime components, immutable blueprint, or
lifecycle coordinator are implemented.

- [ ] **Step 5: Run full cross-cutting verification**

Because this phase changes `actor_system.hpp` and shared runtime behavior, run:

```bash
ninja -C build
ctest --test-dir build --output-on-failure --parallel 8
git diff --check
git status --short --branch
```

Expected:

- Full build succeeds.
- CTest reports zero failed tests.
- Diff check is clean.
- Only `CLAUDE_MEMORY.md` remains uncommitted after Tasks 1-4 commits.

- [ ] **Step 6: Commit verification documentation**

```bash
git add CLAUDE_MEMORY.md
git commit -m "docs: record ActorSystem phase 0 stabilization"
```

- [ ] **Step 7: Inspect the final branch**

Run:

```bash
git log --oneline --decorate -5
git status --short --branch
git diff main...HEAD --stat
```

Expected: five focused commits, a clean worktree, and changes limited to the
Phase 0 files listed in this plan.

## Completion Gate

Phase 0 is complete only when:

- `ActorDirectory` is the sole actor-name store.
- `registry()`, `register_actor()`, `resolve_actor()`, `unregister_actor()`, and
  topology registration all observe the same mapping and duplicate policy.
- Removing an actor removes its associated names.
- `load_topology()` never replaces the `DeadLetterQueue` object retained by
  `DeliveryPipeline`.
- DLQ reconfiguration is synchronized, preserves counters, and has defined
  capacity-shrink behavior.
- Template and configured spawning both leave lifecycle actors active and wire
  logger/metrics consistently.
- Stream route maps are no longer unsynchronized members of `ActorSystem`.
- The stream registry releases its mutex before delivery and passes TSAN.
- Focused tests, ASan checks, TSAN checks, the full build, and the full CTest
  suite all pass.
- Project memory describes only completed Phase 0 behavior.

After this gate is reviewed and accepted, write a separate implementation plan
for Phase 1 (`ActorSystem::Impl` and the `Runtime` ownership shell). Do not
continue into Phase 1 from this plan.
