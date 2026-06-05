# ActorSystem Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `ActorSystem` into focused runtime components while preserving public actor APIs and fixing named actor resolution plus remote spawn argument propagation.

**Architecture:** Keep `ActorSystem` as the user-facing facade. Move actor identity, local delivery, backpressure, remote runtime, topology bootstrap, and shutdown phase ownership into compiled components with narrow contracts and tests. Each task is behavior-preserving unless it is explicitly a tested bug fix.

**Tech Stack:** C++20, CMake, Ninja, GoogleTest, protobuf `TypedMessage`, existing HPActor `result<T>` failure model, no exceptions, no RTTI.

---

## Setup

Use a linked worktree under the repository `.worktrees/` directory before executing this plan.

Recommended branch:

```bash
git switch -c worktree-actor-system-refactor
```

Configure if `build/` does not exist:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Use narrow verification after each task. Run the full suite before opening a pull request.

## File Structure

- Modify: `include/hpactor/core/actor_system.hpp` for facade delegation and reduced private state.
- Modify: `src/actor/actor_system.cpp` for facade methods and wiring removal.
- Modify: `src/CMakeLists.txt` to compile new runtime components.
- Create: `include/hpactor/actor/actor_directory.hpp` for local actor directory API.
- Create: `src/actor/actor_directory.cpp` for actor id, actor slot, mailbox, context, and name registry ownership.
- Create: `include/hpactor/actor/local_delivery_engine.hpp` for local delivery admission API.
- Create: `src/actor/local_delivery_engine.cpp` for delivery result, dedup, expiry, DLQ, mailbox admission, and scheduler wakeup.
- Create: `include/hpactor/actor/backpressure_coordinator.hpp` for backpressure fanout API.
- Create: `src/actor/backpressure_coordinator.cpp` for local callback, metric, and remote wire signal dispatch.
- Create: `include/hpactor/actor/shutdown_coordinator.hpp` for graceful shutdown orchestration.
- Create: `src/actor/shutdown_coordinator.cpp` for shutdown phase transitions and actor drain sequencing.
- Create: `include/hpactor/net/remote_runtime.hpp` for remote runtime lifecycle API.
- Create: `src/net/remote_runtime.cpp` for event loop, transport, discovery, RPC channel, and remote ingress ownership.
- Create: `include/hpactor/spawn/remote_spawn_client.hpp` for remote spawn request API.
- Create: `src/spawn/remote_spawn_client.cpp` for remote spawn request serialization and response mapping.
- Create: `include/hpactor/config/topology_bootstrapper.hpp` for topology load orchestration.
- Create: `src/config/topology_bootstrapper.cpp` for TOML parse and bootstrap execution.
- Create: `tests/unit/actor/test_actor_directory.cpp` for `ActorDirectory` tests.
- Modify: `tests/unit/actor/CMakeLists.txt` to compile the directory tests.
- Modify: `tests/integration/actor/test_actor_system.cpp` for named actor resolution regression coverage.
- Modify: `tests/integration/actor/test_actor_system_backpressure.cpp` for local delivery regression coverage.
- Modify: `tests/integration/actor/test_delivery_semantics.cpp` for DLQ/dedup/expiry characterization if missing.
- Modify: `tests/integration/actor/test_backpressure_signals.cpp` for extracted coordinator regression coverage.
- Modify: `tests/integration/actor/test_remote_backpressure_signals.cpp` for remote signal regression coverage.
- Modify: `tests/integration/actor/test_shutdown_coordinator.cpp` for extracted shutdown coordinator regression coverage.
- Modify: `tests/integration/spawn/test_actor_type_registry.cpp` for spawn argument propagation.
- Modify: `tests/integration/spawn/test_spawn_receiver.cpp` for remote spawn receiver argument propagation.
- Modify: `tests/unit/spawn/test_spawn_serialization.cpp` for spawn argument serialization coverage.
- Modify: `tests/integration/spawn/CMakeLists.txt` only if new spawn test files are split out.

## Task 1: Add Regression Tests For Known Bugs

**Files:**
- Modify: `tests/integration/actor/test_actor_system.cpp`
- Modify: `tests/integration/spawn/test_actor_type_registry.cpp`
- Modify: `tests/unit/spawn/test_spawn_serialization.cpp`

- [ ] **Step 1: Add a failing named actor resolution test**

Add this test to `tests/integration/actor/test_actor_system.cpp` near existing named actor or spawn tests:

```cpp
TEST(ActorSystemTest, ResolveActorReturnsRegisteredNamedActor) {
    Config config;
    ActorSystem system{config};

    auto actor = system.spawn<EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(actor));
    system.register_actor("named-worker", actor);

    auto resolved = system.resolve_actor("named-worker");
    EXPECT_TRUE(static_cast<bool>(resolved));
    EXPECT_EQ(resolved.id(), actor.id());
    EXPECT_EQ(resolved.address(), actor.address());
}
```

- [ ] **Step 2: Run the named actor test and verify it fails before the fix**

Run:

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter=ActorSystemTest.ResolveActorReturnsRegisteredNamedActor
```

Expected before code change: the test fails because `static_cast<bool>(resolved)` is false.

- [ ] **Step 3: Add a failing actor type registry args test**

Add this helper actor and test to `tests/integration/spawn/test_actor_type_registry.cpp`:

```cpp
namespace {

class ArgsEchoActor final : public EventBasedActor {
public:
    ArgsEchoActor(ActorContext* ctx, ActorSystem& sys, std::string value)
        : EventBasedActor(ctx, sys), value_(std::move(value)) {}

    const std::string& value() const noexcept { return value_; }

private:
    std::string value_;
};

StreamBuffer make_string_args(std::string_view value) {
    return StreamBuffer{value.begin(), value.end()};
}

std::string read_string_args(const StreamBuffer& args) {
    return std::string(reinterpret_cast<const char*>(args.data()),
                       args.size());
}

}  // namespace

TEST(ActorTypeRegistryTest, SpawnPassesArgsToFactory) {
    ActorTypeRegistry registry;
    registry.register_factory(
        "ArgsEchoActor",
        [](ActorSystem& system, const StreamBuffer& args, TypeTag args_type)
            -> Actor {
            EXPECT_EQ(args_type, TypeTag::User);
            return system.spawn<ArgsEchoActor>(read_string_args(args));
        });

    Config config;
    ActorSystem system{config};
    auto actor_result = registry.spawn(system, "ArgsEchoActor",
                                       make_string_args("remote-payload"),
                                       TypeTag::User);

    ASSERT_TRUE(actor_result.has_value());
    auto spawned = system.get_actor(actor_result.value().id);
    ASSERT_NE(spawned, nullptr);
    auto* echo = static_cast<ArgsEchoActor*>(spawned.get());
    EXPECT_EQ(echo->value(), "remote-payload");
}
```

- [ ] **Step 4: Run the actor type registry test and verify it fails before the fix**

Run:

```bash
ninja -C build test_integration_spawn
./build/tests/integration/spawn/test_integration_spawn --gtest_filter=ActorTypeRegistryTest.SpawnPassesArgsToFactory
```

Expected before code change: the test fails because the factory receives empty args.

- [ ] **Step 5: Add spawn serialization coverage for args**

If `tests/unit/spawn/test_spawn_serialization.cpp` does not already verify non-empty args, add:

```cpp
TEST(SpawnSerializationTest, RequestPreservesArgsBytes) {
    const std::string payload = "alpha=7";

    hpactor::ProtoTypeRegistry registry;
    registry.register_system_types();

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name("ArgsEchoActor");
    pb_req.set_args_type(static_cast<uint32_t>(hpactor::TypeTag::User));
    pb_req.set_serialized_args(payload);

    hpactor::StreamBuffer encoded = registry.serialize(pb_req);
    ASSERT_FALSE(encoded.empty());

    auto decoded =
        registry.deserialize(hpactor::TypeTag::SpawnRequestTag, encoded);
    ASSERT_NE(decoded, nullptr);
    auto* decoded_req =
        static_cast<::hpactor::SpawnRequestMessage*>(decoded.get());
    EXPECT_EQ(decoded_req->actor_type_name(), "ArgsEchoActor");
    EXPECT_EQ(decoded_req->args_type(),
              static_cast<uint32_t>(hpactor::TypeTag::User));
    EXPECT_EQ(decoded_req->serialized_args(), payload);
}
```

- [ ] **Step 6: Run the spawn serialization test**

Run:

```bash
ninja -C build test_unit_spawn
./build/tests/unit/spawn/test_unit_spawn --gtest_filter=SpawnSerializationTest.RequestPreservesArgsBytes
```

Expected: the test either passes because serialization already preserves args, or fails with a clear mismatch that must be fixed in the spawn serialization layer before remote spawn wiring.

- [ ] **Step 7: Commit regression tests**

Run:

```bash
git add tests/integration/actor/test_actor_system.cpp tests/integration/spawn/test_actor_type_registry.cpp tests/unit/spawn/test_spawn_serialization.cpp
git commit -m "test: characterize actor system refactor regressions"
```

## Task 2: Fix Named Actor Resolution And Spawn Args Propagation

**Files:**
- Modify: `src/actor/actor_system.cpp`
- Modify: `src/actor_type_registry.cpp`
- Modify: `include/hpactor/actor_type_registry.hpp` if the factory signature needs to expose args consistently.
- Modify: `src/spawn.cpp` only if serialization helpers from Task 1 fail.

- [ ] **Step 1: Fix `ActorSystem::resolve_actor()`**

Update the method so it returns the actor matching the resolved local id:

```cpp
Actor ActorSystem::resolve_actor(const std::string& name) {
    ActorAddress address = registry_.get(name);
    if (!address) {
        return Actor{};
    }

    std::lock_guard<std::mutex> lock(actors_mutex_);
    auto it = actors_.find(address.id);
    if (it == actors_.end()) {
        return Actor{};
    }
    return Actor{it->second};
}
```

- [ ] **Step 2: Pass args through synchronous remote spawn**

Update `ActorSystem::spawn_remote()` in `src/actor/actor_system.cpp`:

```cpp
result<ActorRef> ActorSystem::spawn_remote(const std::string& node_name,
                                           const std::string& actor_type,
                                           const StreamBuffer& args) {
    return spawn_remote_async(node_name, actor_type, args).get();
}
```

- [ ] **Step 3: Pass args through async remote spawn request construction**

Update `ActorSystem::spawn_remote_async()` so the request uses the caller-provided buffer:

```cpp
AsyncActor ActorSystem::spawn_remote_async(const std::string& node_name,
                                           const std::string& actor_type,
                                           const StreamBuffer& args) {
    AsyncActor handle(endpoint_, config_.spawn_timeout_ms);

    if (!config_.enable_network || !transport_) {
        SpawnResponse resp;
        resp.error_code = spawn_errors::node_unreachable;
        handle.set_response(resp);
        return handle;
    }

    auto remote_endpoint = endpoint_ops::parse_endpoint(node_name);

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name(actor_type);
    pb_req.set_args_type(static_cast<uint32_t>(TypeTag::User));
    pb_req.set_serialized_args(reinterpret_cast<const char*>(args.data()),
                               args.size());
    net::to_proto(pb_req.mutable_supervisor(), system_actor_.address());

    StreamBuffer request_bytes = proto_registry_.serialize(pb_req);
    // Keep the existing message id, pending_spawns_, frame, and transport
    // code unchanged below this point.
}
```

- [ ] **Step 4: Pass args into actor factory invocation**

Update `include/hpactor/actor_type_registry.hpp` so factories can receive the serialized args while the existing `register_type<T>()` API keeps working:

```cpp
using SpawnFactory =
    std::function<Actor(ActorSystem&, const StreamBuffer&, TypeTag)>;

template <typename T> void register_type(const std::string& name) {
    ActorType id = next_type_id_++;
    types_by_name_[name] = TypeEntry{
        id,
        [](ActorSystem& sys, const StreamBuffer&, TypeTag) -> Actor {
            return sys.spawn<T>();
        }};
    names_by_type_[id] = name;
}

void register_factory(const std::string& name, SpawnFactory factory);
```

Then update `ActorTypeRegistry::spawn()` in `src/actor_type_registry.cpp`:

```cpp
result<ActorAddress>
ActorTypeRegistry::spawn(ActorSystem& system, const std::string& type_name,
                         const StreamBuffer& args,
                         TypeTag args_type) {
    auto it = types_by_name_.find(type_name);
    if (it == types_by_name_.end()) {
        return result<ActorAddress>::make(
            error(spawn_errors::unknown_type,
                  "unknown actor type: " + type_name));
    }

    Actor actor = it->second.factory(system, args, args_type);
    return result<ActorAddress>::make(actor.address());
}
```

- [ ] **Step 5: Run focused bug-fix tests**

Run:

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter=ActorSystemTest.ResolveActorReturnsRegisteredNamedActor
ninja -C build test_integration_spawn
./build/tests/integration/spawn/test_integration_spawn --gtest_filter=ActorTypeRegistryTest.SpawnPassesArgsToFactory
ninja -C build test_unit_spawn
./build/tests/unit/spawn/test_unit_spawn --gtest_filter=SpawnSerializationTest.RequestPreservesArgsBytes
```

Expected: all three selected tests pass.

- [ ] **Step 6: Commit bug fixes**

Run:

```bash
git add src/actor/actor_system.cpp src/actor_type_registry.cpp include/hpactor/actor_type_registry.hpp src/spawn.cpp tests/integration/actor/test_actor_system.cpp tests/integration/spawn/test_actor_type_registry.cpp tests/unit/spawn/test_spawn_serialization.cpp
git commit -m "fix: preserve actor lookup and remote spawn args"
```

## Task 3: Extract ActorDirectory

**Files:**
- Create: `include/hpactor/actor/actor_directory.hpp`
- Create: `src/actor/actor_directory.cpp`
- Create: `tests/unit/actor/test_actor_directory.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add the `ActorDirectory` interface**

Create `include/hpactor/actor/actor_directory.hpp`:

```cpp
#pragma once

#include "hpactor/actor/actor_context.hpp"
#include "hpactor/actor/typed_message.hpp"
#include "hpactor/core/actor.hpp"
#include "hpactor/mailbox/mpsc_actor_mailbox.hpp"
#include "hpactor/ref/actor_address.hpp"
#include "hpactor/types.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {

class AbstractActor;

struct ActorDirectoryEntry {
    Actor actor;
    std::shared_ptr<AbstractActor> instance;
    std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>> mailbox;
    std::shared_ptr<ActorContext> context;
};

class ActorDirectory {
public:
    ActorId allocate_id();
    bool insert(ActorDirectoryEntry entry);
    std::optional<ActorDirectoryEntry> find(ActorId id) const;
    std::optional<Actor> find_actor(ActorId id) const;
    std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>>
    find_mailbox(ActorId id) const;
    std::shared_ptr<ActorContext> find_context(ActorId id) const;
    bool register_name(std::string name, ActorAddress address);
    std::optional<ActorAddress> resolve_name(const std::string& name) const;
    std::optional<Actor> resolve_actor(const std::string& name) const;
    std::vector<ActorDirectoryEntry> snapshot() const;
    bool erase(ActorId id);
    std::size_t size() const noexcept;

private:
    mutable std::mutex mutex_;
    uint64_t next_actor_id_{1};
    std::unordered_map<ActorId, ActorDirectoryEntry> entries_;
    std::unordered_map<std::string, ActorAddress> names_;
};

}  // namespace hpactor
```

- [ ] **Step 2: Add failing `ActorDirectory` tests**

Create `tests/unit/actor/test_actor_directory.cpp`:

```cpp
#include "hpactor/actor/actor_directory.hpp"
#include "hpactor/core/actor_system.hpp"
#include "hpactor/actor/event_based_actor.hpp"
#include "hpactor/mailbox/mpsc_actor_mailbox.hpp"

#include <gtest/gtest.h>

using namespace hpactor;

namespace {

class DirectoryActor final : public EventBasedActor {
public:
    DirectoryActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    Behavior make_behavior() override { return {}; }
};

}  // namespace

TEST(ActorDirectoryTest, AllocateIdIncrements) {
    ActorDirectory directory;
    auto first = directory.allocate_id();
    auto second = directory.allocate_id();
    EXPECT_NE(first, second);
    EXPECT_LT(first.value(), second.value());
}

TEST(ActorDirectoryTest, InsertAndFindEntry) {
    ActorDirectory directory;
    Config config;
    ActorSystem system{config};
    auto instance = std::make_shared<DirectoryActor>(nullptr, system);
    instance->set_address(
        ActorAddress{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{42}, 0});
    Actor actor{instance};
    auto mailbox =
        std::make_shared<mailbox::MPSCActorMailbox<TypedMessage>>(
            ActorId{42}, nullptr, mailbox::MailboxConfig{});
    auto context = std::shared_ptr<ActorContext>{};

    ASSERT_TRUE(directory.insert({actor, instance, mailbox, context}));

    auto found = directory.find(ActorId{42});
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->actor.id(), ActorId{42});
    EXPECT_EQ(found->instance, instance);
    EXPECT_EQ(found->mailbox, mailbox);
}

TEST(ActorDirectoryTest, ResolveActorByName) {
    ActorDirectory directory;
    Config config;
    ActorSystem system{config};
    auto instance = std::make_shared<DirectoryActor>(nullptr, system);
    instance->set_address(
        ActorAddress{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{7}, 0});
    Actor actor{instance};
    ASSERT_TRUE(directory.insert({actor, nullptr, nullptr, nullptr}));
    ASSERT_TRUE(directory.register_name("service", actor.address()));

    auto resolved = directory.resolve_actor("service");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->id(), ActorId{7});
}
```

- [ ] **Step 3: Add the test source to `tests/unit/actor/CMakeLists.txt`**

Update the executable source list:

```cmake
add_executable(test_unit_actor
    test_abstract_actor.cpp
    test_proto_registry.cpp
    test_actor_directory.cpp
)
```

Preserve any additional existing source entries in the same list.

- [ ] **Step 4: Run the new tests and verify they fail before implementation**

Run:

```bash
ninja -C build test_unit_actor
./build/tests/unit/actor/test_unit_actor --gtest_filter=ActorDirectoryTest.*
```

Expected before implementation: build fails because `hpactor/actor/actor_directory.hpp` or methods are missing.

- [ ] **Step 5: Implement `ActorDirectory`**

Create `src/actor/actor_directory.cpp`:

```cpp
#include "hpactor/actor/actor_directory.hpp"

namespace hpactor {

ActorId ActorDirectory::allocate_id() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ActorId{next_actor_id_++};
}

bool ActorDirectory::insert(ActorDirectoryEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto id = entry.actor.id();
    return entries_.emplace(id, std::move(entry)).second;
}

std::optional<ActorDirectoryEntry> ActorDirectory::find(ActorId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<Actor> ActorDirectory::find_actor(ActorId id) const {
    auto entry = find(id);
    if (!entry.has_value()) {
        return std::nullopt;
    }
    return entry->actor;
}

std::shared_ptr<mailbox::MPSCActorMailbox<TypedMessage>>
ActorDirectory::find_mailbox(ActorId id) const {
    auto entry = find(id);
    return entry.has_value() ? entry->mailbox : nullptr;
}

std::shared_ptr<ActorContext> ActorDirectory::find_context(ActorId id) const {
    auto entry = find(id);
    return entry.has_value() ? entry->context : nullptr;
}

bool ActorDirectory::register_name(std::string name, ActorAddress address) {
    std::lock_guard<std::mutex> lock(mutex_);
    return names_.emplace(std::move(name), std::move(address)).second;
}

std::optional<ActorAddress>
ActorDirectory::resolve_name(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = names_.find(name);
    if (it == names_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<Actor> ActorDirectory::resolve_actor(
    const std::string& name) const {
    auto address = resolve_name(name);
    if (!address.has_value()) {
        return std::nullopt;
    }
    return find_actor(address->id());
}

std::vector<ActorDirectoryEntry> ActorDirectory::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ActorDirectoryEntry> result;
    result.reserve(entries_.size());
    for (const auto& [_, entry] : entries_) {
        result.push_back(entry);
    }
    return result;
}

bool ActorDirectory::erase(ActorId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.erase(id) != 0;
}

std::size_t ActorDirectory::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace hpactor
```

- [ ] **Step 6: Add `ActorDirectory` to the library**

Modify `src/CMakeLists.txt`:

```cmake
add_library(hpactor_lib SHARED
    actor/abstract_actor.cpp
    actor/actor_context.cpp
    actor/actor_directory.cpp
    actor/actor_system.cpp
```

Keep the rest of the existing source list unchanged.

- [ ] **Step 7: Migrate name lookup and simple actor lookups**

In `include/hpactor/core/actor_system.hpp`, add:

```cpp
#include "hpactor/actor/actor_directory.hpp"
```

Add private state:

```cpp
ActorDirectory actor_directory_;
```

Then update `src/actor/actor_system.cpp` so `resolve_actor()`, name registration, and read-only actor/mailbox/context lookups use `actor_directory_`. Keep existing maps in place for fields not yet migrated.

- [ ] **Step 8: Run directory and actor system tests**

Run:

```bash
ninja -C build test_unit_actor test_integration_actor
./build/tests/unit/actor/test_unit_actor --gtest_filter=ActorDirectoryTest.*
./build/tests/integration/actor/test_integration_actor --gtest_filter=ActorSystemTest.ResolveActorReturnsRegisteredNamedActor
```

Expected: selected tests pass.

- [ ] **Step 9: Commit ActorDirectory extraction**

Run:

```bash
git add include/hpactor/actor/actor_directory.hpp src/actor/actor_directory.cpp src/CMakeLists.txt include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/unit/actor/test_actor_directory.cpp tests/unit/actor/CMakeLists.txt
git commit -m "refactor: introduce actor directory"
```

## Task 4: Move Local Spawn Registration Behind Directory Helpers

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/actor/test_actor_system.cpp`
- Modify: `tests/integration/actor/test_lifecycle_actor.cpp`
- Modify: `tests/integration/actor/test_is_system_actor.cpp`

- [ ] **Step 1: Add characterization tests for spawn-visible behavior**

Add these tests to `tests/integration/actor/test_actor_system.cpp` if equivalent coverage is absent:

```cpp
TEST(ActorSystemTest, SpawnRegistersActorForDelivery) {
    Config config;
    ActorSystem system{config};
    auto actor = system.spawn<EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(actor));

    TypedMessage msg;
    msg.set_type_tag(static_cast<uint32_t>(TypeTag::UserMessage));
    auto result = system.try_deliver_local(actor.id(), std::move(msg));

    EXPECT_TRUE(result.accepted());
}

TEST(ActorSystemTest, SpawnedActorAppearsInSnapshot) {
    Config config;
    ActorSystem system{config};
    auto actor = system.spawn<EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(actor));

    auto actors = system.actors();
    auto found = std::find_if(actors.begin(), actors.end(), [&](const Actor& item) {
        return item.id() == actor.id();
    });
    EXPECT_NE(found, actors.end());
}
```

- [ ] **Step 2: Run characterization tests**

Run:

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter=ActorSystemTest.SpawnRegistersActorForDelivery:ActorSystemTest.SpawnedActorAppearsInSnapshot
```

Expected: selected tests pass before refactor.

- [ ] **Step 3: Add a compiled registration helper**

In `include/hpactor/core/actor_system.hpp`, declare a private helper:

```cpp
Actor register_local_actor(std::shared_ptr<AbstractActor> actor,
                           std::string name,
                           ActorSpawnOptions options);
```

In the `spawn<T>()` template, keep construction in the header and delegate after construction:

```cpp
auto actor_ptr = std::make_shared<T>(std::forward<Args>(args)...);
return register_local_actor(std::move(actor_ptr), std::move(name),
                            std::move(options));
```

- [ ] **Step 4: Implement the helper with existing spawn behavior**

Move the non-template registration work from `spawn<T>()` into `src/actor/actor_system.cpp`:

```cpp
Actor ActorSystem::register_local_actor(std::shared_ptr<AbstractActor> actor,
                                        std::string name,
                                        ActorSpawnOptions options) {
    auto id = actor_directory_.allocate_id();
    ActorAddress address{id, node_name_};
    Actor handle{address};

    auto mailbox = create_mailbox_for_actor(options);
    auto context = std::make_shared<ActorContext>(*this, handle, mailbox);

    if (auto* local = actor->as_local_actor()) {
        local->set_context(context);
    }

    actor_directory_.insert({handle, actor, mailbox, context});
    if (!name.empty()) {
        actor_directory_.register_name(std::move(name), address);
    }

    register_actor_metrics(handle);
    start_actor_if_needed(actor);
    return handle;
}
```

Use the exact existing helper names from `actor_system.cpp` for mailbox creation, local actor context attach, metrics, scheduler wakeup, and lifecycle start. If a helper does not exist, move the existing code block intact into `register_local_actor()`.

- [ ] **Step 5: Run spawn regression tests**

Run:

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter=ActorSystemTest.SpawnRegistersActorForDelivery:ActorSystemTest.SpawnedActorAppearsInSnapshot:ActorSystemTest.ResolveActorReturnsRegisteredNamedActor
```

Expected: selected tests pass.

- [ ] **Step 6: Commit spawn helper extraction**

Run:

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/integration/actor/test_actor_system.cpp
git commit -m "refactor: delegate local actor registration"
```

## Task 5: Extract LocalDeliveryEngine

**Files:**
- Create: `include/hpactor/actor/local_delivery_engine.hpp`
- Create: `src/actor/local_delivery_engine.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/actor/test_actor_system_backpressure.cpp`
- Modify: `tests/integration/actor/test_delivery_semantics.cpp`

- [ ] **Step 1: Add characterization tests for delivery behavior**

Add or confirm these behaviors in `tests/integration/actor/test_actor_system_backpressure.cpp` and `tests/integration/actor/test_delivery_semantics.cpp`:

```cpp
TEST_F(ActorSystemBackpressureTest, TryDeliverLocalMissingActorReturnsRejected) {
    TypedMessage msg;
    msg.set_type_tag(static_cast<uint32_t>(TypeTag::UserMessage));

    auto result = system_->try_deliver_local(ActorId{99999}, std::move(msg));

    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.code, mailbox::EnqueueResultCode::ActorNotFound);
}

TEST_F(ActorSystemBackpressureTest, TryDeliverLocalAcceptedWakesMailbox) {
    auto actor = system_->spawn<EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(actor));

    TypedMessage msg;
    msg.set_type_tag(static_cast<uint32_t>(TypeTag::UserMessage));
    auto result = system_->try_deliver_local(actor.id(), std::move(msg));

    EXPECT_TRUE(result.accepted());
}
```

- [ ] **Step 2: Run characterization tests**

Run:

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter=ActorSystemBackpressureTest.TryDeliverLocalMissingActorReturnsRejected:ActorSystemBackpressureTest.TryDeliverLocalAcceptedWakesMailbox:DeliverySemanticsTest.*
```

Expected: selected tests pass before extraction.

- [ ] **Step 3: Add the `LocalDeliveryEngine` interface**

Create `include/hpactor/actor/local_delivery_engine.hpp`:

```cpp
#pragma once

#include "hpactor/actor/actor_directory.hpp"
#include "hpactor/mailbox/mailbox_policy.hpp"
#include "hpactor/types/typed_message.hpp"

#include <chrono>
#include <functional>
#include <memory>

namespace hpactor {

class DeadLetterQueue;
class DedupCache;

struct LocalDeliveryOptions {
    bool emit_dead_letter{true};
};

struct LocalDeliveryDependencies {
    ActorDirectory* directory{nullptr};
    DeadLetterQueue* dead_letters{nullptr};
    DedupCache* dedup_cache{nullptr};
    std::function<void(ActorId)> wake_actor;
    std::function<void(const mailbox::BackpressureSignal&)>
        signal_backpressure;
};

class LocalDeliveryEngine {
public:
    explicit LocalDeliveryEngine(LocalDeliveryDependencies deps);

    mailbox::EnqueueResult try_deliver(
        ActorId target, TypedMessage msg, uint8_t priority = 0,
        int64_t deadline_ns = INT64_MAX, mailbox::DeliveryOptions options = {});

private:
    LocalDeliveryDependencies deps_;
};

}  // namespace hpactor
```

- [ ] **Step 4: Implement `LocalDeliveryEngine` by moving existing logic**

Create `src/actor/local_delivery_engine.cpp` and move the current logic from `ActorSystem::try_deliver_local()` into `LocalDeliveryEngine::try_deliver()`:

```cpp
#include "hpactor/actor/local_delivery_engine.hpp"

namespace hpactor {

LocalDeliveryEngine::LocalDeliveryEngine(LocalDeliveryDependencies deps)
    : deps_(std::move(deps)) {}

mailbox::EnqueueResult LocalDeliveryEngine::try_deliver(
    ActorId target, TypedMessage msg, uint8_t priority, int64_t deadline_ns,
    mailbox::DeliveryOptions options) {
    auto mailbox = deps_.directory->find_mailbox(target);
    if (!mailbox) {
        return reject_missing_actor(deps_.dead_letters, target, msg, options,
                                    priority, deadline_ns);
    }

    msg.set_deadline_ns(deadline_ns);
    mailbox::MailboxEnvelopeMeta meta;
    meta.sender = msg.sender_address();
    meta.type_tag = msg.type_id();
    meta.priority = priority;
    meta.deadline_ns = deadline_ns;
    meta.flags = options.flags;
    meta.message_id = options.message_id;
    if (options.no_drop) {
        meta.flags |= net::WireFrame::NoDrop;
    }

    auto result = mailbox->try_push(std::move(msg), meta);
    if (result.accepted()) {
        if (deps_.wake_actor) {
            deps_.wake_actor(target);
        }
        return result;
    }

    if (result.backpressure_signal.has_value() && deps_.signal_backpressure) {
        deps_.signal_backpressure(*result.backpressure_signal);
    }
    record_rejected_dead_letter(deps_.dead_letters, target, result);
    return result;
}

}  // namespace hpactor
```

Move the existing dead-letter helper bodies from `actor_system.cpp` into anonymous-namespace helpers in this file. Preserve current duplicate, expired, missing actor, and mailbox rejection behavior.

- [ ] **Step 5: Wire `ActorSystem` to the engine**

Add a member in `include/hpactor/core/actor_system.hpp`:

```cpp
std::unique_ptr<LocalDeliveryEngine> local_delivery_;
```

Initialize it in the `ActorSystem` constructor in `src/actor/actor_system.cpp`:

```cpp
local_delivery_ = std::make_unique<LocalDeliveryEngine>(
    LocalDeliveryDependencies{
        .directory = &actor_directory_,
        .dead_letters = dead_letters_.get(),
        .dedup_cache = dedup_cache_.get(),
        .wake_actor = [this](ActorId id) { wake_actor(id); },
        .signal_backpressure =
            [this](const mailbox::BackpressureSignal& signal) {
                signal_backpressure(signal);
            },
    });
```

Then reduce `ActorSystem::try_deliver_local()` to facade delegation:

```cpp
mailbox::EnqueueResult ActorSystem::try_deliver_local(
    ActorId target, TypedMessage msg, uint8_t priority,
    int64_t deadline_ns, mailbox::DeliveryOptions options) {
    return local_delivery_->try_deliver(
        target, std::move(msg), priority, deadline_ns, options);
}
```

- [ ] **Step 6: Add the source file to `src/CMakeLists.txt`**

```cmake
add_library(hpactor_lib SHARED
    actor/abstract_actor.cpp
    actor/actor_context.cpp
    actor/actor_directory.cpp
    actor/local_delivery_engine.cpp
    actor/actor_system.cpp
```

- [ ] **Step 7: Run delivery regression tests**

Run:

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter=ActorSystemBackpressureTest.*:DeliverySemanticsTest.*:BackpressureSignalsTest.*
```

Expected: selected tests pass.

- [ ] **Step 8: Commit local delivery extraction**

Run:

```bash
git add include/hpactor/actor/local_delivery_engine.hpp src/actor/local_delivery_engine.cpp src/CMakeLists.txt include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/integration/actor/test_actor_system_backpressure.cpp tests/integration/actor/test_delivery_semantics.cpp
git commit -m "refactor: extract local delivery engine"
```

## Task 6: Extract BackpressureCoordinator

**Files:**
- Create: `include/hpactor/actor/backpressure_coordinator.hpp`
- Create: `src/actor/backpressure_coordinator.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/actor/test_backpressure_signals.cpp`
- Modify: `tests/integration/actor/test_remote_backpressure_signals.cpp`

- [ ] **Step 1: Add characterization tests for backpressure fanout**

Confirm or add tests for:

```cpp
TEST_F(BackpressureSignalsTest, LocalSignalInvokesSenderContextCallback) {
    mailbox::BackpressureSignal observed;
    bool signaled = false;
    sender_ctx->on_backpressure([&](const mailbox::BackpressureSignal& signal) {
        observed = signal;
        signaled = true;
    });

    system_->signal_backpressure(make_soft_pressure_signal(sender.id(),
                                                           target.id()));

    EXPECT_TRUE(signaled);
    EXPECT_EQ(observed.sender, sender.id());
}

TEST(RemoteBackpressureSignalsTest, RemoteSenderReceivesControlFrame) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:9001");
    cfg.scheduler_threads = 0;
    cfg.mailbox.default_capacity = 1;
    cfg.mailbox.backpressure_mode = mailbox::BackpressureMode::RemoteSignal;
    ActorSystem system{cfg};

    bool emitted = false;
    system.set_backpressure_signal_wire_sink_for_test(
        [&](const mailbox::BackpressureSignal&) { emitted = true; });

    system.signal_backpressure(make_remote_pressure_signal());

    EXPECT_TRUE(emitted);
}
```

Use the existing helper names in those files for actor setup and signal construction.

- [ ] **Step 2: Run characterization tests**

Run:

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter=BackpressureSignalsTest.*:RemoteBackpressureSignalsTest.*
```

Expected: selected tests pass before extraction.

- [ ] **Step 3: Add the coordinator interface**

Create `include/hpactor/actor/backpressure_coordinator.hpp`:

```cpp
#pragma once

#include "hpactor/mailbox/backpressure_signal.hpp"
#include "hpactor/mailbox/mailbox_config.hpp"

#include <functional>

namespace hpactor {

struct BackpressureCoordinatorDependencies {
    std::function<void(const mailbox::BackpressureSignal&)>
        emit_local_signal;
    std::function<void(const mailbox::BackpressureSignal&)>
        emit_remote_signal;
    std::function<void(const mailbox::BackpressureSignal&)>
        emit_metric;
};

class BackpressureCoordinator {
public:
    explicit BackpressureCoordinator(BackpressureCoordinatorDependencies deps);

    void signal(const mailbox::BackpressureSignal& signal,
                mailbox::BackpressureMode mode);

private:
    BackpressureCoordinatorDependencies deps_;
};

}  // namespace hpactor
```

- [ ] **Step 4: Implement the coordinator**

Create `src/actor/backpressure_coordinator.cpp`:

```cpp
#include "hpactor/actor/backpressure_coordinator.hpp"

namespace hpactor {
namespace {

bool local_signal_enabled(mailbox::BackpressureMode mode) noexcept {
    return mode == mailbox::BackpressureMode::LocalSignal ||
           mode == mailbox::BackpressureMode::LocalAndRemoteSignal;
}

bool remote_signal_enabled(mailbox::BackpressureMode mode) noexcept {
    return mode == mailbox::BackpressureMode::RemoteSignal ||
           mode == mailbox::BackpressureMode::LocalAndRemoteSignal;
}

}  // namespace

BackpressureCoordinator::BackpressureCoordinator(
    BackpressureCoordinatorDependencies deps)
    : deps_(std::move(deps)) {}

void BackpressureCoordinator::signal(const mailbox::BackpressureSignal& signal,
                                     mailbox::BackpressureMode mode) {
    if (deps_.emit_metric) {
        deps_.emit_metric(signal);
    }
    if (local_signal_enabled(mode) && deps_.emit_local_signal) {
        deps_.emit_local_signal(signal);
    }
    if (remote_signal_enabled(mode) && deps_.emit_remote_signal) {
        deps_.emit_remote_signal(signal);
    }
}

}  // namespace hpactor
```

- [ ] **Step 5: Wire `ActorSystem::signal_backpressure()` through the coordinator**

In `include/hpactor/core/actor_system.hpp`:

```cpp
std::unique_ptr<BackpressureCoordinator> backpressure_;
```

In the constructor:

```cpp
backpressure_ = std::make_unique<BackpressureCoordinator>(
    BackpressureCoordinatorDependencies{
        .emit_local_signal =
            [this](const mailbox::BackpressureSignal& signal) {
                emit_local_backpressure_signal(signal);
            },
        .emit_remote_signal =
            [this](const mailbox::BackpressureSignal& signal) {
                emit_remote_backpressure_signal(signal);
            },
        .emit_metric =
            [this](const mailbox::BackpressureSignal& signal) {
                record_backpressure_metric(signal);
            },
    });
```

Reduce `signal_backpressure()`:

```cpp
void ActorSystem::signal_backpressure(
    const mailbox::BackpressureSignal& signal) {
    backpressure_->signal(signal, config_.mailbox.backpressure_mode);
}
```

- [ ] **Step 6: Add source to `src/CMakeLists.txt`**

```cmake
add_library(hpactor_lib SHARED
    actor/abstract_actor.cpp
    actor/actor_context.cpp
    actor/actor_directory.cpp
    actor/backpressure_coordinator.cpp
    actor/local_delivery_engine.cpp
    actor/actor_system.cpp
```

- [ ] **Step 7: Run backpressure tests**

Run:

```bash
ninja -C build test_integration_actor
./build/tests/integration/actor/test_integration_actor --gtest_filter=BackpressureSignalsTest.*:RemoteBackpressureSignalsTest.*:ActorSystemBackpressureTest.*
```

Expected: selected tests pass.

- [ ] **Step 8: Commit backpressure extraction**

Run:

```bash
git add include/hpactor/actor/backpressure_coordinator.hpp src/actor/backpressure_coordinator.cpp src/CMakeLists.txt include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/integration/actor/test_backpressure_signals.cpp tests/integration/actor/test_remote_backpressure_signals.cpp
git commit -m "refactor: extract backpressure coordinator"
```

## Task 7: Extract RemoteSpawnClient And RemoteRuntime

**Files:**
- Create: `include/hpactor/spawn/remote_spawn_client.hpp`
- Create: `src/spawn/remote_spawn_client.cpp`
- Create: `include/hpactor/net/remote_runtime.hpp`
- Create: `src/net/remote_runtime.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/spawn/test_spawn_receiver.cpp`
- Modify: `tests/integration/spawn/test_spawn_integration.cpp`

- [ ] **Step 1: Add remote spawn receiver args test**

Add a test to `tests/integration/spawn/test_spawn_receiver.cpp` that sends a spawn request with non-empty args and asserts the registered factory receives them:

```cpp
TEST(SpawnReceiverTest, RemoteSpawnRequestPassesArgsToFactory) {
    Config config;
    ActorSystem system{config};
    std::string observed;

    ActorTypeRegistry registry;
    registry.register_factory(
        "ArgsEchoActor",
        [&](ActorSystem& sys, const StreamBuffer& args, TypeTag args_type)
            -> Actor {
            EXPECT_EQ(args_type, TypeTag::User);
            observed.assign(reinterpret_cast<const char*>(args.data()),
                            args.size());
            return sys.spawn<EventBasedActor>();
        });

    SpawnReceiver receiver(system, registry, nullptr);

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name("ArgsEchoActor");
    pb_req.set_args_type(static_cast<uint32_t>(TypeTag::User));
    pb_req.set_serialized_args("payload-42");

    StreamBuffer encoded = system.proto_registry().serialize(pb_req);
    TypedMessage msg(TypeTag::SpawnRequestTag, std::move(encoded));
    auto behavior = receiver.make_behavior();
    behavior(msg);

    EXPECT_EQ(observed, "payload-42");
}
```

- [ ] **Step 2: Run remote spawn receiver test**

Run:

```bash
ninja -C build test_integration_spawn
./build/tests/integration/spawn/test_integration_spawn --gtest_filter=SpawnReceiverTest.RemoteSpawnRequestPassesArgsToFactory
```

Expected: selected test passes after Task 2 and before extraction.

- [ ] **Step 3: Add the `RemoteSpawnClient` interface**

Create `include/hpactor/spawn/remote_spawn_client.hpp`:

```cpp
#pragma once

#include "hpactor/ref/actor_ref.hpp"
#include "hpactor/types/result.hpp"
#include "hpactor/adt/stream_buffer.hpp"
#include "hpactor/net/endpoint.hpp"
#include "hpactor/ref/actor_address.hpp"

#include <future>
#include <memory>
#include <string>
#include <functional>

namespace hpactor {

class AsyncActor;
class ProtoTypeRegistry;
namespace net {
class Transport;
}

struct RemoteSpawnClientDependencies {
    EndPoint local_endpoint;
    ActorAddress system_actor_address;
    uint32_t spawn_timeout_ms{5000};
    bool network_enabled{false};
    ProtoTypeRegistry* proto_registry{nullptr};
    net::Transport* transport{nullptr};
    std::function<void(uint64_t, std::shared_ptr<AsyncActor>)>
        register_pending_spawn;
};

class RemoteSpawnClient {
public:
    explicit RemoteSpawnClient(RemoteSpawnClientDependencies deps);

    AsyncActor spawn(const std::string& node_name,
                     const std::string& actor_type,
                     const StreamBuffer& args,
                     TypeTag args_type = TypeTag::User);

private:
    RemoteSpawnClientDependencies deps_;
};

}  // namespace hpactor
```

- [ ] **Step 4: Implement `RemoteSpawnClient` by moving existing request logic**

Create `src/spawn/remote_spawn_client.cpp`:

```cpp
#include "hpactor/spawn/remote_spawn_client.hpp"

#include "hpactor/common.pb.h"
#include "hpactor/core/actor_system_ids.hpp"
#include "hpactor/core/proto_type_registry.hpp"
#include "hpactor/messages.pb.h"
#include "hpactor/net/frame.hpp"
#include "hpactor/net/transport.hpp"
#include "hpactor/spawn.hpp"

namespace hpactor {

RemoteSpawnClient::RemoteSpawnClient(RemoteSpawnClientDependencies deps)
    : deps_(std::move(deps)) {}

AsyncActor RemoteSpawnClient::spawn(const std::string& node_name,
                                    const std::string& actor_type,
                                    const StreamBuffer& args,
                                    TypeTag args_type) {
    AsyncActor handle(deps_.local_endpoint, deps_.spawn_timeout_ms);

    if (!deps_.network_enabled || deps_.transport == nullptr ||
        deps_.proto_registry == nullptr) {
        SpawnResponse resp;
        resp.error_code = spawn_errors::node_unreachable;
        handle.set_response(resp);
        return handle;
    }

    auto remote_endpoint = endpoint_ops::parse_endpoint(node_name);

    ::hpactor::SpawnRequestMessage pb_req;
    pb_req.set_actor_type_name(actor_type);
    pb_req.set_args_type(static_cast<uint32_t>(args_type));
    pb_req.set_serialized_args(reinterpret_cast<const char*>(args.data()),
                               args.size());
    net::to_proto(pb_req.mutable_supervisor(), deps_.system_actor_address);

    StreamBuffer request_bytes = deps_.proto_registry->serialize(pb_req);
    uint64_t msg_id = generate_message_id().value();

    net::WireFrame frame;
    net::to_proto(frame.pb_frame.mutable_sender(),
                  deps_.system_actor_address);
    net::to_proto(
        frame.pb_frame.mutable_receiver(),
        ActorAddress{remote_endpoint, SystemActorType, SpawnReceiverId, 0});
    frame.pb_frame.set_message_id(msg_id);
    frame.pb_frame.set_flags(net::WireFrame::RpcRequest);
    frame.pb_frame.set_payload(reinterpret_cast<const char*>(request_bytes.data()),
                               request_bytes.size());

    auto pending = std::make_shared<AsyncActor>(std::move(handle));
    pending->set_message_id(msg_id);
    deps_.register_pending_spawn(msg_id, pending);

    deps_.transport->send(net::from_proto(frame.pb_frame.receiver()),
                          frame.encode());
    return std::move(*pending);
}

}  // namespace hpactor
```

- [ ] **Step 5: Add the `RemoteRuntime` interface**

Create `include/hpactor/net/remote_runtime.hpp`:

```cpp
#pragma once

#include "hpactor/net/endpoint.hpp"
#include "hpactor/types/result.hpp"

#include <memory>
#include <string>

namespace hpactor {

class ActorLocationCache;
class IServiceDiscovery;
class RpcChannel;
class TcpTransport;

class RemoteRuntime {
public:
    RemoteRuntime(std::string node_name, EndPoint endpoint);
    ~RemoteRuntime();

    result<void> start();
    void stop();
    RpcChannel* rpc_channel() noexcept;
    ActorLocationCache* location_cache() noexcept;
    IServiceDiscovery* discovery() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hpactor
```

- [ ] **Step 6: Implement `RemoteRuntime` by moving lifecycle ownership**

Create `src/net/remote_runtime.cpp` and move event loop, transport, discovery, location cache, and RPC lifecycle code from `ActorSystem` into `RemoteRuntime::Impl`. Preserve existing startup and shutdown ordering.

The public methods should keep this shape:

```cpp
result<void> RemoteRuntime::start() {
    return impl_->start();
}

void RemoteRuntime::stop() {
    impl_->stop();
}

RpcChannel* RemoteRuntime::rpc_channel() noexcept {
    return impl_->rpc_channel.get();
}
```

- [ ] **Step 7: Wire `ActorSystem` to remote components**

In `include/hpactor/core/actor_system.hpp`, replace remote ownership fields with:

```cpp
std::unique_ptr<RemoteRuntime> remote_runtime_;
std::unique_ptr<RemoteSpawnClient> remote_spawn_client_;
```

In `src/actor/actor_system.cpp`, update remote facade methods:

```cpp
AsyncActor ActorSystem::spawn_remote_async(const std::string& node_name,
                                           const std::string& actor_type,
                                           const StreamBuffer& args) {
    return remote_spawn_client_->spawn(node_name, actor_type, args,
                                      TypeTag::User);
}
```

Keep `deliver_remote()` and remote ingress delegating through `RemoteRuntime` and `LocalDeliveryEngine`.

- [ ] **Step 8: Add sources to `src/CMakeLists.txt`**

```cmake
add_library(hpactor_lib SHARED
    actor/abstract_actor.cpp
    actor/actor_context.cpp
    actor/actor_directory.cpp
    actor/backpressure_coordinator.cpp
    actor/local_delivery_engine.cpp
    actor/actor_system.cpp
    spawn/remote_spawn_client.cpp
    net/remote_runtime.cpp
```

- [ ] **Step 9: Run spawn and remote delivery tests**

Run:

```bash
ninja -C build test_unit_spawn test_integration_spawn test_integration_actor
./build/tests/unit/spawn/test_unit_spawn --gtest_filter=SpawnSerializationTest.*
./build/tests/integration/spawn/test_integration_spawn --gtest_filter=ActorTypeRegistryTest.*:SpawnReceiverTest.*:SpawnIntegrationTest.*
./build/tests/integration/actor/test_integration_actor --gtest_filter=RemoteBackpressureSignalsTest.*:DeliverySemanticsTest.*
```

Expected: selected tests pass.

- [ ] **Step 10: Commit remote extraction**

Run:

```bash
git add include/hpactor/spawn/remote_spawn_client.hpp src/spawn/remote_spawn_client.cpp include/hpactor/net/remote_runtime.hpp src/net/remote_runtime.cpp src/CMakeLists.txt include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/integration/spawn/test_spawn_receiver.cpp tests/integration/spawn/test_spawn_integration.cpp
git commit -m "refactor: extract remote actor runtime"
```

## Task 8: Extract TopologyBootstrapper

**Files:**
- Create: `include/hpactor/config/topology_bootstrapper.hpp`
- Create: `src/config/topology_bootstrapper.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/config/test_bootstrap_engine.cpp`
- Modify: `tests/system/test_system_topology_bootstrap.cpp`

- [ ] **Step 1: Run topology characterization tests**

Run:

```bash
ninja -C build test_integration_config test_system
./build/tests/integration/config/test_integration_config --gtest_filter=BootstrapEngineTest.*
./build/tests/system/test_system --gtest_filter=SystemTopologyBootstrapTest.*
```

Expected: selected tests pass before extraction.

- [ ] **Step 2: Add the bootstrapper interface**

Create `include/hpactor/config/topology_bootstrapper.hpp`:

```cpp
#pragma once

#include "hpactor/types/result.hpp"

#include <string>

namespace hpactor {

class ActorSystem;

namespace config {

class TopologyBootstrapper {
public:
    explicit TopologyBootstrapper(ActorSystem& system);
    result<void> load_file(const std::string& toml_path);

private:
    ActorSystem& system_;
};

}  // namespace config
}  // namespace hpactor
```

- [ ] **Step 3: Implement the bootstrapper**

Create `src/config/topology_bootstrapper.cpp`:

```cpp
#include "hpactor/config/topology_bootstrapper.hpp"

#include "hpactor/config/bootstrap_engine.hpp"
#include "hpactor/config/toml_parser.hpp"
#include "hpactor/core/actor_system.hpp"

namespace hpactor::config {

TopologyBootstrapper::TopologyBootstrapper(ActorSystem& system)
    : system_(system) {}

result<void> TopologyBootstrapper::load_file(const std::string& toml_path) {
    TomlParser parser;
    auto model = parser.parse(toml_path);
    if (!model.has_value()) {
        return make_error<void>(model.error());
    }

    BootstrapEngine engine(system_);
    return engine.execute(model.value());
}

}  // namespace hpactor::config
```

Use the exact parser and bootstrap method names from the current `ActorSystem::load_topology()` implementation.

- [ ] **Step 4: Wire `ActorSystem::load_topology()` to the bootstrapper**

In `include/hpactor/core/actor_system.hpp`:

```cpp
std::unique_ptr<config::TopologyBootstrapper> topology_bootstrapper_;
```

In the constructor:

```cpp
topology_bootstrapper_ =
    std::make_unique<config::TopologyBootstrapper>(*this);
```

In `src/actor/actor_system.cpp`:

```cpp
result<void> ActorSystem::load_topology(const std::string& toml_path) {
    return topology_bootstrapper_->load_file(toml_path);
}
```

- [ ] **Step 5: Add source to `src/CMakeLists.txt`**

```cmake
add_library(hpactor_lib SHARED
    config/topology_bootstrapper.cpp
```

Place it near the existing config source files.

- [ ] **Step 6: Run topology tests**

Run:

```bash
ninja -C build test_integration_config test_system
./build/tests/integration/config/test_integration_config --gtest_filter=BootstrapEngineTest.*
./build/tests/system/test_system --gtest_filter=SystemTopologyBootstrapTest.*
```

Expected: selected tests pass.

- [ ] **Step 7: Commit topology extraction**

Run:

```bash
git add include/hpactor/config/topology_bootstrapper.hpp src/config/topology_bootstrapper.cpp src/CMakeLists.txt include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "refactor: extract topology bootstrapper"
```

## Task 9: Extract ShutdownCoordinator

**Files:**
- Create: `include/hpactor/actor/shutdown_coordinator.hpp`
- Create: `src/actor/shutdown_coordinator.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `tests/integration/actor/test_shutdown_coordinator.cpp`
- Modify: `tests/system/test_system_graceful_shutdown.cpp`

- [ ] **Step 1: Run shutdown characterization tests**

Run:

```bash
ninja -C build test_integration_actor test_system
./build/tests/integration/actor/test_integration_actor --gtest_filter=ShutdownCoordinatorTest.*
./build/tests/system/test_system --gtest_filter=GracefulShutdown.*
```

Expected: selected tests pass before extraction.

- [ ] **Step 2: Add the shutdown coordinator interface**

Create `include/hpactor/actor/shutdown_coordinator.hpp`:

```cpp
#pragma once

#include "hpactor/core/actor_system.hpp"
#include "hpactor/types.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

namespace hpactor {

struct ShutdownCoordinatorDependencies {
    std::atomic<ShutdownPhase>* phase{nullptr};
    std::function<void(bool)> set_ready;
    std::function<std::vector<ActorId>()> actor_snapshot;
    std::function<void(ActorId)> request_actor_drain;
    std::function<bool()> actors_drained;
    std::function<void()> stop_remote_runtime;
    std::function<void()> leave_discovery;
    std::function<void()> flush_telemetry;
};

class ShutdownCoordinator {
public:
    explicit ShutdownCoordinator(ShutdownCoordinatorDependencies deps);

    void shutdown(std::chrono::milliseconds drain_timeout);
    ShutdownPhase phase() const noexcept;
    bool accepting_ingress() const noexcept;

private:
    void set_phase(ShutdownPhase phase) noexcept;

    ShutdownCoordinatorDependencies deps_;
};

}  // namespace hpactor
```

If including `actor_system.hpp` creates a cycle, move `ShutdownPhase` to a small header such as `include/hpactor/actor/shutdown_phase.hpp` and include that from both files.

- [ ] **Step 3: Implement shutdown phase transitions**

Create `src/actor/shutdown_coordinator.cpp` and move the existing phase machine from `ActorSystem::shutdown()`:

```cpp
#include "hpactor/actor/shutdown_coordinator.hpp"

namespace hpactor {

ShutdownCoordinator::ShutdownCoordinator(ShutdownCoordinatorDependencies deps)
    : deps_(std::move(deps)) {}

void ShutdownCoordinator::set_phase(ShutdownPhase phase) noexcept {
    deps_.phase->store(phase, std::memory_order_release);
}

ShutdownPhase ShutdownCoordinator::phase() const noexcept {
    return deps_.phase->load(std::memory_order_acquire);
}

bool ShutdownCoordinator::accepting_ingress() const noexcept {
    auto current = phase();
    return current == ShutdownPhase::Running;
}

void ShutdownCoordinator::shutdown(std::chrono::milliseconds drain_timeout) {
    set_phase(ShutdownPhase::DrainingIngress);
    if (deps_.set_ready) {
        deps_.set_ready(false);
    }

    set_phase(ShutdownPhase::DrainingActors);
    for (auto id : deps_.actor_snapshot()) {
        deps_.request_actor_drain(id);
    }

    wait_for_actor_drain(deps_.actors_drained, drain_timeout);

    set_phase(ShutdownPhase::LeavingCluster);
    if (deps_.leave_discovery) {
        deps_.leave_discovery();
    }
    if (deps_.stop_remote_runtime) {
        deps_.stop_remote_runtime();
    }

    set_phase(ShutdownPhase::FlushingTelemetry);
    if (deps_.flush_telemetry) {
        deps_.flush_telemetry();
    }

    set_phase(ShutdownPhase::Stopped);
}

}  // namespace hpactor
```

Move the exact current drain timeout and forced stop behavior from `actor_system.cpp` into this file.

- [ ] **Step 4: Wire `ActorSystem` to the coordinator**

In `include/hpactor/core/actor_system.hpp`:

```cpp
std::unique_ptr<ShutdownCoordinator> shutdown_coordinator_;
```

In the constructor:

```cpp
shutdown_coordinator_ = std::make_unique<ShutdownCoordinator>(
    ShutdownCoordinatorDependencies{
        .phase = &shutdown_phase_,
        .set_ready = [this](bool ready) { readiness_.store(ready); },
        .actor_snapshot = [this] {
            std::vector<ActorId> ids;
            for (const auto& entry : actor_directory_.snapshot()) {
                ids.push_back(entry.actor.id());
            }
            return ids;
        },
        .request_actor_drain = [this](ActorId id) { request_actor_drain(id); },
        .actors_drained = [this] { return actors_drained(); },
        .stop_remote_runtime = [this] { remote_runtime_->stop(); },
        .leave_discovery = [this] { leave_cluster(); },
        .flush_telemetry = [this] { flush_telemetry(); },
    });
```

Reduce facade methods:

```cpp
void ActorSystem::shutdown() {
    shutdown_coordinator_->shutdown(config_.shutdown.drain_timeout);
}

ShutdownPhase ActorSystem::shutdown_phase() const noexcept {
    return shutdown_coordinator_->phase();
}

bool ActorSystem::accepting_ingress() const noexcept {
    return shutdown_coordinator_->accepting_ingress();
}
```

- [ ] **Step 5: Add source to `src/CMakeLists.txt`**

```cmake
add_library(hpactor_lib SHARED
    actor/shutdown_coordinator.cpp
```

Place it near the other actor runtime sources.

- [ ] **Step 6: Run shutdown tests**

Run:

```bash
ninja -C build test_integration_actor test_system
./build/tests/integration/actor/test_integration_actor --gtest_filter=ShutdownCoordinatorTest.*
./build/tests/system/test_system --gtest_filter=GracefulShutdown.*
```

Expected: selected tests pass.

- [ ] **Step 7: Commit shutdown extraction**

Run:

```bash
git add include/hpactor/actor/shutdown_coordinator.hpp src/actor/shutdown_coordinator.cpp src/CMakeLists.txt include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp tests/integration/actor/test_shutdown_coordinator.cpp tests/system/test_system_graceful_shutdown.cpp
git commit -m "refactor: extract shutdown coordinator"
```

## Task 10: Clean Up ActorSystem Facade And Verify

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: tests touched by prior tasks if final names changed during implementation.

- [ ] **Step 1: Remove migrated private state from `ActorSystem`**

Remove state that is fully owned by extracted components:

```cpp
// Remove after migration:
std::unordered_map<ActorId, std::shared_ptr<AbstractActor>> actors_;
std::unordered_map<ActorId, std::unique_ptr<mailbox::MPSCActorMailbox<TypedMessage>>> mailboxes_;
std::unordered_map<ActorId, std::shared_ptr<ActorContext>> actor_contexts_;
ActorId next_actor_id_;
actor_registry registry_;
```

Keep only facade wiring, config, public lifecycle state, and component pointers:

```cpp
ActorDirectory actor_directory_;
std::unique_ptr<LocalDeliveryEngine> local_delivery_;
std::unique_ptr<BackpressureCoordinator> backpressure_;
std::unique_ptr<RemoteRuntime> remote_runtime_;
std::unique_ptr<RemoteSpawnClient> remote_spawn_client_;
std::unique_ptr<config::TopologyBootstrapper> topology_bootstrapper_;
std::unique_ptr<ShutdownCoordinator> shutdown_coordinator_;
```

- [ ] **Step 2: Reduce broad includes**

In `include/hpactor/core/actor_system.hpp`, replace implementation-heavy includes with forward declarations where possible:

```cpp
namespace hpactor {
class BackpressureCoordinator;
class LocalDeliveryEngine;
class RemoteSpawnClient;
class RemoteRuntime;
class ShutdownCoordinator;
namespace config {
class TopologyBootstrapper;
}
}
```

Keep includes required by public method signatures and templates.

- [ ] **Step 3: Run narrow compile checks**

Run:

```bash
ninja -C build hpactor_lib test_unit_actor test_integration_actor test_integration_spawn test_integration_config test_system
```

Expected: all listed targets build.

- [ ] **Step 4: Run targeted regression suite**

Run:

```bash
./build/tests/unit/actor/test_unit_actor --gtest_filter=ActorDirectoryTest.*
./build/tests/integration/actor/test_integration_actor --gtest_filter=ActorSystemTest.*:ActorSystemBackpressureTest.*:BackpressureSignalsTest.*:RemoteBackpressureSignalsTest.*:DeliverySemanticsTest.*:ShutdownCoordinatorTest.*
./build/tests/integration/spawn/test_integration_spawn --gtest_filter=ActorTypeRegistryTest.*:SpawnReceiverTest.*:SpawnIntegrationTest.*
./build/tests/unit/spawn/test_unit_spawn --gtest_filter=SpawnSerializationTest.*
./build/tests/integration/config/test_integration_config --gtest_filter=BootstrapEngineTest.*
./build/tests/system/test_system --gtest_filter=GracefulShutdown.*:SystemTopologyBootstrapTest.*
```

Expected: all selected tests pass.

- [ ] **Step 5: Run full verification before PR**

Run:

```bash
ninja -C build
ctest --test-dir build --output-on-failure
git diff --check
git status --short --branch
```

Expected: build succeeds, CTest succeeds, diff check is clean, and only intentional files are modified before final staging.

- [ ] **Step 6: Commit final cleanup**

Run:

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp src/CMakeLists.txt tests/integration/actor/test_actor_system.cpp tests/integration/actor/test_actor_system_backpressure.cpp tests/integration/actor/test_delivery_semantics.cpp tests/integration/actor/test_backpressure_signals.cpp tests/integration/actor/test_remote_backpressure_signals.cpp tests/integration/actor/test_shutdown_coordinator.cpp tests/integration/spawn/test_actor_type_registry.cpp tests/integration/spawn/test_spawn_receiver.cpp tests/integration/spawn/test_spawn_integration.cpp tests/unit/spawn/test_spawn_serialization.cpp
git commit -m "refactor: slim actor system facade"
```

## Final Review Checklist

- [ ] `ActorSystem::resolve_actor()` returns the registered named actor.
- [ ] Remote spawn APIs pass non-empty args from caller to factory.
- [ ] `ActorSystem` public API signatures remain source-compatible.
- [ ] `ActorSystem::try_deliver_local()` remains the facade for local and remote ingress.
- [ ] Directory, delivery, backpressure, remote, topology, and shutdown components have narrow ownership contracts.
- [ ] Extracted components do not introduce exceptions or RTTI.
- [ ] Dead letters, backpressure metrics, readiness, shutdown phase, and CLI-backed introspection remain observable.
- [ ] The full test suite passes before PR.
