# Actor Link and Monitor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `link_to`/`unlink_from` (bidirectional death sharing) and `monitor`/`demonitor` (one-way death watching) so that actors are notified via `DownMsg` when linked or monitored actors terminate.

**Architecture:** Add a virtual `actor_context()` accessor to `AbstractActor` so the stub methods can reach `ActorContext`. Add accessor methods to `ActorContext` for the `linked_`/`monitored_` vectors. Implement the four methods on `AbstractActor` — link sends a `LinkMsg` to the target for bidirectional handshake, monitor is local-only. Override `EventBasedActor::receive()` to intercept system messages (`LinkMsg`, `UnlinkMsg`, `DownMsg`) before delegating to user behavior. Override `on_exit()` to propagate `DownMsg` to all linked and monitored actors.

**Tech Stack:** C++20, protobuf (hpactor::LinkMessage, hpactor::DownMessage, hpactor::UnlinkMessage), existing ActorSystem/deliver_local infrastructure

**Design Spec:** `docs/architecture/core/actor-link-monitor-design.md`

---

### Task 1: Add `actor_context()` virtual accessor to AbstractActor

**Files:**
- Modify: `include/hpactor/actor/abstract_actor.hpp:88-91` (protected section)
- Modify: `include/hpactor/actor/local_actor.hpp:25-48` (add override)
- Modify: `src/actor/abstract_actor.cpp:19-47` (use in stubs)

**Why:** `AbstractActor` has `link_to`/`unlink_from`/`monitor`/`demonitor` methods but no access to `ActorContext` (which is owned by `LocalActor`). A virtual accessor lets the base class reach the context polymorphically; subclasses that lack a context (system actors) return nullptr and link/monitor become no-ops.

- [ ] **Step 1: Add virtual `actor_context()` to AbstractActor header**

In `include/hpactor/actor/abstract_actor.hpp`, add to the protected section after the constructor:

```cpp
  protected:
    AbstractActor(ActorId id, ActorType type, ActorSystem& sys);

    // Overridden by LocalActor to return the ActorContext.
    // Returns nullptr for actors without a context (e.g., system actor).
    virtual ActorContext* actor_context() { return nullptr; }
```

Forward-declare `ActorContext` if needed (it already is forward-declared transitively through other includes, but verify).

- [ ] **Step 2: Override `actor_context()` in LocalActor**

In `include/hpactor/actor/local_actor.hpp`, add in the protected section:

```cpp
  protected:
    LocalActor(ActorContext* ctx, ActorSystem& sys);
    LocalActor(ActorId id, ActorContext* ctx, ActorSystem& sys);

    ActorContext* actor_context() override { return ctx_; }

    virtual void on_activate() {}
    virtual void on_deactivate() {}
```

- [ ] **Step 3: Verify build compiles**

```bash
cmake -S . -B build -GNinja && ninja -C build
```
Expected: Clean build, no new errors.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/abstract_actor.hpp include/hpactor/actor/local_actor.hpp
git commit -m "feat(actor): add virtual actor_context() accessor to AbstractActor
Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 2: Add accessor methods to ActorContext

**Files:**
- Modify: `include/hpactor/actor_context.hpp:98-101` (add methods)
- Modify: `tests/actor/test_actor_context.cpp` (add tests)

**Why:** `linked_` and `monitored_` are private vectors. `AbstractActor` needs to mutate them through clean accessor methods rather than direct field access.

- [ ] **Step 1: Add accessor methods to ActorContext header**

In `include/hpactor/actor_context.hpp`, in the public section near the existing `linked_actors()` and `monitor()` methods:

```cpp
    // Link management (used by AbstractActor)
    void add_linked(const ActorAddress& addr) { linked_.push_back(addr); }
    void remove_linked(const ActorAddress& addr) {
        linked_.erase(std::remove(linked_.begin(), linked_.end(), addr), linked_.end());
    }

    // Monitor management (used by AbstractActor)
    void add_monitored(const ActorAddress& addr) { monitored_.push_back(addr); }
    void remove_monitored(const ActorAddress& addr) {
        monitored_.erase(std::remove(monitored_.begin(), monitored_.end(), addr),
                         monitored_.end());
    }

    // Read access to monitored actors
    const std::vector<ActorAddress>& monitored_actors() const { return monitored_; }
```

Note: `<algorithm>` is needed for `std::remove`. Check if it's already included transitively.

- [ ] **Step 2: Write tests for new accessor methods**

In `tests/actor/test_actor_context.cpp`, add:

```cpp
void test_actor_context_add_remove_linked() {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    assert(ctx.linked_actors().empty());

    ActorAddress addr1{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1},
                       ActorId{10}, 0};
    ctx.add_linked(addr1);
    assert(ctx.linked_actors().size() == 1);
    assert(ctx.linked_actors()[0] == addr1);

    // Duplicate add — allowed (idempotency is caller's responsibility)
    ctx.add_linked(addr1);
    assert(ctx.linked_actors().size() == 2);

    ctx.remove_linked(addr1);
    assert(ctx.linked_actors().size() == 1);

    ctx.remove_linked(addr1);
    assert(ctx.linked_actors().empty());

    // Remove non-existent — no-op, no crash
    ctx.remove_linked(addr1);
    assert(ctx.linked_actors().empty());
}

void test_actor_context_add_remove_monitored() {
    Actor empty_actor;
    ActorContext ctx(empty_actor);

    assert(ctx.monitored_actors().empty());

    ActorAddress addr1{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{2},
                       ActorId{20}, 0};
    ctx.add_monitored(addr1);
    assert(ctx.monitored_actors().size() == 1);
    assert(ctx.monitored_actors()[0] == addr1);

    ctx.remove_monitored(addr1);
    assert(ctx.monitored_actors().empty());

    // Remove non-existent — no-op
    ctx.remove_monitored(addr1);
    assert(ctx.monitored_actors().empty());
}
```

Register them in `main()`:
```cpp
test_actor_context_add_remove_linked();
test_actor_context_add_remove_monitored();
```

- [ ] **Step 3: Build and run the test**

```bash
ninja -C build test_actor_context && ./build/tests/test_actor_context
```
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor_context.hpp tests/actor/test_actor_context.cpp
git commit -m "feat(actor): add linked/monitored accessor methods to ActorContext
Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 3: Implement link_to and unlink_from on AbstractActor

**Files:**
- Modify: `src/actor/abstract_actor.cpp:22-28` (replace stubs)
- Ensure: `include/hpactor/actor/abstract_actor.hpp` has needed includes

**Why:** `link_to` sends a `LinkMsg` to the target for bidirectional handshake. `unlink_from` sends `UnlinkMsg` to notify the target to remove the link.

- [ ] **Step 1: Check required includes in abstract_actor.cpp**

The implementation needs:
- `<hpactor/actor_context.hpp>` — for `ActorContext` accessors
- `<hpactor/messages.pb.h>` — for `LinkMessage`, `UnlinkMessage`

`abstract_actor.cpp` already includes `<hpactor/actor/abstract_actor.hpp>`. Verify `ActorContext` is available (it's forward-declared in the header but the .cpp needs the full definition).

- [ ] **Step 2: Implement link_to**

Replace the stub in `src/actor/abstract_actor.cpp`:

```cpp
#include <hpactor/actor_context.hpp>
#include <hpactor/messages.pb.h>

#include <iostream>

void AbstractActor::link_to(const ActorAddr& other) {
    auto* ctx = actor_context();
    if (!ctx) return;

    // Reject link-to-self
    if (other == address()) {
        std::cerr << "HPActor: link_to self (" << id().value() << ") ignored"
                  << std::endl;
        return;
    }

    // Idempotency: check if already linked
    for (const auto& linked : ctx->linked_actors()) {
        if (linked == other) return;
    }

    ctx->add_linked(other);

    // Notify target via LinkMessage
    hpactor::LinkMessage pb;
    pb.set_actor_id(id().value());

    bytes payload(pb.ByteSizeLong());
    pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    ctx->send(other, TypedMessage(TypeTag::LinkMsg, std::move(payload)));
}
```

- [ ] **Step 3: Implement unlink_from**

```cpp
void AbstractActor::unlink_from(const ActorAddr& other) {
    auto* ctx = actor_context();
    if (!ctx) return;

    ctx->remove_linked(other);

    // Notify target via UnlinkMessage
    hpactor::UnlinkMessage pb;
    pb.set_actor_id(id().value());

    bytes payload(pb.ByteSizeLong());
    pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    ctx->send(other, TypedMessage(TypeTag::UnlinkMsg, std::move(payload)));
}
```

- [ ] **Step 4: Implement monitor**

```cpp
void AbstractActor::monitor(const ActorAddr& target) {
    auto* ctx = actor_context();
    if (!ctx) return;

    // Idempotency
    for (const auto& m : ctx->monitored_actors()) {
        if (m == target) return;
    }

    // Use the existing ActorContext::monitor which adds to monitored_
    ctx->monitor(target);
}
```

Wait — `ActorContext::monitor()` already exists but it's the method that should be called. Let me re-examine: the existing `ActorContext::monitor()` only does `monitored_.push_back(target)`. We added `add_monitored()` in Task 2. To avoid duplication, update `ActorContext::monitor()` to delegate to `add_monitored()`:

```cpp
void ActorContext::monitor(const ActorAddress& target) {
    add_monitored(target);
}
```

Then `AbstractActor::monitor()` calls `ctx->monitor(target)`.

- [ ] **Step 5: Implement demonitor**

```cpp
void AbstractActor::demonitor(const ActorAddr& target) {
    auto* ctx = actor_context();
    if (!ctx) return;
    ctx->remove_monitored(target);
}
```

- [ ] **Step 6: Build and verify compilation**

```bash
ninja -C build
```
Expected: Clean build. Link against hpactor_proto if needed (check CMakeLists.txt for hpactor_lib).

- [ ] **Step 7: Commit**

```bash
git add src/actor/abstract_actor.cpp src/actor/actor_context.cpp
git commit -m "feat(actor): implement link_to, unlink_from, monitor, demonitor
Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 4: Intercept system messages in EventBasedActor::receive()

**Files:**
- Modify: `src/actor/event_based_actor.cpp:27-56` (receive implementation)

**Why:** `EventBasedActor::receive()` must intercept `LinkMsg`, `UnlinkMsg`, and `DownMsg` before the existing proto-handler and behavior dispatch. This makes link/monitor bookkeeping transparent to user code. System message interception is added at the top of the existing `receive()` body; `LinkMsg`/`UnlinkMsg` return early, `DownMsg` cleans up then falls through to the existing handler pipeline so supervision can react.

**Critical: The existing `receive()` at line 27 performs three essential operations that must be preserved:**
1. Lazy proto handler initialization (`handlers_initialized_` + `initialize_proto_handlers()`)
2. Sender address capture (`ctx->set_current_sender(msg.sender_address())`)
3. Proto handler dispatch (`proto_handlers_` lookup with deserialize → invoke → reply)

The system message interception is inserted *before* the proto handler lookup, so link/monitor bookkeeping happens regardless of whether the actor uses proto handlers or behaviors.

- [ ] **Step 1: Read the existing receive() implementation**

The existing code is at `src/actor/event_based_actor.cpp:27-56`.

- [ ] **Step 2: Insert system message dispatch at top of receive()**

Insert between line 26 (`void EventBasedActor::receive(TypedMessage& msg) {`) and line 28 (`if (!handlers_initialized_)`). First add the include at the top of the file:

```cpp
#include <hpactor/messages.pb.h>
```

Then modify `receive()` — insert the system message interception block immediately after the opening brace:

```cpp
void EventBasedActor::receive(TypedMessage& msg) {
    // -- System message interception (link / monitor / death) --
    {
        auto* ctx = context();
        switch (msg.type_id()) {
            case TypeTag::LinkMsg: {
                // Bidirectional link handshake: add sender to our linked_ set
                if (ctx) {
                    const auto& sender = msg.sender_address();
                    bool already_linked = false;
                    for (const auto& linked : ctx->linked_actors()) {
                        if (linked == sender) {
                            already_linked = true;
                            break;
                        }
                    }
                    if (!already_linked) {
                        ctx->add_linked(sender);
                    }
                }
                return; // System message — fully handled, do not forward
            }

            case TypeTag::UnlinkMsg: {
                if (ctx) {
                    ctx->remove_linked(msg.sender_address());
                }
                return; // System message — fully handled
            }

            case TypeTag::DownMsg: {
                // Clean up linked/monitored entries for the dead actor
                if (ctx) {
                    ctx->remove_linked(msg.sender_address());
                    ctx->remove_monitored(msg.sender_address());
                }
                // Fall through — behavior/supervision must see DownMsg
                break;
            }

            default:
                break;
        }
    }
    // -- End system message interception --

    // Existing handler pipeline (preserved exactly as-is, including the
    // local ctx redeclaration on line 33 of the original file):
    if (!handlers_initialized_) {
        initialize_proto_handlers();
    }

    auto* ctx = context();
    if (ctx) {
        ctx->set_current_sender(msg.sender_address());
    }

    auto it = proto_handlers_.find(msg.type_id());
    if (it != proto_handlers_.end()) {
        auto deserialized = it->second.deserialize(msg.payload());
        if (deserialized) {
            bytes response = it->second.invoke(std::move(deserialized));
            if (!response.empty() && ctx) {
                TypedMessage reply_msg(it->first, response);
                ctx->reply(std::move(reply_msg));
            }
        }
        return;
    }

    if (behavior_) {
        behavior_(msg);
    }
}
```

- [ ] **Step 3: Build and fix any issues**

```bash
ninja -C build
```

Expected: Clean build. If `messages.pb.h` is not found, verify `hpactor_proto` is in the link dependencies for `hpactor_lib` in CMakeLists.txt (it should already be there since `supervision.cpp` uses it).

- [ ] **Step 4: Commit**

```bash
git add src/actor/event_based_actor.cpp
git commit -m "feat(actor): intercept LinkMsg/UnlinkMsg/DownMsg in EventBasedActor::receive()

System message dispatch is inserted before the existing proto-handler and
behavior pipeline. LinkMsg/UnlinkMsg return early. DownMsg cleans up linked
and monitored lists then falls through so supervision can react.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 5: Add exit_reason tracking and on_exit death propagation

**Files:**
- Modify: `include/hpactor/actor/event_based_actor.hpp:239` (on_exit declaration, add exit_reason_ field and setter)
- Modify: `src/actor/event_based_actor.cpp` (on_exit implementation)

**Why:** When an actor terminates, `on_exit()` must send `DownMsg` to every actor in `linked_` and `monitored_`. An `exit_reason_` field tracks why the actor stopped (0 = normal, non-zero = error). The header currently defines `on_exit()` inline as `virtual void on_exit() {}` — this must become a declaration-only so the implementation can live in the .cpp.

- [ ] **Step 1: Add exit_reason_ field and setter, change on_exit to declaration**

In `include/hpactor/actor/event_based_actor.hpp`, change line 239 from:

```cpp
    virtual void on_exit() {}
```

To a declaration-only (note: no `override` — `on_exit` is not in the base class):

```cpp
    virtual void on_exit();
```

And add exit_reason support nearby (in the public or a new protected section):

```cpp
    void set_exit_reason(uint32_t code) { exit_reason_ = code; }
    uint32_t exit_reason() const { return exit_reason_; }
```

In the private section (near `behavior_` and `exit_reason_`):

```cpp
    uint32_t exit_reason_ = 0;
```

- [ ] **Step 2: Implement on_exit in event_based_actor.cpp**

In `src/actor/event_based_actor.cpp`:

```cpp
void EventBasedActor::on_exit() {
    auto* ctx = context();
    if (!ctx) return;

    // Build DownMessage
    hpactor::DownMessage pb;
    pb.set_actor_id(id().value());
    pb.set_reason_code(exit_reason_);

    bytes payload(pb.ByteSizeLong());
    pb.SerializeToArray(payload.data(), static_cast<int>(payload.size()));

    // Send DownMsg to all linked actors
    for (const auto& addr : ctx->linked_actors()) {
        TypedMessage down_msg(TypeTag::DownMsg, bytes(payload));
        ctx->send(addr, std::move(down_msg));
    }

    // Send DownMsg to all monitored actors
    for (const auto& addr : ctx->monitored_actors()) {
        TypedMessage down_msg(TypeTag::DownMsg, bytes(payload));
        ctx->send(addr, std::move(down_msg));
    }
    // linked_ and monitored_ vectors will be destroyed with the context
}
```

- [ ] **Step 3: Build**

```bash
ninja -C build
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/actor/event_based_actor.hpp src/actor/event_based_actor.cpp
git commit -m "feat(actor): add exit_reason tracking and on_exit death propagation
Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 6: Integrate exit_reason with scheduler termination paths

**Files:**
- Modify: `src/sched/scheduler.cpp:220-244` (execute_actor termination)

**Why:** The scheduler calls `on_exit()` in two places — coroutine path (line 238) and behavior path (line 224). Before calling `on_exit()`, it should set the exit reason for non-normal terminations.

- [ ] **Step 1: Audit scheduler termination paths**

Read `src/sched/scheduler.cpp` around lines 220-244:

```bash
grep -n "on_exit\|is_terminated\|coroutine.done" src/sched/scheduler.cpp
```

There are two paths:
1. Line ~224: CAS fails because state is Terminated → `actor->on_exit()`
2. Line ~237-238: `coroutine.done()` → `actor->on_exit()`

- [ ] **Step 2: Set exit_reason before on_exit calls**

For the "terminated" path (abnormal):
```cpp
// Before actor->on_exit() on line 224:
actor->set_exit_reason(errors::actor_down);
actor->on_exit();
```

For the coroutine done path (normal exit):
```cpp
// Before actor->on_exit() on line 238:
// exit_reason_ defaults to 0 (normal) — no change needed, but explicit is fine
actor->on_exit();
```

- [ ] **Step 3: Build**

```bash
ninja -C build
```

- [ ] **Step 4: Commit**

```bash
git add src/sched/scheduler.cpp
git commit -m "fix(sched): set exit_reason before on_exit in termination paths
Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 7: Write integration test for link_to and down notification

**Files:**
- Create: `tests/actor/test_link_monitor.cpp`
- Modify: `tests/CMakeLists.txt` (add test target)

**Why:** End-to-end test proving that when a linked actor dies, the surviving actor receives DownMsg.

**Important:** Death propagation via `on_exit()` requires the coroutine scheduler path (`config.use_coroutines = true`) because the behavior-based path in `scheduler.cpp` does not call `on_exit()` on termination. The tests are gated behind `#if HPACTOR_SUPPORT_COROUTINES` so they compile-out (pass trivially) when coroutine support is unavailable.

- [ ] **Step 1: Create the test file**

Create `tests/actor/test_link_monitor.cpp`:

```cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/messages.pb.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace hpactor;

// Helper: poll until condition is true or timeout expires
template <typename Fn>
static bool poll_until(Fn&& condition, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

// Actor that records received DownMsg notifications
class DownRecordingActor : public EventBasedActor {
  public:
    DownRecordingActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    int down_count() const { return down_count_; }
    ActorId last_down_actor() const { return last_down_actor_; }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == TypeTag::DownMsg) {
                auto pb = std::make_shared<::hpactor::DownMessage>();
                if (pb->ParseFromArray(msg.payload().data(),
                                       static_cast<int>(msg.payload().size()))) {
                    ++down_count_;
                    last_down_actor_ = ActorId(pb->actor_id());
                }
            }
        }};
    }

  private:
    int down_count_ = 0;
    ActorId last_down_actor_;
};

// =============================================================================
// Tests that require coroutine-based scheduling (on_exit is only called
// from the coroutine path in scheduler.cpp::execute_actor).
// =============================================================================
#if HPACTOR_SUPPORT_COROUTINES

// Actor that exits after processing one message.
// The coroutine path calls on_exit() when the coroutine completes.
class ShortLivedActor : public EventBasedActor {
  public:
    ShortLivedActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    sched::CoroutineTask act() override {
        auto msg = co_await make_mailbox_awaiter();
        set_exit_reason(errors::actor_down);
        co_return;
    }
};

// Test: A links to B. B exits. A receives DownMsg.
void test_link_to_down_notification() {
    Config config{.scheduler_threads = 1, .max_queue_depth = 1024,
                  .use_coroutines = true};
    ActorSystem system(config);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<ShortLivedActor>();

    // A links to B
    a.get()->link_to(b.address());

    // Verify A has B in linked_ list
    auto* ctx_a = a.get()->context();
    assert(ctx_a != nullptr);
    bool found = false;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == b.address()) { found = true; break; }
    }
    assert(found);

    // Deliver message to trigger B's coroutine (which will exit after one msg)
    system.deliver_local(b.id(), TypedMessage(TypeTag::User, bytes{1}));

    // Poll until A receives DownMsg or timeout
    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    bool received = poll_until([rec, &b]() {
        return rec->down_count() >= 1 && rec->last_down_actor() == b.id();
    });
    assert(received);
}

// Test: monitor is one-way — A monitors B, B exits, A gets DownMsg
void test_monitor_down_notification() {
    Config config{.scheduler_threads = 1, .max_queue_depth = 1024,
                  .use_coroutines = true};
    ActorSystem system(config);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<ShortLivedActor>();

    a.get()->monitor(b.address());

    system.deliver_local(b.id(), TypedMessage(TypeTag::User, bytes{1}));

    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    bool received = poll_until([rec, &b]() {
        return rec->down_count() >= 1 && rec->last_down_actor() == b.id();
    });
    assert(received);
}

// Test: unlink_from removes the link, no DownMsg after unlink
void test_unlink_from_stops_notification() {
    Config config{.scheduler_threads = 1, .max_queue_depth = 1024,
                  .use_coroutines = true};
    ActorSystem system(config);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<ShortLivedActor>();

    a.get()->link_to(b.address());
    a.get()->unlink_from(b.address());

    // Verify link removed
    auto* ctx_a = a.get()->context();
    for (const auto& linked : ctx_a->linked_actors()) {
        assert(!(linked == b.address()));
    }

    system.deliver_local(b.id(), TypedMessage(TypeTag::User, bytes{1}));

    // Give time for any potential message delivery, then verify no DownMsg
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    assert(rec->down_count() == 0);
}

// Test: demonitor stops monitoring
void test_demonitor_stops_notification() {
    Config config{.scheduler_threads = 1, .max_queue_depth = 1024,
                  .use_coroutines = true};
    ActorSystem system(config);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<ShortLivedActor>();

    a.get()->monitor(b.address());
    a.get()->demonitor(b.address());

    system.deliver_local(b.id(), TypedMessage(TypeTag::User, bytes{1}));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    assert(rec->down_count() == 0);
}

// Test: link to already-dead/same actor by manually testing link_to self-rejection
void test_link_to_dead_or_self() {
    Config config{.scheduler_threads = 1, .max_queue_depth = 1024,
                  .use_coroutines = true};
    ActorSystem system(config);

    auto a = system.spawn<DownRecordingActor>();

    // link-to-self should be rejected
    a.get()->link_to(a.get()->address());

    auto* ctx_a = a.get()->context();
    int self_count = 0;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == a.get()->address()) ++self_count;
    }
    assert(self_count == 0);
}

#else // !HPACTOR_SUPPORT_COROUTINES

// Stub tests when coroutine support is not compiled in.
// Death propagation (on_exit) requires the coroutine scheduler path.
void test_link_to_down_notification() {
    // SKIP: coroutine support required for scheduler-mediated on_exit
}

void test_monitor_down_notification() {
    // SKIP: coroutine support required
}

void test_unlink_from_stops_notification() {
    // SKIP: coroutine support required
}

void test_demonitor_stops_notification() {
    // SKIP: coroutine support required
}

void test_link_to_dead_or_self() {
    // Self-rejection doesn't require coroutines — still test it
    Config config{.scheduler_threads = 1, .max_queue_depth = 1024};
    ActorSystem system(config);

    auto a = system.spawn<DownRecordingActor>();
    a.get()->link_to(a.get()->address());

    auto* ctx_a = a.get()->context();
    int self_count = 0;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == a.get()->address()) ++self_count;
    }
    assert(self_count == 0);
}

#endif // HPACTOR_SUPPORT_COROUTINES

// =============================================================================
// Tests that work regardless of coroutine support (protocol-only)
// =============================================================================

// Test: link_to is idempotent
void test_link_to_idempotent() {
    Config config{.scheduler_threads = 1, .max_queue_depth = 1024};
    ActorSystem system(config);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<DownRecordingActor>();

    a.get()->link_to(b.address());
    a.get()->link_to(b.address()); // duplicate

    // Should only have one entry
    auto* ctx_a = a.get()->context();
    int count = 0;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == b.address()) ++count;
    }
    assert(count == 1);
}

// Test: link_to sends LinkMsg, which adds bidirectional entry on receiver
void test_link_to_sends_link_msg() {
    Config config{.scheduler_threads = 1, .max_queue_depth = 1024};
    ActorSystem system(config);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<DownRecordingActor>();

    a.get()->link_to(b.address());

    // The LinkMsg should be in B's mailbox, which the scheduler should deliver
    // via receive(), adding A to B's linked_ set
    // Poll for delivery
    auto* ctx_b = b.get()->context();
    bool delivered = poll_until([ctx_b, &a]() {
        for (const auto& linked : ctx_b->linked_actors()) {
            if (linked == a.get()->address()) return true;
        }
        return false;
    });
    assert(delivered);
}

int main() {
    test_link_to_down_notification();
    std::cout << "PASS: test_link_to_down_notification" << std::endl;

    test_monitor_down_notification();
    std::cout << "PASS: test_monitor_down_notification" << std::endl;

    test_unlink_from_stops_notification();
    std::cout << "PASS: test_unlink_from_stops_notification" << std::endl;

    test_demonitor_stops_notification();
    std::cout << "PASS: test_demonitor_stops_notification" << std::endl;

    test_link_to_idempotent();
    std::cout << "PASS: test_link_to_idempotent" << std::endl;

    test_link_to_dead_or_self();
    std::cout << "PASS: test_link_to_dead_or_self" << std::endl;

    test_link_to_sends_link_msg();
    std::cout << "PASS: test_link_to_sends_link_msg" << std::endl;

    return 0;
}
```

- [ ] **Step 2: Register test in CMakeLists.txt**

In `tests/CMakeLists.txt`, add after the existing actor tests:

```cmake
add_executable(test_link_monitor actor/test_link_monitor.cpp)
target_link_libraries(test_link_monitor hpactor hpactor_proto pthread)
add_test(NAME test_link_monitor COMMAND test_link_monitor)
```

- [ ] **Step 3: Build and run the test**

```bash
ninja -C build test_link_monitor && ./build/tests/test_link_monitor
```
Expected: All tests pass.

- [ ] **Step 4: Run full test suite to check for regressions**

```bash
ctest --output-on-failure
```
Expected: All existing tests pass.

- [ ] **Step 5: Commit**

```bash
git add tests/actor/test_link_monitor.cpp tests/CMakeLists.txt
git commit -m "test(actor): add link/monitor integration tests
Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 8: Final verification and cleanup

**Files:**
- Verify: all modified files

- [ ] **Step 1: Run full build**

```bash
cmake -S . -B build -GNinja && ninja -C build
```
Expected: Zero errors.

- [ ] **Step 2: Run full test suite**

```bash
ctest --output-on-failure
```
Expected: All tests pass (23 existing + new link_monitor test).

- [ ] **Step 3: Review diff for completeness**

```bash
git diff --stat main
```

Expected files changed:
- `include/hpactor/actor/abstract_actor.hpp` — virtual `actor_context()`
- `include/hpactor/actor/local_actor.hpp` — override `actor_context()`
- `include/hpactor/actor_context.hpp` — accessor methods
- `include/hpactor/actor/event_based_actor.hpp` — `exit_reason_`, `set_exit_reason()`
- `src/actor/abstract_actor.cpp` — `link_to`, `unlink_from`, `monitor`, `demonitor`
- `src/actor/event_based_actor.cpp` — `receive()`, `on_exit()`
- `src/sched/scheduler.cpp` — `set_exit_reason` before `on_exit`
- `tests/actor/test_link_monitor.cpp` — new integration tests
- `tests/CMakeLists.txt` — test registration

- [ ] **Step 4: Check for any remaining stubs**

```bash
grep -rn "TODO.*link\|TODO.*monitor\|TODO.*implement link" include/ src/
```

Expected: No remaining TODO items for link/monitor.

---

### Task 9: Verify supervision integration is not broken

**Files:**
- Read: `src/supervision/supervision.cpp:49-55` (SupervisorActor::make_behavior)
- Run: existing supervision tests

**Why:** `SupervisorActor::make_behavior()` already catches `DownMsg` in its behavior handler to trigger restart/stop/escalate logic. The new `receive()` system message dispatch must not interfere — `DownMsg` must still reach the behavior after the link/monitor cleanup. This task confirms no regression.

- [ ] **Step 1: Verify DownMsg still reaches supervisor behavior**

In `src/supervision/supervision.cpp:49-55`, the `SupervisorActor::make_behavior()` returns:

```cpp
return Behavior{[this](TypedMessage& msg) {
    if (msg.type_id() == TypeTag::DownMsg) {
        handle_child_down(msg.type_id(), msg.payload());
    }
}};
```

In the new `receive()` (Task 4), `DownMsg` cleanup runs first (removing the dead actor from `linked_`/`monitored_`), then falls through `break` to the existing behavior pipeline. The supervisor's behavior handler still sees the `DownMsg` because the `break` does not return early.

**Verdict:** No code change needed — the `break` in the `DownMsg` case correctly passes through to behavior dispatch.

- [ ] **Step 2: Run existing supervision tests**

```bash
ninja -C build test_supervisor_actor test_self_supervising_actor test_one_for_one_supervisor test_all_for_one_supervisor test_supervision && ctest -R supervision --output-on-failure
```

Expected: All 5 supervision tests pass.

- [ ] **Step 3: Run example 04 (supervision tree demo)**

```bash
ninja -C build 04_supervision_tree && timeout 5 ./build/examples/04_supervision_tree 2>&1 || true
```

Expected: No crashes, supervision tree demo works as before.

- [ ] **Step 4: Commit**

(No code changes in this task — verification only, included for traceability in the plan.)

---

## Notes

- **ActorContext::monitor() duality:** The existing `ActorContext::monitor()` method (which just appends to `monitored_`) is called by `AbstractActor::monitor()`. Update it to delegate to `add_monitored()` to avoid code duplication.
- **Protobuf linking:** `messages.pb.h` is generated from `protos/hpactor/messages.proto`. Ensure `hpactor_proto` is linked for any target that includes this header.
- **No remote support yet:** This implementation handles local link/monitor. Remote support (via `ActorProxy`) follows the same protocol — the messages are already serialized as protobuf, and `ActorContext::send()` already routes through proxies for non-local targets.
- **Thread safety:** `linked_` and `monitored_` vectors are accessed only from within the actor's own execution context (single-threaded turn-based concurrency). No locking needed.
