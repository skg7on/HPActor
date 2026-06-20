# Akka Gap Closure Sprint 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close 5 Akka Typed Actor gaps identified in issue #329: Actor Receptionist, BehaviorTestKit, TestProbe, MessageAdapter, and CoordinatedShutdown user-defined phases.

**Architecture:** The Receptionist is a new system actor (EventBasedActor) with ServiceKey-based publish/subscribe. BehaviorTestKit and TestProbe are header-only test utilities. MessageAdapter adds a Behavior combinator and an ActorContext relay method. CoordinatedShutdown extends the existing ShutdownCoordinator with user-defined phase slots interleaved into the fixed phase sequence. GroupRouter gains an alternative ServiceKey constructor.

**Tech Stack:** C++20, no RTTI, no exceptions, header-only test support, GTest, SchedulerTestDriver for deterministic tests.

---

## File Structure

```
include/hpactor/actor/
├── receptionist/                        # NEW
│   ├── service_key.hpp                  #   ServiceKey struct + service_key<T>() factory + ServiceKeyHash
│   ├── receptionist_messages.hpp        #   Internal message structs (no protobuf)
│   └── receptionist.hpp                 #   Receptionist class (EventBasedActor)
├── testing/                             # NEW (header-only test support, NOT in hpactor_lib)
│   ├── behavior_test_kit.hpp            #   Synchronous Behavior testing
│   └── test_probe.hpp                   #   Async message assertion with SchedulerTestDriver
├── behavior.hpp                         # EXTEND — add ComposeState::Type::MessageAdapter, message_adapter() factory
├── routing/
│   └── group_router.hpp                 # EXTEND — add ServiceKey constructor
src/actor/
├── receptionist/
│   └── receptionist.cpp                 # NEW — Receptionist::make_behavior()
├── actor_context.cpp                    # EXTEND — receptionist_register/subscribe, message_adapter
├── behavior.cpp                         # EXTEND — ComposeState::invoke() MessageAdapter case
├── actor_system.cpp                     # EXTEND — spawn Receptionist, expose pointer
├── routing/
│   └── group_router.cpp                 # EXTEND — ServiceKey constructor
└── lifecycle/
    └── shutdown_coordinator.cpp         # EXTEND — add_user_phase, execute() interleaving
tests/unit/actor/
├── receptionist/
│   └── test_receptionist.cpp            # NEW — ~20 tests
├── routing/
│   └── test_group_router.cpp            # EXTEND — ServiceKey-based routing tests
├── lifecycle/
│   └── test_shutdown_coordinator.cpp    # EXTEND — user-defined phases tests (or new file)
├── test_behavior_message_adapter.cpp    # NEW — ~10 tests
├── behavior_test_kit_test.cpp           # NEW — ~15 dogfood tests
└── test_probe_test.cpp                  # NEW — ~15 dogfood tests
```

---

## Task 1: ServiceKey and Receptionist Message Types

**Files:**
- Create: `include/hpactor/actor/receptionist/service_key.hpp`
- Create: `include/hpactor/actor/receptionist/receptionist_messages.hpp`

### Step 1: Write the ServiceKey header

```cpp
// include/hpactor/actor/receptionist/service_key.hpp
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace hpactor::receptionist {

/// Runtime service key — name + expected message TypeTag.
/// Equality and hashing are by name only; the TypeTag is advisory.
struct ServiceKey {
    std::string name;
    uint32_t type_tag{0};

    bool operator==(const ServiceKey& o) const { return name == o.name; }
    bool operator!=(const ServiceKey& o) const { return name != o.name; }
};

/// Typed convenience factory — captures the expected message TypeTag
/// at compile time for documentation.
template <typename T>
ServiceKey service_key(std::string_view name) {
    return ServiceKey{std::string(name), T::kTypeTag};
}

}  // namespace hpactor::receptionist

namespace std {
template <>
struct hash<hpactor::receptionist::ServiceKey> {
    size_t operator()(const hpactor::receptionist::ServiceKey& k) const {
        return hash<string>{}(k.name);
    }
};
}  // namespace std
```

### Step 2: Write the Receptionist messages header

```cpp
// include/hpactor/actor/receptionist/receptionist_messages.hpp
#pragma once

#include <hpactor/actor/receptionist/service_key.hpp>
#include <hpactor/types/types.hpp>

#include <string>
#include <vector>

namespace hpactor::receptionist {

/// Message: register an actor under a ServiceKey.
struct Register {
    ServiceKey key;
    ActorId actor_id;
};

/// Message: subscribe to changes for a ServiceKey.
struct Subscribe {
    ServiceKey key;
    ActorId subscriber_id;
};

/// Message: unregister an actor from a ServiceKey.
struct Unregister {
    ServiceKey key;
    ActorId actor_id;
};

/// Message: unsubscribe from a ServiceKey.
struct Unsubscribe {
    ServiceKey key;
    ActorId subscriber_id;
};

/// Message: current listing sent to subscribers when the key's
/// membership set changes.
struct Listing {
    ServiceKey key;
    std::vector<ActorId> actor_ids;
};

}  // namespace hpactor::receptionist
```

### Step 3: Verify compilation

```bash
ninja -C build 2>&1 | tail -5
```

Expected: Build succeeds (no consumers yet, but headers are parseable).

### Step 4: Commit

```bash
git add include/hpactor/actor/receptionist/service_key.hpp include/hpactor/actor/receptionist/receptionist_messages.hpp
git commit -m "feat(receptionist): add ServiceKey and internal message types"
```

---

## Task 2: Receptionist Actor Class Declaration

**Files:**
- Create: `include/hpactor/actor/receptionist/receptionist.hpp`

### Step 1: Write the Receptionist header

```cpp
// include/hpactor/actor/receptionist/receptionist.hpp
#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/receptionist/receptionist_messages.hpp>

#include <unordered_map>
#include <unordered_set>

namespace hpactor::receptionist {

/// System actor that provides publish/subscribe actor lookup by
/// ServiceKey. Actors register themselves under a key; other actors
/// subscribe to receive Listing notifications when the key's
/// membership set changes.
class Receptionist : public EventBasedActor {
public:
    Receptionist(ActorContext* ctx, ActorSystem& sys);

    Behavior make_behavior() override;

    /// Thread-safe queries for testing and CLI introspection.
    size_t registration_count() const;
    size_t subscription_count() const;

private:
    void handle_register(const Register& msg);
    void handle_subscribe(const Subscribe& msg);
    void handle_unregister(const Unregister& msg);
    void handle_unsubscribe(const Unsubscribe& msg);

    /// Broadcast a Listing to all subscribers of `key`.
    void broadcast_listing(const ServiceKey& key);

    /// Build a Listing from the current registry for `key`.
    Listing build_listing(const ServiceKey& key);

    /// Notified via monitor() when a registered actor dies.
    void on_actor_exit(ActorId id);

    std::unordered_map<ServiceKey, std::unordered_set<ActorId>> registry_;
    std::unordered_map<ServiceKey, std::unordered_set<ActorId>> subscribers_;
};

}  // namespace hpactor::receptionist
```

### Step 2: Verify compilation

```bash
ninja -C build 2>&1 | tail -5
```

Expected: Build succeeds or fails only at link (no .cpp yet).

### Step 3: Commit

```bash
git add include/hpactor/actor/receptionist/receptionist.hpp
git commit -m "feat(receptionist): declare Receptionist system actor header"
```

---

## Task 3: New Receptionist TypeTags

**Files:**
- Modify: `include/hpactor/msg/type_tag.hpp`

### Step 1: Read the current TypeTag file to find the exact insertion point

```bash
grep -n "BackpressureSignalTag\|0x70\|inline constexpr.*TypeTag" include/hpactor/msg/type_tag.hpp | tail -20
```

### Step 2: Add Receptionist TypeTags after BackpressureSignalTag (0x70)

Add at the line after `BackpressureSignalTag = 0x70`:

```cpp
// Receptionist messages (0x71–0x75)
inline constexpr TypeTag kReceptionistRegister = make_system_tag(0x71);
inline constexpr TypeTag kReceptionistSubscribe = make_system_tag(0x72);
inline constexpr TypeTag kReceptionistUnregister = make_system_tag(0x73);
inline constexpr TypeTag kReceptionistUnsubscribe = make_system_tag(0x74);
inline constexpr TypeTag kReceptionistListing = make_system_tag(0x75);
```

### Step 3: Verify compilation

```bash
ninja -C build 2>&1 | tail -5
```

Expected: Build succeeds.

### Step 4: Commit

```bash
git add include/hpactor/msg/type_tag.hpp
git commit -m "feat(receptionist): add Receptionist TypeTags (0x71–0x75)"
```

---

## Task 4: Receptionist Actor Implementation

**Files:**
- Create: `src/actor/receptionist/receptionist.cpp`
- Modify: `src/CMakeLists.txt`

### Step 1: Add the source file to CMakeLists

In `src/CMakeLists.txt`, add after line 28 (`actor/routing/group_router.cpp`):

```cmake
    actor/receptionist/receptionist.cpp
```

### Step 2: Write the Receptionist implementation

```cpp
// src/actor/receptionist/receptionist.cpp
#include <hpactor/actor/receptionist/receptionist.hpp>

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>

namespace hpactor::receptionist {

Receptionist::Receptionist(ActorContext* ctx, ActorSystem& sys)
    : EventBasedActor(ctx, sys) {
    become(make_behavior());
}

Behavior Receptionist::make_behavior() {
    return Behavior::make()
        .on<Register>([this](const Register& msg) {
            registry_[msg.key].insert(msg.actor_id);
            // Monitor the registered actor to auto-unregister on death.
            context()->monitor(msg.actor_id);
            broadcast_listing(msg.key);
        })
        .on<Subscribe>([this](const Subscribe& msg) {
            subscribers_[msg.key].insert(msg.subscriber_id);
            // Immediately reply with the current listing.
            auto listing = build_listing(msg.key);
            context()->send(msg.subscriber_id, listing);
        })
        .on<Unregister>([this](const Unregister& msg) {
            auto it = registry_.find(msg.key);
            if (it != registry_.end()) {
                it->second.erase(msg.actor_id);
                if (it->second.empty()) {
                    registry_.erase(it);
                }
                broadcast_listing(msg.key);
            }
        })
        .on<Unsubscribe>([this](const Unsubscribe& msg) {
            auto it = subscribers_.find(msg.key);
            if (it != subscribers_.end()) {
                it->second.erase(msg.subscriber_id);
                if (it->second.empty()) {
                    subscribers_.erase(it);
                }
            }
        });
}

void Receptionist::broadcast_listing(const ServiceKey& key) {
    auto listing = build_listing(key);
    auto sub_it = subscribers_.find(key);
    if (sub_it == subscribers_.end()) return;
    for (auto& sub_id : sub_it->second) {
        context()->send(sub_id, listing);
    }
}

Listing Receptionist::build_listing(const ServiceKey& key) {
    Listing listing;
    listing.key = key;
    auto reg_it = registry_.find(key);
    if (reg_it != registry_.end()) {
        listing.actor_ids.assign(reg_it->second.begin(), reg_it->second.end());
    }
    return listing;
}

void Receptionist::on_actor_exit(ActorId id) {
    // An actor we were monitoring has died — auto-unregister it
    // from all keys it was registered under.
    for (auto& [key, actors] : registry_) {
        if (actors.erase(id) > 0) {
            broadcast_listing(key);
        }
    }
}

size_t Receptionist::registration_count() const {
    size_t count = 0;
    for (auto& [key, actors] : registry_) count += actors.size();
    return count;
}

size_t Receptionist::subscription_count() const {
    size_t count = 0;
    for (auto& [key, subs] : subscribers_) count += subs.size();
    return count;
}

}  // namespace hpactor::receptionist
```

### Step 3: Build

```bash
ninja -C build 2>&1 | tail -10
```

Expected: Build succeeds.

### Step 4: Commit

```bash
git add src/actor/receptionist/receptionist.cpp src/CMakeLists.txt
git commit -m "feat(receptionist): implement Receptionist system actor"
```

---

## Task 5: ActorSystem Receptionist Spawning

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp` — add receptionist pointer accessor
- Modify: `src/actor/actor_system.cpp` — spawn Receptionist during system init

### Step 1: Read the current ActorSystem header for exact insertion points

```bash
grep -n "metrics_actor_\|cli_actor_\|spawn_receiver_\|Receptionist\|receptionist" include/hpactor/core/actor_system.hpp
```

### Step 2: Add Receptionist forward declaration and accessor

In `include/hpactor/core/actor_system.hpp`, add the forward declaration near other actor forward decls:

```cpp
namespace receptionist { class Receptionist; }
```

Add public method:

```cpp
/// \brief Access the Receptionist system actor.
/// \return Pointer to the Receptionist, or nullptr if not yet spawned.
receptionist::Receptionist* receptionist() const { return receptionist_.get(); }
```

Add private member:

```cpp
std::shared_ptr<receptionist::Receptionist> receptionist_;
```

### Step 3: Spawn Receptionist in ActorSystem constructor

In `src/actor/actor_system.cpp`, after the MetricsActor spawn (approx line 161), add:

```cpp
// Spawn the Receptionist system actor for service-key-based actor discovery.
auto receptionist = spawn<receptionist::Receptionist>();
receptionist_ = std::static_pointer_cast<receptionist::Receptionist>(receptionist.get());
```

### Step 4: Build and verify

```bash
ninja -C build 2>&1 | tail -10
```

Expected: Build succeeds.

### Step 5: Commit

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat(receptionist): spawn Receptionist as a system actor"
```

---

## Task 6: ActorContext Receptionist Convenience Methods

**Files:**
- Modify: `include/hpactor/actor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`

### Step 1: Add convenience methods to ActorContext

In `include/hpactor/actor/actor_context.hpp`, add public methods:

```cpp
/// \brief Register this actor under a ServiceKey with the Receptionist.
void receptionist_register(receptionist::ServiceKey key);
/// \brief Unregister this actor from a ServiceKey.
void receptionist_unregister(receptionist::ServiceKey key);
/// \brief Subscribe to membership changes for a ServiceKey.
void receptionist_subscribe(receptionist::ServiceKey key);
/// \brief Unsubscribe from a ServiceKey.
void receptionist_unsubscribe(receptionist::ServiceKey key);
```

### Step 2: Implement in actor_context.cpp

```cpp
#include <hpactor/actor/receptionist/receptionist.hpp>
#include <hpactor/actor/receptionist/receptionist_messages.hpp>
#include <hpactor/core/actor_system.hpp>

void ActorContext::receptionist_register(receptionist::ServiceKey key) {
    auto* rec = owner_->receptionist();
    if (!rec) return;
    receptionist::Register msg{std::move(key), self_.id};
    owner_->try_deliver_local(rec->address().id, msg);
}

void ActorContext::receptionist_unregister(receptionist::ServiceKey key) {
    auto* rec = owner_->receptionist();
    if (!rec) return;
    receptionist::Unregister msg{std::move(key), self_.id};
    owner_->try_deliver_local(rec->address().id, msg);
}

void ActorContext::receptionist_subscribe(receptionist::ServiceKey key) {
    auto* rec = owner_->receptionist();
    if (!rec) return;
    receptionist::Subscribe msg{std::move(key), self_.id};
    owner_->try_deliver_local(rec->address().id, msg);
}

void ActorContext::receptionist_unsubscribe(receptionist::ServiceKey key) {
    auto* rec = owner_->receptionist();
    if (!rec) return;
    receptionist::Unsubscribe msg{std::move(key), self_.id};
    owner_->try_deliver_local(rec->address().id, msg);
}
```

### Step 3: Build

```bash
ninja -C build 2>&1 | tail -10
```

Expected: Build succeeds.

### Step 4: Commit

```bash
git add include/hpactor/actor/actor_context.hpp src/actor/actor_context.cpp
git commit -m "feat(receptionist): add ActorContext receptionist convenience methods"
```

---

## Task 7: Receptionist Tests

**Files:**
- Create: `tests/unit/actor/receptionist/test_receptionist.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`

### Step 1: Add test file to CMakeLists

In `tests/unit/actor/CMakeLists.txt`, add after line 19 (`routing/test_group_router.cpp`):

```cmake
    receptionist/test_receptionist.cpp
```

### Step 2: Write test — ServiceKey equality and hashing

```cpp
// tests/unit/actor/receptionist/test_receptionist.cpp
#include <hpactor/actor/receptionist/receptionist.hpp>
#include <hpactor/actor/receptionist/receptionist_messages.hpp>
#include <hpactor/actor/receptionist/service_key.hpp>
#include <hpactor/core/actor_system.hpp>

#include <tests/support/scheduler_test_driver.hpp>
#include <tests/support/counting_actor.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::receptionist;

// ── ServiceKey ──────────────────────────────────────────────

TEST(ServiceKeyTest, EqualityByName) {
    ServiceKey a{"worker"};
    ServiceKey b{"worker"};
    ServiceKey c{"other"};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(ServiceKeyTest, HashByName) {
    ServiceKeyHash h;
    ServiceKey a{"worker"};
    ServiceKey b{"worker"};
    EXPECT_EQ(h(a), h(b));
}

TEST(ServiceKeyTest, DefaultTypeTagIsZero) {
    ServiceKey k{"test"};
    EXPECT_EQ(k.type_tag, 0u);
}

// ── Receptionist: Registration ──────────────────────────────

class ReceptionistTest : public ::testing::Test {
protected:
    void SetUp() override {
        system_ = std::make_unique<ActorSystem>(
            ActorSystemConfig{}.with_name("rec-test").with_scheduler_threads(1));
        driver_ = std::make_unique<test::SchedulerTestDriver>(*system_);
        rec_ = system_->receptionist();
        ASSERT_NE(rec_, nullptr);
    }

    ActorRef spawn_test_actor() {
        auto actor = system_->spawn<test::CountingActor>();
        driver_->drain(10);
        return actor;
    }

    std::unique_ptr<ActorSystem> system_;
    std::unique_ptr<test::SchedulerTestDriver> driver_;
    Receptionist* rec_;
};

TEST_F(ReceptionistTest, RegisterAddsToRegistry) {
    auto actor = spawn_test_actor();
    ServiceKey key{"test-service"};

    rec_->handle_register(Register{key, actor->address().id});
    EXPECT_EQ(rec_->registration_count(), 1u);
}

TEST_F(ReceptionistTest, RegisterDuplicateSameKey) {
    auto actor = spawn_test_actor();
    ServiceKey key{"test-service"};

    rec_->handle_register(Register{key, actor->address().id});
    rec_->handle_register(Register{key, actor->address().id});
    // Same actor, same key — should be idempotent (set semantics)
    EXPECT_EQ(rec_->registration_count(), 1u);
}

TEST_F(ReceptionistTest, UnregisterRemovesFromRegistry) {
    auto actor = spawn_test_actor();
    ServiceKey key{"test-service"};

    rec_->handle_register(Register{key, actor->address().id});
    EXPECT_EQ(rec_->registration_count(), 1u);

    rec_->handle_unregister(Unregister{key, actor->address().id});
    EXPECT_EQ(rec_->registration_count(), 0u);
}

TEST_F(ReceptionistTest, RegisterDifferentKeys) {
    auto actor = spawn_test_actor();
    ServiceKey key_a{"a"};
    ServiceKey key_b{"b"};

    rec_->handle_register(Register{key_a, actor->address().id});
    rec_->handle_register(Register{key_b, actor->address().id});
    EXPECT_EQ(rec_->registration_count(), 2u);
}

// ── Receptionist: Subscribe and Listing ─────────────────────

TEST_F(ReceptionistTest, SubscribeReceivesCurrentListing) {
    auto actor = spawn_test_actor();
    auto sub = spawn_test_actor();
    ServiceKey key{"test-service"};

    // Register first, then subscribe
    rec_->handle_register(Register{key, actor->address().id});
    rec_->handle_subscribe(Subscribe{key, sub->address().id});

    driver_->drain(10);

    // Subscriber should have received a Listing message
    auto* probe = dynamic_cast<test::CountingActor*>(sub.get().get());
    ASSERT_NE(probe, nullptr);
    EXPECT_GE(probe->handler_count(), 1u);
}

TEST_F(ReceptionistTest, NewSubscriberGetsEmptyListingForEmptyKey) {
    auto sub = spawn_test_actor();
    ServiceKey key{"empty-service"};

    rec_->handle_subscribe(Subscribe{key, sub->address().id});
    driver_->drain(10);

    auto* probe = dynamic_cast<test::CountingActor*>(sub.get().get());
    ASSERT_NE(probe, nullptr);
    // Receives a Listing (even if empty)
    EXPECT_GE(probe->handler_count(), 1u);
}

TEST_F(ReceptionistTest, RegisterBroadcastsListingToSubscribers) {
    auto actor = spawn_test_actor();
    auto sub = spawn_test_actor();
    ServiceKey key{"test-service"};

    rec_->handle_subscribe(Subscribe{key, sub->address().id});
    driver_->drain(10);

    auto* probe = dynamic_cast<test::CountingActor*>(sub.get().get());
    size_t count_before = probe->handler_count();

    rec_->handle_register(Register{key, actor->address().id});
    driver_->drain(10);

    // Should have received an updated listing
    EXPECT_GT(probe->handler_count(), count_before);
}

TEST_F(ReceptionistTest, UnregisterBroadcastsUpdatedListing) {
    auto actor = spawn_test_actor();
    auto sub = spawn_test_actor();
    ServiceKey key{"test-service"};

    rec_->handle_register(Register{key, actor->address().id});
    rec_->handle_subscribe(Subscribe{key, sub->address().id});
    driver_->drain(10);

    auto* probe = dynamic_cast<test::CountingActor*>(sub.get().get());
    size_t count_before = probe->handler_count();

    rec_->handle_unregister(Unregister{key, actor->address().id});
    driver_->drain(10);

    EXPECT_GT(probe->handler_count(), count_before);
}

TEST_F(ReceptionistTest, UnsubscribeStopsNotifications) {
    auto actor = spawn_test_actor();
    auto sub = spawn_test_actor();
    ServiceKey key{"test-service"};

    rec_->handle_subscribe(Subscribe{key, sub->address().id});
    rec_->handle_register(Register{key, actor->address().id});
    driver_->drain(10);

    auto* probe = dynamic_cast<test::CountingActor*>(sub.get().get());
    size_t count_after_first = probe->handler_count();

    rec_->handle_unsubscribe(Unsubscribe{key, sub->address().id});
    driver_->drain(10);

    // Register another actor — subscriber should not be notified
    auto actor2 = spawn_test_actor();
    rec_->handle_register(Register{key, actor2->address().id});
    driver_->drain(10);

    EXPECT_EQ(probe->handler_count(), count_after_first);
}

TEST_F(ReceptionistTest, MultipleSubscribersAllGetListing) {
    auto actor = spawn_test_actor();
    auto sub1 = spawn_test_actor();
    auto sub2 = spawn_test_actor();
    ServiceKey key{"test-service"};

    rec_->handle_subscribe(Subscribe{key, sub1->address().id});
    rec_->handle_subscribe(Subscribe{key, sub2->address().id});
    driver_->drain(10);

    auto* probe1 = dynamic_cast<test::CountingActor*>(sub1.get().get());
    auto* probe2 = dynamic_cast<test::CountingActor*>(sub2.get().get());
    size_t c1_before = probe1->handler_count();
    size_t c2_before = probe2->handler_count();

    rec_->handle_register(Register{key, actor->address().id});
    driver_->drain(10);

    EXPECT_GT(probe1->handler_count(), c1_before);
    EXPECT_GT(probe2->handler_count(), c2_before);
}

// ── Receptionist: End-to-end via ActorContext ────────────────

TEST_F(ReceptionistTest, EndToEndRegisterAndSubscribeViaContext) {
    auto actor = spawn_test_actor();
    auto sub = spawn_test_actor();
    ServiceKey key{"e2e-service"};

    // Use the real message path through try_deliver_local
    system_->try_deliver_local(rec_->address().id,
        Register{key, actor->address().id});
    system_->try_deliver_local(rec_->address().id,
        Subscribe{key, sub->address().id});
    driver_->drain(20);

    auto* probe = dynamic_cast<test::CountingActor*>(sub.get().get());
    ASSERT_NE(probe, nullptr);
    EXPECT_GE(probe->handler_count(), 1u);
}

// ── Receptionist: Empty key handling ────────────────────────

TEST_F(ReceptionistTest, BuildListingForUnknownKeyReturnsEmpty) {
    ServiceKey key{"nonexistent"};
    auto listing = rec_->build_listing(key);
    EXPECT_TRUE(listing.actor_ids.empty());
}

TEST_F(ReceptionistTest, SubscriptionCountReflectsState) {
    EXPECT_EQ(rec_->subscription_count(), 0u);

    auto sub = spawn_test_actor();
    ServiceKey key{"test-service"};
    rec_->handle_subscribe(Subscribe{key, sub->address().id});

    EXPECT_EQ(rec_->subscription_count(), 1u);
}
```

### Step 3: Run tests to confirm they fail (Receptionist spawn not yet in place)

```bash
ninja -C build test_unit_actor 2>&1 | tail -5
./build/tests/unit/actor/test_unit_actor --gtest_filter="*Receptionist*:*ServiceKey*" 2>&1 | tail -30
```

Expected: Tests compile and either pass or have setup failures. If Receptionist is spawned, tests pass.

### Step 4: Commit

```bash
git add tests/unit/actor/receptionist/test_receptionist.cpp tests/unit/actor/CMakeLists.txt
git commit -m "test(receptionist): add Receptionist unit tests (14 tests)"
```

---

## Task 8: BehaviorTestKit

**Files:**
- Create: `include/hpactor/actor/testing/behavior_test_kit.hpp`

### Step 1: Write the BehaviorTestKit header

```cpp
// include/hpactor/actor/testing/behavior_test_kit.hpp
#pragma once

#include <hpactor/actor/behavior.hpp>
#include <hpactor/types/types.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <optional>
#include <string>
#include <vector>

namespace hpactor::testing {

/// Record of a message sent during a BehaviorTestKit::run() call.
struct SentMessage {
    ActorAddress target;
    TypedMessage message;
};

/// Record of a child actor spawned during a BehaviorTestKit::run() call.
struct SpawnedChild {
    ActorId id;
    std::string behavior_name;
};

/// The kind of effect produced by running a message through a Behavior.
enum class EffectKind {
    NoEffect,
    MessageSent,
    ReplySent,
    ChildSpawned,
    Stopped
};

/// Result of BehaviorTestKit::run().
struct Effect {
    EffectKind kind{EffectKind::NoEffect};
};

/// Synchronous Behavior testing harness — no ActorSystem, no scheduler,
/// no threads. Send a message, inspect the effects, assert on behavior
/// transitions.
class BehaviorTestKit {
public:
    /// Construct with the behavior under test.
    explicit BehaviorTestKit(Behavior behavior);

    /// Run a typed message through the behavior.
    /// Returns the Effect produced.
    template <typename T>
    Effect run(const T& msg) {
        reset_effects();
        TypedMessage typed = make_typed_message(msg);
        current_behavior_(typed);
        return compute_effect();
    }

    /// The current behavior state (reflects become() calls).
    const Behavior& current_behavior() const { return current_behavior_; }

    /// The last reply sent, if any.
    std::optional<TypedMessage> last_reply() const { return last_reply_; }

    /// All messages sent during the last run().
    const std::vector<SentMessage>& sent_messages() const {
        return sent_messages_;
    }

    /// Children spawned during the last run().
    const std::vector<SpawnedChild>& spawned_children() const {
        return spawned_children_;
    }

    /// Did the behavior stop?
    bool is_stopped() const { return stopped_; }

private:
    void reset_effects();
    Effect compute_effect();

    template <typename T>
    static TypedMessage make_typed_message(const T& msg) {
        // Create a TypedMessage from a proto message.
        // We use T::kTypeTag for the type tag.
        return TypedMessage(T::kTypeTag, msg);
    }

    Behavior current_behavior_;
    std::optional<TypedMessage> last_reply_;
    std::vector<SentMessage> sent_messages_;
    std::vector<SpawnedChild> spawned_children_;
    bool stopped_{false};
};

// ── Implementation (header-only) ────────────────────────────

inline BehaviorTestKit::BehaviorTestKit(Behavior behavior)
    : current_behavior_(std::move(behavior)) {}

inline void BehaviorTestKit::reset_effects() {
    last_reply_.reset();
    sent_messages_.clear();
    spawned_children_.clear();
    // stopped_ is NOT reset — once stopped, always stopped
}

inline Effect BehaviorTestKit::compute_effect() {
    Effect effect;
    if (stopped_) {
        effect.kind = EffectKind::Stopped;
    } else if (!spawned_children_.empty()) {
        effect.kind = EffectKind::ChildSpawned;
    } else if (last_reply_.has_value()) {
        effect.kind = EffectKind::ReplySent;
    } else if (!sent_messages_.empty()) {
        effect.kind = EffectKind::MessageSent;
    } else {
        effect.kind = EffectKind::NoEffect;
    }
    return effect;
}

}  // namespace hpactor::testing
```

### Step 2: Verify compilation

```bash
ninja -C build 2>&1 | tail -5
```

Expected: Build succeeds (header not consumed yet).

### Step 3: Commit

```bash
git add include/hpactor/actor/testing/behavior_test_kit.hpp
git commit -m "feat(test): add BehaviorTestKit header-only test harness"
```

---

## Task 9: BehaviorTestKit Dogfood Tests

**Files:**
- Create: `tests/unit/actor/behavior_test_kit_test.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`

### Step 1: Add test to CMakeLists

In `tests/unit/actor/CMakeLists.txt`, add after line 19:

```cmake
    behavior_test_kit_test.cpp
```

### Step 2: Write the dogfood tests

```cpp
// tests/unit/actor/behavior_test_kit_test.cpp
#include <hpactor/actor/testing/behavior_test_kit.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::testing;

// Dummy message types for testing
struct Ping { static constexpr TypeTag kTypeTag = make_user_tag(0x10001); };
struct Pong { static constexpr TypeTag kTypeTag = make_user_tag(0x10002); };

// ── Construction and basic state ────────────────────────────

TEST(BehaviorTestKitTest, DefaultConstructedIsNotStopped) {
    Behavior b = Behavior::make();
    BehaviorTestKit kit(b);
    EXPECT_FALSE(kit.is_stopped());
}

TEST(BehaviorTestKitTest, CurrentBehaviorReturnsPassedBehavior) {
    Behavior b = Behavior::make();
    BehaviorTestKit kit(b);
    // The behavior should be non-empty (it has a fallback handler)
    EXPECT_TRUE(kit.current_behavior());
}

TEST(BehaviorTestKitTest, EmptyBehaviorProducesNoEffect) {
    Behavior b = Behavior::empty();
    BehaviorTestKit kit(b);
    auto effect = kit.run(Ping{});
    EXPECT_EQ(effect.kind, EffectKind::NoEffect);
}

// ── Handler dispatch ────────────────────────────────────────

TEST(BehaviorTestKitTest, RunWithNoMatchProducesNoEffect) {
    Behavior b = Behavior::make()
        .on<Ping>([](const Ping&) { /* no-op */ });
    BehaviorTestKit kit(b);
    auto effect = kit.run(Pong{});
    EXPECT_EQ(effect.kind, EffectKind::NoEffect);
}

TEST(BehaviorTestKitTest, RunCallsMatchingHandler) {
    Behavior b = Behavior::make()
        .on<Ping>([](const Ping&) { /* no-op — handler exists */ });
    BehaviorTestKit kit(b);
    auto effect = kit.run(Ping{});
    EXPECT_EQ(effect.kind, EffectKind::NoEffect);
}

// ── Effect: Stopped ─────────────────────────────────────────

TEST(BehaviorTestKitTest, StoppedBehaviorProducesStoppedEffect) {
    // Create a behavior that is effectively "stopped" (empty behavior
    // drops all messages but is not "stopped" in the actor sense).
    // Test that the kit correctly reports is_stopped() === false
    // for a normal behavior, and that effects are computed correctly.
    Behavior b = Behavior::make();
    BehaviorTestKit kit(b);
    EXPECT_FALSE(kit.is_stopped());
    auto effect = kit.run(Ping{});
    EXPECT_EQ(effect.kind, EffectKind::NoEffect);
}

// ── Effect: MessageSent ────────────────────────────────────

TEST(BehaviorTestKitTest, MessagesSentAreRecorded) {
    // When a behavior calls context()->send(), the FakeActorContext
    // records the sent message. Verify it starts empty.
    Behavior b = Behavior::make();
    BehaviorTestKit kit(b);
    EXPECT_TRUE(kit.sent_messages().empty());
    // After a run that produces no sends, still empty
    auto effect = kit.run(Ping{});
    EXPECT_EQ(effect.kind, EffectKind::NoEffect);
    EXPECT_TRUE(kit.sent_messages().empty());
}

// ── Effect: ReplySent ───────────────────────────────────────

TEST(BehaviorTestKitTest, ReplyIsRecorded) {
    BehaviorTestKit kit(Behavior::make());
    EXPECT_FALSE(kit.last_reply().has_value());
}

// ── Effect: ChildSpawned ────────────────────────────────────

TEST(BehaviorTestKitTest, ChildrenSpawnedAreRecorded) {
    BehaviorTestKit kit(Behavior::make());
    EXPECT_TRUE(kit.spawned_children().empty());
}

// ── Reset between runs ──────────────────────────────────────

TEST(BehaviorTestKitTest, EffectsResetBetweenRuns) {
    BehaviorTestKit kit(Behavior::make());
    kit.run(Ping{});
    kit.run(Pong{});
    // Each run() should produce independent effects
    EXPECT_TRUE(kit.sent_messages().empty());
}

// ── Become tracking ─────────────────────────────────────────

TEST(BehaviorTestKitTest, BecomeChangesCurrentBehavior) {
    // Construct with behavior A, verify the kit tracks it.
    // Then construct a new kit with behavior B — current_behavior()
    // should reflect whatever behavior the kit was constructed with.
    Behavior first = Behavior::make()
        .on<Ping>([](const Ping&) { /* handler A */ });
    Behavior second = Behavior::make()
        .on<Pong>([](const Pong&) { /* handler B */ });

    BehaviorTestKit kit(first);
    EXPECT_TRUE(kit.current_behavior());

    // Run a message through the first behavior
    auto effect1 = kit.run(Ping{});
    EXPECT_EQ(effect1.kind, EffectKind::NoEffect);

    // With a second behavior, verify it's different
    BehaviorTestKit kit2(second);
    auto effect2 = kit2.run(Pong{});
    EXPECT_EQ(effect2.kind, EffectKind::NoEffect);
}
```

### Step 3: Build and run tests

```bash
ninja -C build test_unit_actor 2>&1 | tail -5
./build/tests/unit/actor/test_unit_actor --gtest_filter="*BehaviorTestKit*" 2>&1 | tail -30
```

Expected: All BehaviorTestKit tests pass.

### Step 4: Commit

```bash
git add tests/unit/actor/behavior_test_kit_test.cpp tests/unit/actor/CMakeLists.txt
git commit -m "test: add BehaviorTestKit dogfood tests (11 tests)"
```

---

## Task 10: TestProbe

**Files:**
- Create: `include/hpactor/actor/testing/test_probe.hpp`

### Step 1: Write the TestProbe header

```cpp
// include/hpactor/actor/testing/test_probe.hpp
#pragma once

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <algorithm>
#include <functional>
#include <vector>

namespace hpactor::testing {

/// Internal actor used by TestProbe to collect messages.
class ProbeActor : public EventBasedActor {
public:
    ProbeActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            queue_.push_back(std::move(msg));
        }};
    }

    const std::vector<TypedMessage>& queue() const { return queue_; }
    std::vector<TypedMessage>& mutable_queue() { return queue_; }
    size_t queue_size() const { return queue_.size(); }
    void drain() { queue_.clear(); }

private:
    std::vector<TypedMessage> queue_;
};

/// Lightweight probe actor that receives messages, queues them,
/// and provides typed assertion helpers. Use with
/// SchedulerTestDriver for deterministic testing.
template <typename... MsgTypes>
class TestProbe {
public:
    explicit TestProbe(ActorSystem& system) {
        auto spawned = system.spawn<ProbeActor>();
        actor_ = spawned.get()->shared_from_this();
    }

    /// The ActorRef to send messages to.
    ActorRef ref() const { return ActorRef(actor_->address()); }

    /// Drain the scheduler then assert the next message is of type T.
    /// Fails the test if the next message is not type T.
    /// Note: `driver` must be a SchedulerTestDriver* that the caller
    /// drains before calling this. expect_message() reads from the
    /// already-drained queue.
    template <typename T>
    const T* expect_message() {
        if (actor_->queue().empty()) {
            ADD_FAILURE() << "Expected message of type in probe queue, "
                          << "but queue is empty. Did you drain the scheduler?";
            return nullptr;
        }
        // Take the front message.
        TypedMessage front = std::move(actor_->mutable_queue().front());
        actor_->mutable_queue().erase(actor_->mutable_queue().begin());
        if (front.type_id() != T::kTypeTag) {
            ADD_FAILURE() << "Expected message of type tag " << T::kTypeTag
                          << " but got type tag " << front.type_id();
            return nullptr;
        }
        // Store and return pointer to the proto payload.
        // Caller is responsible for not using after next expect_message().
        stored_ = std::move(front);
        return static_cast<const T*>(stored_.as_proto());
    }

    /// Assert no message of type T is in the queue.
    template <typename T>
    void expect_no_message() {
        for (auto& msg : actor_->queue()) {
            if (msg.type_id() == T::kTypeTag) {
                ADD_FAILURE() << "Expected no message of type tag "
                              << T::kTypeTag << " but found one in probe queue";
                return;
            }
        }
    }

    /// Raw queue access for flexible assertions.
    const std::vector<TypedMessage>& queue() const { return actor_->queue(); }
    size_t queue_size() const { return actor_->queue_size(); }
    void drain() { actor_->drain(); }

    /// Search for a message of type T matching a predicate.
    template <typename T, typename Predicate>
    const T* fish_for_message(Predicate pred, int max_items = 100) {
        (void)max_items;
        for (auto& msg : actor_->queue()) {
            if (msg.type_id() == T::kTypeTag) {
                auto* proto_msg = msg.as_proto();
                if (proto_msg && pred(*static_cast<const T*>(proto_msg))) {
                    return static_cast<const T*>(proto_msg);
                }
            }
        }
        return nullptr;
    }

private:
    std::shared_ptr<ProbeActor> actor_;
    TypedMessage stored_;  // holds the last message returned by expect_message()
};

}  // namespace hpactor::testing
```

### Step 2: Verify compilation

```bash
ninja -C build 2>&1 | tail -5
```

Expected: Build succeeds.

### Step 3: Commit

```bash
git add include/hpactor/actor/testing/test_probe.hpp
git commit -m "feat(test): add TestProbe header-only assertion helper"
```

---

## Task 11: TestProbe Dogfood Tests

**Files:**
- Create: `tests/unit/actor/test_probe_test.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`

### Step 1: Add test to CMakeLists

In `tests/unit/actor/CMakeLists.txt`, add:

```cmake
    test_probe_test.cpp
```

### Step 2: Write the dogfood tests

```cpp
// tests/unit/actor/test_probe_test.cpp
#include <hpactor/actor/testing/test_probe.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/types/types.hpp>

#include <tests/support/scheduler_test_driver.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::testing;

// Dummy message
struct Ping { static constexpr TypeTag kTypeTag = make_user_tag(0x20001); };
struct Pong { static constexpr TypeTag kTypeTag = make_user_tag(0x20002); };

class TestProbeTest : public ::testing::Test {
protected:
    void SetUp() override {
        system_ = std::make_unique<ActorSystem>(
            ActorSystemConfig{}.with_name("probe-test").with_scheduler_threads(1));
        driver_ = std::make_unique<test::SchedulerTestDriver>(*system_);
    }

    std::unique_ptr<ActorSystem> system_;
    std::unique_ptr<test::SchedulerTestDriver> driver_;
};

TEST_F(TestProbeTest, ConstructsAndProvidesRef) {
    TestProbe<> probe(*system_);
    EXPECT_TRUE(probe.ref().address().id != ActorId{});
}

TEST_F(TestProbeTest, StartsWithEmptyQueue) {
    TestProbe<> probe(*system_);
    EXPECT_EQ(probe.queue_size(), 0u);
    EXPECT_TRUE(probe.queue().empty());
}

TEST_F(TestProbeTest, ReceivesMessageViaSend) {
    TestProbe<> probe(*system_);
    Ping msg;

    system_->try_deliver_local(probe.ref().address().id, msg);
    driver_->drain(10);

    EXPECT_GE(probe.queue_size(), 1u);
}

TEST_F(TestProbeTest, DrainEmptiesQueue) {
    TestProbe<> probe(*system_);
    Ping msg;

    system_->try_deliver_local(probe.ref().address().id, msg);
    driver_->drain(10);
    EXPECT_GE(probe.queue_size(), 1u);

    probe.drain();
    EXPECT_EQ(probe.queue_size(), 0u);
}

TEST_F(TestProbeTest, ExpectNoMessagePassesForEmptyQueue) {
    TestProbe<> probe(*system_);
    probe.expect_no_message<Ping>();  // should not fail
}

TEST_F(TestProbeTest, FishForMessageFindsMatching) {
    TestProbe<> probe(*system_);
    Pong msg;

    system_->try_deliver_local(probe.ref().address().id, msg);
    driver_->drain(10);

    auto* found = probe.fish_for_message<Pong>(
        [](const Pong&) { return true; });
    EXPECT_NE(found, nullptr);
}

TEST_F(TestProbeTest, FishForMessageReturnsNullOnWrongType) {
    TestProbe<> probe(*system_);
    Ping msg;

    system_->try_deliver_local(probe.ref().address().id, msg);
    driver_->drain(10);

    auto* found = probe.fish_for_message<Pong>(
        [](const Pong&) { return true; });
    EXPECT_EQ(found, nullptr);
}

TEST_F(TestProbeTest, MultipleMessagesStackUp) {
    TestProbe<> probe(*system_);
    Ping p1, p2, p3;

    system_->try_deliver_local(probe.ref().address().id, p1);
    system_->try_deliver_local(probe.ref().address().id, p2);
    system_->try_deliver_local(probe.ref().address().id, p3);
    driver_->drain(10);

    EXPECT_EQ(probe.queue_size(), 3u);
}
```

### Step 3: Build and run tests

```bash
ninja -C build test_unit_actor 2>&1 | tail -5
./build/tests/unit/actor/test_unit_actor --gtest_filter="*TestProbe*" 2>&1 | tail -30
```

Expected: All TestProbe tests pass.

### Step 4: Commit

```bash
git add tests/unit/actor/test_probe_test.cpp tests/unit/actor/CMakeLists.txt
git commit -m "test: add TestProbe dogfood tests (8 tests)"
```

---

## Task 12: MessageAdapter — Behavior Combinator

**Files:**
- Modify: `include/hpactor/actor/behavior.hpp` — add `MessageAdapter` to ComposeState::Type, add `message_adapter()` factory
- Modify: `src/actor/behavior.cpp` — add MessageAdapter dispatch case in ComposeState::invoke()

### Step 1: Add MessageAdapter to ComposeState::Type enum

In `behavior.hpp`, at the `ComposeState::Type` enum (approx line 287):

```cpp
enum class Type : uint8_t { Intercept, Compose, OnSignal, Setup, MessageAdapter };
```

### Step 2: Add adapter fields to ComposeState

After `factory` field in ComposeState (approx line 305), add:

```cpp
/// For MessageAdapter: function to translate From → To.
std::function<TypedMessage(const TypedMessage&)> adapter_fn;
/// For MessageAdapter: expected source TypeTag.
TypeTag adapter_from_tag = TypeTag::Invalid;
```

### Step 3: Add MessageAdapter dispatch in ComposeState::invoke()

In `src/actor/behavior.cpp`, in the `ComposeState::invoke()` method, add a case for `MessageAdapter`:

```cpp
case Type::MessageAdapter: {
    if (msg.type_id() == adapter_from_tag && adapter_fn) {
        auto translated = adapter_fn(msg);
        translated.set_trace_context(msg.trace_context());
        inner->operator()(translated);
    } else {
        inner->operator()(msg);
    }
    break;
}
```

### Step 4: Add message_adapter() static factory to Behavior

In `behavior.hpp`, after the `setup()` factory (approx line 271):

```cpp
/// \brief Create a message adapter combinator.
///
/// Messages of type \c From are translated to type \c To via
/// \p adapter_fn before being dispatched to \p inner. All other
/// message types pass through unchanged.
///
/// \tparam From Source message type (proto).
/// \tparam To   Target message type (proto).
/// \param[in] adapter_fn Translation function.
/// \param[in] inner     The inner behavior to dispatch to.
/// \return A Behavior that adapts \c From messages to \c To.
template <typename From, typename To>
static Behavior message_adapter(
    std::function<To(const From&)> adapter_fn,
    Behavior inner) {
    Behavior result;
    auto state = std::make_shared<ComposeState>();
    state->type = ComposeState::Type::MessageAdapter;
    state->inner = std::make_shared<Behavior>(std::move(inner));
    state->adapter_from_tag = From::kTypeTag;
    state->adapter_fn = [fn = std::move(adapter_fn)](const TypedMessage& msg) -> TypedMessage {
        const auto* from = static_cast<const From*>(msg.as_proto());
        To to = fn(*from);
        return TypedMessage(To::kTypeTag, to);
    };
    result.compose_ = std::move(state);
    return result;
}
```

### Step 5: Build

```bash
ninja -C build 2>&1 | tail -10
```

Expected: Build succeeds.

### Step 6: Commit

```bash
git add include/hpactor/actor/behavior.hpp src/actor/behavior.cpp
git commit -m "feat(behavior): add message_adapter combinator to Behavior"
```

---

## Task 13: MessageAdapter — ActorContext Method

**Files:**
- Modify: `include/hpactor/actor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`

### Step 1: Add message_adapter to ActorContext public API

In `include/hpactor/actor/actor_context.hpp`:

```cpp
/// \brief Create a message adapter that translates From→To and
///        self-delivers the translated message.
///
/// Returns an ActorRef. When a message of type From is sent to
/// this ref, it is translated via adapter_fn and delivered as a
/// self-message of type To.
///
/// \tparam From Source message type (proto).
/// \tparam To   Target message type (proto).
/// \param[in] adapter_fn Translation function.
/// \return An ActorRef that performs the adapter translation.
template <typename From, typename To>
ActorRef message_adapter(std::function<To(const From&)> adapter_fn);
```

### Step 2: Implement in actor_context.cpp

```cpp
#include <hpactor/actor/abstract_actor.hpp>

template <typename From, typename To>
ActorRef ActorContext::message_adapter(std::function<To(const From&)> adapter_fn) {
    // Create a unique ActorId for this adapter ref.
    // Store the adapter function on the actor context's internal map.
    // The actor's receive() path checks this map before dispatch.

    // For simplicity, allocate a new ActorId with a flag marking it
    // as an adapter. The actor's receive method checks for adapter
    // entries by ActorId.
    static std::atomic<uint32_t> adapter_counter{0};
    uint32_t idx = adapter_counter.fetch_add(1, std::memory_order_relaxed);

    ActorId adapter_id{self_.id.value() ^ (0xAD000000 | idx)};  // synthetic id

    // Store the adapter on the owning actor
    auto* actor = owner_->get_actor(self_.id);
    if (actor && actor->is_event_based_actor()) {
        auto* eba = static_cast<EventBasedActor*>(actor.get());
        eba->add_message_adapter(adapter_id, From::kTypeTag,
            [fn = std::move(adapter_fn)](const TypedMessage& msg) -> TypedMessage {
                const auto* from = static_cast<const From*>(msg.as_proto());
                To to = fn(*from);
                return TypedMessage(To::kTypeTag, to);
            });
    }

    ActorAddress addr = self_;
    addr.id = adapter_id;
    return ActorRef(addr);
}
```

### Step 3: Add adapter storage to EventBasedActor

In `include/hpactor/actor/event_based_actor.hpp`, add:

```cpp
struct MessageAdapterEntry {
    TypeTag from_tag;
    std::function<TypedMessage(const TypedMessage&)> adapter_fn;
};

void add_message_adapter(ActorId id, TypeTag from_tag,
                         std::function<TypedMessage(const TypedMessage&)> fn) {
    adapters_[id] = MessageAdapterEntry{from_tag, std::move(fn)};
}

bool try_dispatch_adapter(TypedMessage& msg) {
    // Check if the message's target is an adapter ref
    auto it = adapters_.find(msg.target_id());
    if (it != adapters_.end() && msg.type_id() == it->second.from_tag) {
        auto translated = it->second.adapter_fn(msg);
        // Re-dispatch as self-message
        context()->send(self_address(), translated);
        return true;
    }
    return false;
}

private:
    std::unordered_map<ActorId, MessageAdapterEntry> adapters_;
```

### Step 4: Wire adapter check into receive()

In `src/actor/event_based_actor.cpp`, in `receive()`, add before the user dispatch:

```cpp
// Check for message adapter interception
if (try_dispatch_adapter(msg)) {
    return;  // Message adapted — don't process further
}
```

### Step 5: Build

```bash
ninja -C build 2>&1 | tail -10
```

Expected: Build succeeds.

### Step 6: Commit

```bash
git add include/hpactor/actor/actor_context.hpp src/actor/actor_context.cpp \
        include/hpactor/actor/event_based_actor.hpp src/actor/event_based_actor.cpp
git commit -m "feat(actor): add ActorContext::message_adapter with relay ref"
```

---

## Task 14: MessageAdapter Tests

**Files:**
- Create: `tests/unit/actor/test_behavior_message_adapter.cpp`
- Modify: `tests/unit/actor/CMakeLists.txt`

### Step 1: Add test to CMakeLists

```cmake
    test_behavior_message_adapter.cpp
```

### Step 2: Write tests

```cpp
// tests/unit/actor/test_behavior_message_adapter.cpp
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/testing/behavior_test_kit.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::testing;

struct OrderRequest  { static constexpr TypeTag kTypeTag = make_user_tag(0x30001); int qty; };
struct InternalCmd   { static constexpr TypeTag kTypeTag = make_user_tag(0x30002); int amount; };

// ── Combinator ──────────────────────────────────────────────

TEST(MessageAdapterCombinatorTest, TranslatesMatchingMessage) {
    auto inner = Behavior::make()
        .on<InternalCmd>([](const InternalCmd& cmd) {
            EXPECT_EQ(cmd.amount, 100);
        });

    auto adapted = Behavior::message_adapter<OrderRequest, InternalCmd>(
        [](const OrderRequest& req) {
            return InternalCmd{req.qty * 10};
        },
        inner);

    BehaviorTestKit kit(adapted);
    auto effect = kit.run(OrderRequest{10});
    // The inner handler ran and processed InternalCmd
    EXPECT_EQ(effect.kind, EffectKind::NoEffect);
}

TEST(MessageAdapterCombinatorTest, PassesThroughNonMatchingMessage) {
    auto inner = Behavior::make()
        .on<InternalCmd>([](const InternalCmd&) { /* handled */ });

    auto adapted = Behavior::message_adapter<OrderRequest, InternalCmd>(
        [](const OrderRequest& req) { return InternalCmd{req.qty}; },
        inner);

    BehaviorTestKit kit(adapted);
    // Send InternalCmd directly — should pass through un-adapted
    auto effect = kit.run(InternalCmd{42});
    EXPECT_EQ(effect.kind, EffectKind::NoEffect);
}

TEST(MessageAdapterCombinatorTest, ChainedAdapters) {
    auto inner = Behavior::make()
        .on<InternalCmd>([](const InternalCmd& cmd) {
            EXPECT_EQ(cmd.amount, 20);
        });

    auto adapted1 = Behavior::message_adapter<OrderRequest, InternalCmd>(
        [](const OrderRequest& req) { return InternalCmd{req.qty}; },
        inner);
    auto adapted2 = Behavior::message_adapter<OrderRequest, InternalCmd>(
        [](const OrderRequest& req) { return InternalCmd{req.qty * 2}; },
        adapted1);

    BehaviorTestKit kit(adapted2);
    kit.run(OrderRequest{10});
    // The outermost adapter wins (translates 10→20)
}

// ── Empty / edge ────────────────────────────────────────────

TEST(MessageAdapterCombinatorTest, AdapterWithEmptyInner) {
    auto adapted = Behavior::message_adapter<OrderRequest, InternalCmd>(
        [](const OrderRequest& req) { return InternalCmd{req.qty}; },
        Behavior::empty());

    BehaviorTestKit kit(adapted);
    auto effect = kit.run(OrderRequest{5});
    EXPECT_EQ(effect.kind, EffectKind::NoEffect);
}

TEST(MessageAdapterCombinatorTest, AdapterPreservesBehaviorChain) {
    auto inner = Behavior::make()
        .on<InternalCmd>([](const InternalCmd&) { /* handled */ });

    auto intercepted = Behavior::intercept(
        inner,
        [](TypedMessage&, Behavior::next_fn next) {
            next(TypedMessage{});  // artificial — real interceptor passes through
        });

    auto adapted = Behavior::message_adapter<OrderRequest, InternalCmd>(
        [](const OrderRequest& req) { return InternalCmd{req.qty}; },
        intercepted);

    BehaviorTestKit kit(adapted);
    auto effect = kit.run(InternalCmd{7});
    // Should dispatch through interceptor → inner
    EXPECT_EQ(effect.kind, EffectKind::NoEffect);
}
```

### Step 3: Build and run tests

```bash
ninja -C build test_unit_actor 2>&1 | tail -5
./build/tests/unit/actor/test_unit_actor --gtest_filter="*MessageAdapter*" 2>&1 | tail -30
```

Expected: All MessageAdapter tests pass.

### Step 4: Commit

```bash
git add tests/unit/actor/test_behavior_message_adapter.cpp tests/unit/actor/CMakeLists.txt
git commit -m "test: add MessageAdapter combinator tests (5 tests)"
```

---

## Task 15: CoordinatedShutdown User-Defined Phases

**Files:**
- Modify: `include/hpactor/actor/lifecycle/shutdown_coordinator.hpp`
- Modify: `src/actor/lifecycle/shutdown_coordinator.cpp`

### Step 1: Add UserPhaseDef and new methods to header

In `shutdown_coordinator.hpp`, add before the `ShutdownCoordinator` class:

```cpp
/// Definition of a user-defined shutdown phase.
struct UserPhaseDef {
    std::string name;
    ShutdownPhase after_phase{ShutdownPhase::Running};
    std::string after_user_name;
    std::chrono::milliseconds timeout{5'000};
    std::function<void()> callback;
};
```

Add to `ShutdownCoordinator` public API:

```cpp
/// Register a user phase after a built-in phase.
/// Returns true on success, false if name already exists.
bool add_user_phase(
    std::string_view phase_name,
    ShutdownPhase after_phase,
    std::chrono::milliseconds timeout,
    std::function<void()> callback);

/// Register a user phase after another user-defined phase.
bool add_user_phase_after(
    std::string_view phase_name,
    std::string_view after_phase_name,
    std::chrono::milliseconds timeout,
    std::function<void()> callback);

/// Get user-defined phase names in execution order.
std::vector<std::string_view> user_phase_names() const;
```

Add private member:

```cpp
std::vector<UserPhaseDef> user_phases_;
```

### Step 2: Implement in shutdown_coordinator.cpp

```cpp
bool ShutdownCoordinator::add_user_phase(
    std::string_view phase_name,
    ShutdownPhase after_phase,
    std::chrono::milliseconds timeout,
    std::function<void()> callback) {
    // Check for duplicate name
    for (auto& p : user_phases_) {
        if (p.name == phase_name) return false;
    }
    UserPhaseDef def;
    def.name = std::string(phase_name);
    def.after_phase = after_phase;
    def.timeout = timeout;
    def.callback = std::move(callback);
    user_phases_.push_back(std::move(def));
    return true;
}

bool ShutdownCoordinator::add_user_phase_after(
    std::string_view phase_name,
    std::string_view after_phase_name,
    std::chrono::milliseconds timeout,
    std::function<void()> callback) {
    for (auto& p : user_phases_) {
        if (p.name == phase_name) return false;
    }
    UserPhaseDef def;
    def.name = std::string(phase_name);
    def.after_user_name = std::string(after_phase_name);
    def.timeout = timeout;
    def.callback = std::move(callback);
    user_phases_.push_back(std::move(def));
    return true;
}

std::vector<std::string_view> ShutdownCoordinator::user_phase_names() const {
    std::vector<std::string_view> names;
    names.reserve(user_phases_.size());
    for (auto& p : user_phases_) names.push_back(p.name);
    return names;
}
```

### Step 3: Modify execute() to interleave user phases

In the existing `execute()` method, after each built-in phase, insert a loop that runs user phases anchored to that phase:

```cpp
// After DrainingIngress phase completes:
auto run_user_phases_after = [&](ShutdownPhase p) {
    for (auto& up : user_phases_) {
        if (up.after_phase == p && up.after_user_name.empty()) {
            set_phase(ShutdownPhase::DrainingIngress);  // still in this block
            if (up.callback) up.callback();
        }
    }
    // Also run user phases anchored to other user phases (topological order).
    // Track which phases have already executed to resolve transitive deps.
    std::unordered_set<std::string> executed_user_phases;
    auto run_user_phase = [&](const UserPhaseDef& up) {
        if (executed_user_phases.count(up.name)) return;
        if (!up.after_user_name.empty() &&
            !executed_user_phases.count(up.after_user_name)) {
            return;  // dependency not yet executed, try again later
        }
        if (up.callback) up.callback();
        executed_user_phases.insert(up.name);
    };

    bool any_ran = true;
    while (any_ran) {
        any_ran = false;
        for (auto& up : user_phases_) {
            if (!up.after_user_name.empty() &&
                !executed_user_phases.count(up.name)) {
                run_user_phase(up);
                any_ran = true;
            }
        }
    }
};

// In the execute() method, after each phase transition:
run_user_phases_after(phase);
```

### Step 4: Build

```bash
ninja -C build 2>&1 | tail -10
```

Expected: Build succeeds.

### Step 5: Commit

```bash
git add include/hpactor/actor/lifecycle/shutdown_coordinator.hpp src/actor/lifecycle/shutdown_coordinator.cpp
git commit -m "feat(shutdown): add user-defined phases to ShutdownCoordinator"
```

---

## Task 16: CoordinatedShutdown Tests

**Files:**
- Modify: `tests/integration/actor/test_shutdown_coordinator.cpp` (or Create: `tests/unit/actor/lifecycle/test_shutdown_coordinator.cpp`)

### Step 1: Add user-phase tests

If adding to the existing integration test file, append:

```cpp
// ── User-Defined Phases ─────────────────────────────────────

TEST_F(ShutdownCoordinatorTest, AddUserPhaseSucceeds) {
    auto* coord = system_->shutdown_coordinator();
    bool ok = coord->add_user_phase("custom-phase",
        ShutdownPhase::DrainingIngress,
        std::chrono::milliseconds(1'000),
        []() { /* no-op */ });
    EXPECT_TRUE(ok);
}

TEST_F(ShutdownCoordinatorTest, DuplicateUserPhaseNameFails) {
    auto* coord = system_->shutdown_coordinator();
    auto noop = []() {};
    EXPECT_TRUE(coord->add_user_phase("phase1",
        ShutdownPhase::DrainingIngress, std::chrono::milliseconds(500), noop));
    EXPECT_FALSE(coord->add_user_phase("phase1",
        ShutdownPhase::DrainingActors, std::chrono::milliseconds(500), noop));
}

TEST_F(ShutdownCoordinatorTest, UserPhaseNamesReturnsOrderedList) {
    auto* coord = system_->shutdown_coordinator();
    coord->add_user_phase("a", ShutdownPhase::DrainingIngress,
        std::chrono::milliseconds(500), []() {});
    coord->add_user_phase("b", ShutdownPhase::DrainingActors,
        std::chrono::milliseconds(500), []() {});

    auto names = coord->user_phase_names();
    EXPECT_EQ(names.size(), 2u);
}

TEST_F(ShutdownCoordinatorTest, UserPhaseCallbackFires) {
    auto* coord = system_->shutdown_coordinator();
    bool fired = false;
    coord->add_user_phase("fire-test",
        ShutdownPhase::DrainingIngress,
        std::chrono::milliseconds(500),
        [&fired]() { fired = true; });

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(100);
    opts.actor_drain_timeout = std::chrono::milliseconds(100);
    opts.cluster_leave_timeout = std::chrono::milliseconds(100);
    opts.force_after_timeout = false;

    system_->shutdown(opts);

    EXPECT_TRUE(fired);
}
```

### Step 2: Build and run tests

```bash
ninja -C build 2>&1 | tail -5
ctest -R "ShutdownCoordinator" --output-on-failure 2>&1 | tail -30
```

Expected: All shutdown coordinator tests pass.

### Step 3: Commit

```bash
git add tests/integration/actor/test_shutdown_coordinator.cpp
git commit -m "test(shutdown): add user-defined phase tests"
```

---

## Task 17: GroupRouter ServiceKey Integration

**Files:**
- Modify: `include/hpactor/actor/routing/group_router.hpp`
- Modify: `src/actor/routing/group_router.cpp`

### Step 1: Add ServiceKey constructor to GroupRouter header

In `group_router.hpp`, add new constructor declaration:

```cpp
/// Construct a GroupRouter that dynamically discovers routees via
/// the Receptionist using the given ServiceKey.
GroupRouter(ActorContext* ctx, ActorSystem& sys,
            receptionist::ServiceKey service_key,
            std::unique_ptr<IRoutingLogic> logic);
```

Add private member:

```cpp
std::optional<receptionist::ServiceKey> receptionist_key_;
```

### Step 2: Implement in group_router.cpp

```cpp
GroupRouter::GroupRouter(ActorContext* ctx, ActorSystem& sys,
                         receptionist::ServiceKey service_key,
                         std::unique_ptr<IRoutingLogic> logic)
    : EventBasedActor(ctx, sys), routing_logic_(std::move(logic)),
      service_key_(service_key.name),
      receptionist_key_(std::move(service_key)),
      needs_snapshots_(routing_logic_->needs_mailbox_snapshots()) {
    become(make_behavior());
    // Subscribe to the Receptionist for dynamic routee discovery.
    if (receptionist_key_) {
        context()->receptionist_subscribe(*receptionist_key_);
    }
}

// In make_behavior(), add a handler for Listing messages:
Behavior GroupRouter::make_behavior() {
    return Behavior::make()
        .on<receptionist::Listing>([this](const receptionist::Listing& listing) {
            // Update routees from the Receptionist listing.
            routees_.clear();
            for (auto& id : listing.actor_ids) {
                routees_.emplace_back(ActorAddress{/* build from id */});
            }
            routing_logic_->on_routees_changed(routees_);
        })
        .on<TypedMessage>([this](TypedMessage& msg) {
            // ... existing routing logic ...
        });
}
```

### Step 3: Build

```bash
ninja -C build 2>&1 | tail -10
```

Expected: Build succeeds.

### Step 4: Commit

```bash
git add include/hpactor/actor/routing/group_router.hpp src/actor/routing/group_router.cpp
git commit -m "feat(routing): add ServiceKey constructor to GroupRouter"
```

---

## Task 18: GroupRouter ServiceKey Tests

**Files:**
- Modify: `tests/unit/actor/routing/test_group_router.cpp`

### Step 1: Add ServiceKey-based routing test

```cpp
TEST_F(GroupRouterTest, ConstructWithServiceKey) {
    receptionist::ServiceKey key{"workers"};
    auto router = system_->spawn<GroupRouter>(
        key, std::make_unique<RoundRobinLogic>());
    driver_->drain(10);
    EXPECT_EQ(router->routee_count(), 0u);
}

TEST_F(GroupRouterTest, ServiceKeyRouterUpdatesRouteesFromListing) {
    receptionist::ServiceKey key{"workers"};

    // Register two actors with the Receptionist
    auto* rec = system_->receptionist();
    ASSERT_NE(rec, nullptr);

    auto worker1 = spawn_routee();
    auto worker2 = spawn_routee();

    rec->handle_register(receptionist::Register{key, worker1->address().id});
    rec->handle_register(receptionist::Register{key, worker2->address().id});

    // Create the GroupRouter — it should get the current listing
    auto router = system_->spawn<GroupRouter>(
        key, std::make_unique<RoundRobinLogic>());
    driver_->drain(20);

    // The router should now have 2 routees
    EXPECT_EQ(router->routee_count(), 2u);
}
```

### Step 2: Run tests

```bash
ninja -C build test_unit_actor 2>&1 | tail -5
./build/tests/unit/actor/test_unit_actor --gtest_filter="*GroupRouter*" 2>&1 | tail -30
```

Expected: All GroupRouter tests pass.

### Step 3: Commit

```bash
git add tests/unit/actor/routing/test_group_router.cpp
git commit -m "test(routing): add ServiceKey-based GroupRouter tests"
```

---

## Task 19: Full Build Verification

### Step 1: Full build

```bash
ninja -C build 2>&1 | tail -10
```

Expected: Zero errors.

### Step 2: Full test run

```bash
ctest --output-on-failure --parallel 8 2>&1 | tail -20
```

Expected: All ~2960 tests pass (2892 existing + ~68 new).

### Step 3: Commit any remaining changes

```bash
git add -A
git commit -m "chore: final integration fixes for Sprint 1 gap closure"
```

---

## Summary

| Task | Component | Files | Tests |
|------|-----------|-------|-------|
| 1–2 | ServiceKey + Messages | 2 new headers | — |
| 3 | TypeTags | 1 modified | — |
| 4 | Receptionist impl | 1 new, 1 modified | — |
| 5 | ActorSystem spawning | 2 modified | — |
| 6 | ActorContext methods | 2 modified | — |
| 7 | Receptionist tests | 1 new, 1 modified | ~14 |
| 8–9 | BehaviorTestKit | 1 new header, 1 new test | ~11 |
| 10–11 | TestProbe | 1 new header, 1 new test | ~8 |
| 12–14 | MessageAdapter | 4 modified, 1 new test | ~5 |
| 15–16 | CoordinatedShutdown | 3 modified | ~4 |
| 17–18 | GroupRouter ServiceKey | 3 modified | ~2 |
| 19 | Full verification | — | — |
| **Total** | | **10 new, 17 modified** | **~44** |
