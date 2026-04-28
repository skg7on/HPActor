# Unified Message Passing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ActorContext::send() location-transparent, with zero-copy shared_ptr delivery for local actors and protobuf-over-TCP for remote actors, plus working reply() tracking.

**Architecture:** ActorContext gains a ref_cache_ (LRU, max 256) and resolve() to map ActorAddress → ActorRef. send() stamps sender_address_ on the message, then dispatches through ActorRef (which already handles variant<Actor, ActorProxy>). deliver_local() remains the single mailbox entry point. deliver_remote() bridges WireFrame → TypedMessage. EventBasedActor::receive() captures current_sender_ for reply().

**Tech Stack:** C++20, protobuf, lock-free MPSC mailbox, existing TcpTransport/ConnectionPool

---

## File Structure Map

| File | Responsibility | New/Modify |
|------|---------------|------------|
| `include/hpactor/actor/typed_message.hpp` | TypedMessage: sender_address_ field + accessors + move update | Modify |
| `include/hpactor/core/actor_ref_cache.hpp` | ActorRefCache: LRU cache for resolved ActorRefs | **New** |
| `include/hpactor/actor_context.hpp` | ActorContext: ref_cache_, current_sender_, new send/reply sigs | Modify |
| `src/actor/actor_context.cpp` | ActorContext: resolve(), send() rewrite, reply() impl | Modify |
| `include/hpactor/core/actor_system.hpp` | ActorSystem: deliver_remote(), get_transport_for() | Modify |
| `src/actor/actor_system.cpp` | ActorSystem: deliver_remote() impl, get_transport_for() impl | Modify |
| `include/hpactor/ref/actor_proxy.hpp` | ActorProxy: new ActorProxy(addr, system) ctor | Modify |
| `src/ref/actor_proxy.cpp` | ActorProxy: new ctor impl | Modify |
| `include/hpactor/types/types.hpp` | TypeTag: ErrorMsg = 7 | Modify |
| `src/actor/event_based_actor.cpp` | current_sender_ capture in receive(), reply TODO resolved | Modify |
| `src/net/connection_pool.cpp` | on_frame_received(): route non-RPC to deliver_remote() | Modify |
| `tests/actor/test_actor_ref_cache.cpp` | ActorRefCache unit tests | **New** |
| `tests/actor/test_actor_context.cpp` | Extended with send/reply tests | Modify |
| `tests/CMakeLists.txt` | Register new test executables | Modify |

---

### Task 1: Add sender_address_ to TypedMessage

**Files:**
- Modify: `include/hpactor/actor/typed_message.hpp`

- [ ] **Step 1: Add sender_address_ field and accessors**

In `include/hpactor/actor/typed_message.hpp`, add the sender address field after `parsed_` (line 109) and public accessors:

```cpp
// After the existing as<T>() method (line 101), add:

    // Sender address — set by ActorContext::send() (local) or deliver_remote() (remote).
    // Read by EventBasedActor::receive() to populate current_sender_ for reply().
    const ActorAddress& sender_address() const noexcept { return sender_address_; }
    void set_sender_address(const ActorAddress& addr) { sender_address_ = addr; }

// Add to private section (after parsed_ on line 109):
    ActorAddress sender_address_{};
```

- [ ] **Step 2: Update move constructor to transfer sender_address_**

The move constructor at lines 48-53 must include `sender_address_`:

```cpp
TypedMessage(TypedMessage&& other) noexcept
    : tag_(other.tag_),
      payload_(std::move(other.payload_)),
      parsed_(std::move(other.parsed_)),
      sender_address_(other.sender_address_) {
    // mpsc_next is left default-initialized in the moved-from object
}
```

- [ ] **Step 3: Update move assignment to transfer sender_address_**

The move assignment at lines 54-59:

```cpp
TypedMessage& operator=(TypedMessage&& other) noexcept {
    tag_ = other.tag_;
    payload_ = std::move(other.payload_);
    parsed_ = std::move(other.parsed_);
    sender_address_ = other.sender_address_;
    return *this;
}
```

- [ ] **Step 4: Add include for ActorAddress**

The file needs `#include <hpactor/ref/actor_address.hpp>` since `sender_address_` is of type `ActorAddress`:

```cpp
#include <hpactor/ref/actor_address.hpp>
```

Add it after `#include <hpactor/types/types.hpp>` on line 17.

- [ ] **Step 5: Build to verify compilation**

```bash
ninja -C build
```
Expected: clean build, no errors.

- [ ] **Step 6: Run existing tests to verify no regressions**

```bash
ctest --output-on-failure
```
Expected: all 59 tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/actor/typed_message.hpp
git commit -m "feat(types): add sender_address_ to TypedMessage for reply tracking

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 2: Implement ActorRefCache

**Files:**
- Create: `include/hpactor/core/actor_ref_cache.hpp`
- Create: `tests/actor/test_actor_ref_cache.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/actor/test_actor_ref_cache.cpp`:

```cpp
#include <hpactor/core/actor_ref_cache.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>

using namespace hpactor;

void test_empty_cache() {
    ActorRefCache cache;
    assert(!cache.get(ActorId{1}).has_value());
}

void test_put_and_get() {
    ActorRefCache cache;
    ActorAddress addr{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{1}, 0};
    ActorProxy proxy(addr, nullptr);
    ActorRef ref(std::move(proxy));

    cache.put(ActorId{1}, ref);

    auto result = cache.get(ActorId{1});
    assert(result.has_value());
    assert(!result->is_local());
    assert(result->address().id == ActorId{1});
}

void test_put_updates_existing() {
    ActorRefCache cache;
    ActorAddress addr1{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{1}, 0};
    ActorAddress addr2{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{2}, ActorId{1}, 0};

    ActorProxy proxy1(addr1, nullptr);
    ActorRef ref1(std::move(proxy1));
    cache.put(ActorId{1}, ref1);

    ActorProxy proxy2(addr2, nullptr);
    ActorRef ref2(std::move(proxy2));
    cache.put(ActorId{1}, ref2);  // overwrite

    auto result = cache.get(ActorId{1});
    assert(result.has_value());
    assert(result->address().type == ActorType{2});  // updated
}

void test_eviction_at_max() {
    // Create a small cache for testing
    ActorRefCache cache(3);  // max 3 entries

    for (uint64_t i = 1; i <= 3; ++i) {
        ActorAddress addr{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{i}, 0};
        ActorProxy proxy(addr, nullptr);
        ActorRef ref(std::move(proxy));
        cache.put(ActorId{i}, ref);
    }

    // All 3 should be present
    assert(cache.get(ActorId{1}).has_value());
    assert(cache.get(ActorId{2}).has_value());
    assert(cache.get(ActorId{3}).has_value());

    // Insert 4th — should evict least recently used (id=1, accessed once at insert)
    ActorAddress addr4{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{4}, 0};
    ActorProxy proxy4(addr4, nullptr);
    ActorRef ref4(std::move(proxy4));
    cache.put(ActorId{4}, ref4);

    // id=1 was LRU, should be gone
    assert(!cache.get(ActorId{1}).has_value());
    assert(cache.get(ActorId{2}).has_value());
    assert(cache.get(ActorId{3}).has_value());
    assert(cache.get(ActorId{4}).has_value());
}

void test_lru_access_updates_tick() {
    ActorRefCache cache(3);

    for (uint64_t i = 1; i <= 3; ++i) {
        ActorAddress addr{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{i}, 0};
        ActorProxy proxy(addr, nullptr);
        ActorRef ref(std::move(proxy));
        cache.put(ActorId{i}, ref);
    }

    // Access id=1 (making it recently used), so id=2 becomes LRU
    assert(cache.get(ActorId{1}).has_value());

    // Insert 4th — should evict id=2 (now LRU)
    ActorAddress addr4{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1}, ActorId{4}, 0};
    ActorProxy proxy4(addr4, nullptr);
    ActorRef ref4(std::move(proxy4));
    cache.put(ActorId{4}, ref4);

    assert(cache.get(ActorId{1}).has_value());
    assert(!cache.get(ActorId{2}).has_value());  // evicted
    assert(cache.get(ActorId{3}).has_value());
    assert(cache.get(ActorId{4}).has_value());
}

int main() {
    test_empty_cache();
    test_put_and_get();
    test_put_updates_existing();
    test_eviction_at_max();
    test_lru_access_updates_tick();
    return 0;
}
```

- [ ] **Step 2: Register test in CMakeLists.txt**

Add to `tests/CMakeLists.txt` in the Actor tests section (after line 82, the proto_registry test):

```cmake
add_executable(test_actor_ref_cache actor/test_actor_ref_cache.cpp)
target_link_libraries(test_actor_ref_cache hpactor)
add_test(NAME test_actor_ref_cache COMMAND test_actor_ref_cache)
```

- [ ] **Step 3: Run test to verify it fails**

```bash
cmake -S . -B build -GNinja && ninja -C build
ctest -R test_actor_ref_cache --output-on-failure
```
Expected: FAIL — ActorRefCache not defined.

- [ ] **Step 4: Write minimal ActorRefCache implementation**

Create `include/hpactor/core/actor_ref_cache.hpp`:

```cpp
#pragma once

#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <optional>
#include <unordered_map>

namespace hpactor {

class ActorRefCache {
public:
    explicit ActorRefCache(size_t max_entries = kDefaultMaxEntries)
        : max_entries_(max_entries) {}

    std::optional<ActorRef> get(ActorId id) {
        auto it = cache_.find(id);
        if (it == cache_.end()) {
            return std::nullopt;
        }
        it->second.last_access_tick = ++tick_;
        return it->second.ref;
    }

    void put(ActorId id, ActorRef ref) {
        auto it = cache_.find(id);
        if (it != cache_.end()) {
            it->second.ref = std::move(ref);
            it->second.last_access_tick = ++tick_;
            return;
        }
        if (cache_.size() >= max_entries_) {
            evict_lru();
        }
        cache_.emplace(id, Entry{std::move(ref), ++tick_});
    }

private:
    static constexpr size_t kDefaultMaxEntries = 256;

    struct Entry {
        ActorRef ref;
        uint64_t last_access_tick = 0;
    };

    void evict_lru() {
        ActorId lru_id;
        uint64_t min_tick = UINT64_MAX;
        for (const auto& [id, entry] : cache_) {
            if (entry.last_access_tick < min_tick) {
                min_tick = entry.last_access_tick;
                lru_id = id;
            }
        }
        cache_.erase(lru_id);
    }

    std::unordered_map<ActorId, Entry> cache_;
    uint64_t tick_ = 0;
    size_t max_entries_;
};

} // namespace hpactor
```

- [ ] **Step 5: Run test to verify it passes**

```bash
ninja -C build && ctest -R test_actor_ref_cache --output-on-failure
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/core/actor_ref_cache.hpp tests/actor/test_actor_ref_cache.cpp tests/CMakeLists.txt
git commit -m "feat(core): add ActorRefCache — LRU cache for resolved ActorRefs

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 3: Add transport resolution to ActorSystem and new ActorProxy constructor

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp` — add get_transport_for()
- Modify: `src/actor/actor_system.cpp` — implement get_transport_for()
- Modify: `include/hpactor/ref/actor_proxy.hpp` — add new constructor
- Modify: `src/ref/actor_proxy.cpp` — implement new constructor

- [ ] **Step 1: Add get_transport_for() declaration to ActorSystem**

In `include/hpactor/core/actor_system.hpp`, add after `transport()` at line 168:

```cpp
    // Get or create a TcpTransport for the given remote endpoint.
    // Returns nullptr if networking is not enabled.
    net::Transport* get_transport_for(const CommunicationEndpoint& endpoint);
```

No additional member needed — the existing `transport_` handles all endpoints via its internal `pools_` map (TcpTransport::send() already calls get_or_create_pool() internally).

- [ ] **Step 2: Implement get_transport_for()**

In `src/actor/actor_system.cpp`, add after the existing `transport()` accessor context:

```cpp
net::Transport* ActorSystem::get_transport_for(const CommunicationEndpoint& /*endpoint*/) {
    // TcpTransport already handles per-endpoint routing via its internal pools_
    // map — TcpTransport::send() calls get_or_create_pool(target.endpoint)
    // internally. Return the single transport_ for all remote endpoints.
    if (!config_.enable_network) {
        return nullptr;
    }
    return transport_.get();
}
```

No per-endpoint transport map needed — the existing `TcpTransport::send()` already creates `ConnectionPool` objects on demand via `get_or_create_pool()`.

- [ ] **Step 3: Add new ActorProxy constructor declaration**

In `include/hpactor/ref/actor_proxy.hpp`, add after line 39 (existing constructor):

```cpp
    // Create an actor proxy from address + system (resolves transport internally).
    // Used by ActorContext::resolve() for lazy proxy creation.
    ActorProxy(const ActorAddress& addr, ActorSystem* system);
```

Add forward declaration for `ActorSystem` at the top (after `namespace net { class Transport; }`):

```cpp
class ActorSystem;
```

- [ ] **Step 4: Update ActorProxy::send() to propagate sender_address_**

The existing `ActorProxy::send()` in `src/ref/actor_proxy.cpp` hardcodes `frame.sender = address_` (the proxy's own address). After unification, `TypedMessage` carries `sender_address_` (set by `ActorContext::send()`). `ActorProxy::send()` must use it when available so the receiver's `current_sender_` is correct:

```cpp
void ActorProxy::send(const ActorAddress& target, TypedMessage msg) {
    net::WireFrame frame;
    // Use msg.sender_address() if present, fall back to the proxy address
    frame.sender = msg.sender_address().id != ActorId{0}
                       ? msg.sender_address()
                       : address_;
    frame.receiver = target;
    frame.message_id = MessageId::generate().value();
    frame.type_tag = static_cast<uint32_t>(msg.type_id());
    frame.payload = msg.payload();

    transport_->send(target, frame.encode());
}
```

- [ ] **Step 5: Implement new ActorProxy constructor**

In `src/ref/actor_proxy.cpp`, add:

```cpp
#include <hpactor/core/actor_system.hpp>

ActorProxy::ActorProxy(const ActorAddress& addr, ActorSystem* system)
    : address_(addr),
      transport_(system ? system->get_transport_for(addr.endpoint) : nullptr) {}
```

- [ ] **Step 6: Build and run existing tests**

```bash
ninja -C build && ctest --output-on-failure
```
Expected: all existing 59 tests pass (plus test_actor_ref_cache from Task 2 = 60).

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp include/hpactor/ref/actor_proxy.hpp src/ref/actor_proxy.cpp
git commit -m "feat(net): add get_transport_for() and ActorProxy(system) convenience ctor

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 4: Rewrite ActorContext::send() with resolve() and ActorRef routing

**Files:**
- Modify: `include/hpactor/actor_context.hpp` — add ref_cache_, current_sender_, new signatures
- Modify: `src/actor/actor_context.cpp` — rewrite send(), add resolve()

- [ ] **Step 1: Add new members and declarations to ActorContext header**

In `include/hpactor/actor_context.hpp`:

Add include for ActorRefCache:
```cpp
#include <hpactor/core/actor_ref_cache.hpp>
```

Add `send(ActorRef, TypedMessage)` overload after line 57:

```cpp
    // Primary: send to an already-resolved ActorRef (local or remote)
    void send(const ActorRef& target, TypedMessage msg);
```

Add `resolve()` and accessors in the private section (after line 107):

```cpp
    // Resolve an ActorAddress to an ActorRef (lazy + cached)
    ActorRef resolve(const ActorAddress& target);

    ActorRefCache ref_cache_;
    ActorAddress current_sender_;
```

And add a public accessor for `current_sender_`:

```cpp
    // Get the sender of the current message (for reply routing)
    const ActorAddress& current_sender() const { return current_sender_; }
    void set_current_sender(const ActorAddress& sender) { current_sender_ = sender; }
```

- [ ] **Step 2: Implement resolve() and rewrite send() in .cpp**

Replace the existing `send()`, `send_with_priority()`, `reply()`, and `reply_with_error()` implementations in `src/actor/actor_context.cpp`:

```cpp
#include <hpactor/core/actor_system.hpp>
#include <hpactor/ref/actor_proxy.hpp>

ActorRef ActorContext::resolve(const ActorAddress& target) {
    // 1. Check cache (hot path)
    auto cached = ref_cache_.get(target.id);
    if (cached.has_value()) {
        return *cached;
    }

    // 2. Check local registry
    auto system = system_ ? system_ : (owner_ ? &owner_.get()->system() : nullptr);
    if (!system) {
        return ActorRef{};
    }

    auto actor = system->get_actor(target.id);
    if (actor) {
        ActorRef ref(Actor(actor));
        ref_cache_.put(target.id, ref);
        return ref;
    }

    // 3. Non-loopback endpoint: create ActorProxy
    if (!target.is_local()) {
        ActorProxy proxy(target, system);
        ActorRef ref(std::move(proxy));
        // Only cache if transport was resolved successfully
        if (ref.get_proxy() && ref.get_proxy()->transport()) {
            ref_cache_.put(target.id, ref);
        }
        return ref;
    }

    return ActorRef{};
}

void ActorContext::send(const ActorRef& target, TypedMessage msg) {
    // Stamp sender identity for reply tracking
    if (owner_) {
        msg.set_sender_address(owner_.address());
    }

    target.send(target.address(), std::move(msg));
}

void ActorContext::send(const ActorAddress& target, TypedMessage msg) {
    auto ref = resolve(target);
    if (ref) {
        send(ref, std::move(msg));
    }
    // If resolve failed (no transport, no local actor), silently drop.
    // Matches async messaging semantics — fire and forget.
}

void ActorContext::send(const ActorAddress& target, TypeTag tag,
                        const google::protobuf::Message& proto_msg) {
    TypedMessage msg(tag, proto_msg);
    send(target, std::move(msg));
}

void ActorContext::send_with_priority(const ActorAddress& target, TypedMessage msg,
                                      uint8_t priority, int64_t deadline_ns) {
    auto ref = resolve(target);
    if (!ref) return;

    if (owner_) {
        msg.set_sender_address(owner_.address());
    }

    if (ref.is_local()) {
        auto system = owner_ ? &owner_.get()->system() : system_;
        if (system) {
            system->deliver_local(target.id, std::move(msg), priority, deadline_ns);
        }
    } else {
        ref.send(target.address(), std::move(msg));
    }
}

void ActorContext::reply(TypedMessage msg) {
    if (current_sender_.id != ActorId{0}) {
        send(current_sender_, std::move(msg));
    }
}

void ActorContext::reply(TypeTag tag, const google::protobuf::Message& proto_msg) {
    TypedMessage msg(tag, proto_msg);
    reply(std::move(msg));
}

void ActorContext::reply_with_error(error err) {
    // Placeholder — will be fully implemented in Task 7 (ErrorMsg TypeTag)
    if (current_sender_.id != ActorId{0}) {
        // For now, send error as a simple TypedMessage with error details
        (void)err;
    }
}
```

- [ ] **Step 3: Build and fix any compilation errors**

```bash
ninja -C build
```
Expected: compilation succeeds.

- [ ] **Step 4: Run existing tests (especially test_actor_context)**

```bash
ctest -R test_actor_context --output-on-failure
ctest --output-on-failure
```
Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/actor_context.hpp src/actor/actor_context.cpp
git commit -m "refactor(actor): rewrite ActorContext::send() with resolve() and ActorRef routing

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 5: Write ActorContext send/reply behavioral tests

**Files:**
- Modify: `tests/actor/test_actor_context.cpp` — add new tests

- [ ] **Step 1: Add tests to test_actor_context.cpp**

Append to `tests/actor/test_actor_context.cpp` (before `main()`):

```cpp
#include <hpactor/core/actor_ref_cache.hpp>
#include <hpactor/core/actor_system.hpp>

void test_actor_context_send_with_actor_ref() {
    // Create a minimal ActorSystem for testing
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor = system.spawn([](ActorContext&) {});
    ActorContext ctx(actor, &system);

    // send via ActorRef
    ActorRef target_ref(actor);
    TypedMessage msg(TypeTag::User, bytes{1, 2, 3});
    ctx.send(target_ref, std::move(msg));

    // Message should be in the actor's mailbox
    auto* mailbox = system.get_mailbox(actor.id());
    assert(mailbox != nullptr);
    // After send, mailbox should be non-empty (actor hasn't processed yet)
}

void test_actor_context_send_sets_sender_address() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto sender = system.spawn([](ActorContext&) {});
    auto target = system.spawn([](ActorContext&) {});

    ActorContext ctx(sender, &system);

    TypedMessage msg(TypeTag::User, bytes{42});
    ctx.send(ActorRef(target), std::move(msg));

    // Pop from target's mailbox and check sender_address_
    auto* mailbox = system.get_mailbox(target.id());
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.sender_address().id == sender.id());
}

void test_actor_context_resolve_local() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor = system.spawn([](ActorContext&) {});
    ActorContext ctx(actor, &system);

    ActorRef ref = ctx.resolve(actor.address());
    assert(ref);
    assert(ref.is_local());
    assert(ref.address().id == actor.id());
}

void test_actor_context_resolve_remote() {
    Config config;
    config.enable_network = true;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor = system.spawn([](ActorContext&) {});
    ActorContext ctx(actor, &system);

    // Create a remote address (non-loopback)
    auto remote_ep = endpoint_ops::parse_endpoint("10.0.0.1:12345");
    ActorAddress remote_addr{remote_ep, ActorType{1}, ActorId{42}, 0};

    ActorRef ref = ctx.resolve(remote_addr);
    assert(ref);
    assert(!ref.is_local());  // Should create an ActorProxy
    assert(ref.address().id == ActorId{42});
}

void test_actor_context_reply() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor_a = system.spawn([](ActorContext&) {});
    auto actor_b = system.spawn([](ActorContext&) {});

    ActorContext ctx(actor_a, &system);
    ctx.set_current_sender(actor_b.address());

    TypedMessage reply_msg(TypeTag::User, bytes{99});
    ctx.reply(std::move(reply_msg));

    // Reply should go to actor_b's mailbox
    auto* mailbox = system.get_mailbox(actor_b.id());
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.sender_address().id == actor_a.id());
}

void test_actor_context_reply_no_sender() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor = system.spawn([](ActorContext&) {});
    ActorContext ctx(actor, &system);
    // No current_sender_ set — reply should be no-op, not crash
    TypedMessage reply_msg(TypeTag::User, bytes{99});
    ctx.reply(std::move(reply_msg));
    // Test passes if we reach here without crashing
}
```

Update `main()` to call the new tests:
```cpp
int main() {
    test_actor_context_children();
    test_actor_context_linked_actors();
    test_actor_context_monitor();
    test_actor_context_remote_children();
    test_actor_context_send_with_actor_ref();
    test_actor_context_send_sets_sender_address();
    test_actor_context_resolve_local();
    test_actor_context_resolve_remote();
    test_actor_context_reply();
    test_actor_context_reply_no_sender();
    return 0;
}
```

Note: The `resolve()` method and `set_current_sender()` need to be public for testing. Either make them public or friend the test. The simplest approach: make `resolve()` and `set_current_sender()` public in the header (they're already declared in the private section in step 1 — move them to public).

- [ ] **Step 2: Move resolve() and set_current_sender() to public in the header**

In `include/hpactor/actor_context.hpp`, move the declarations added in Task 4 Step 1 from private to public:

```cpp
    // Resolve an ActorAddress to an ActorRef (lazy + cached)
    ActorRef resolve(const ActorAddress& target);

    // Sender tracking for reply routing
    const ActorAddress& current_sender() const { return current_sender_; }
    void set_current_sender(const ActorAddress& sender) { current_sender_ = sender; }
```

Keep `ref_cache_` and `current_sender_` in private.

- [ ] **Step 3: Build and run the new tests**

```bash
ninja -C build && ctest -R test_actor_context --output-on-failure
```
Expected: all tests pass.

- [ ] **Step 4: Run full test suite**

```bash
ctest --output-on-failure
```
Expected: all 60 tests pass (59 original + test_actor_ref_cache; test_actor_context is modified but no new test target).

- [ ] **Step 5: Commit**

```bash
git add tests/actor/test_actor_context.cpp include/hpactor/actor_context.hpp
git commit -m "test(actor): add send/resolve/reply behavioral tests for ActorContext

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 6: Implement deliver_remote() and wire on_frame_received()

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp` — add deliver_remote() declaration
- Modify: `src/actor/actor_system.cpp` — implement deliver_remote()
- Modify: `src/net/connection_pool.cpp` — wire on_frame_received() to deliver_remote()

- [ ] **Step 1: Add include and deliver_remote() declaration**

In `include/hpactor/core/actor_system.hpp`, add the include for `WireFrame` (needed for the `deliver_remote` signature):

```cpp
#include <hpactor/net/frame.hpp>
```

Then add after `deliver_local()` at line 161:

```cpp
    // Deliver a remote message (from WireFrame) to the target actor's mailbox.
    // Bridges the transport layer to the unified deliver_local() sink.
    void deliver_remote(const net::WireFrame& frame);
```

- [ ] **Step 2: Add actor_message_handler to ConnectionPool (declaration only)**

In `include/hpactor/net/connection_pool.hpp`, near the top add a forward declaration for `WireFrame` (since the callback references it by const-ref):

```cpp
struct WireFrame;  // forward decl, full def in <hpactor/net/frame.hpp>
```

Then add to ConnectionPool's public section (alongside existing `rpc_handler_`/`spawn_handler_` setters):

```cpp
    using actor_message_handler = std::function<void(const WireFrame& frame)>;

    void set_actor_message_handler(actor_message_handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        actor_message_handler_ = std::move(handler);
    }
```

And in the private section (alongside existing handler fields):
```cpp
    actor_message_handler actor_message_handler_;
```

- [ ] **Step 3: Implement deliver_remote()**

In `src/actor/actor_system.cpp`, add:

```cpp
void ActorSystem::deliver_remote(const net::WireFrame& frame) {
    TypedMessage msg(static_cast<TypeTag>(frame.type_tag), frame.payload);
    msg.set_sender_address(frame.sender);
    deliver_local(frame.receiver.id, std::move(msg));
}
```

- [ ] **Step 4: Wire on_frame_received() to call deliver_remote()**

In `src/net/connection_pool.cpp`, update `on_frame_received()` — replace the TODO at lines 223-224:

Replace:
```cpp
    // TODO: existing actor message handling
    (void)frame;
```

With:
```cpp
    // Route to actor message handler (deliver_remote)
    if (actor_message_handler_) {
        actor_message_handler_(frame);
    }
```

- [ ] **Step 5: Wire ConnectionPool → ActorSystem for deliver_remote()**

Now propagate the callback through TcpTransport to reach each ConnectionPool. (The `on_frame_received()` edit was done in Step 4 — this step handles the TcpTransport propagation and ActorSystem wiring.)

In `include/hpactor/net/tcp_transport.hpp`, add to TcpTransport:
```cpp
    void set_actor_message_handler(std::function<void(const net::WireFrame&)> h) {
        actor_msg_handler_ = std::move(h);
    }
```
And in the private section: `std::function<void(const net::WireFrame&)> actor_msg_handler_;`

In `src/net/tcp_transport.cpp`, in `get_or_create_pool()`, after creating a pool add:
```cpp
    if (actor_msg_handler_) {
        pool->set_actor_message_handler(actor_msg_handler_);
    }
```

In `ActorSystem` constructor in `src/actor/actor_system.cpp`, after `transport_` creation (line 75-77):
```cpp
    transport_->set_actor_message_handler(
        [this](const net::WireFrame& frame) {
            this->deliver_remote(frame);
        });
```

- [ ] **Step 6: Build**

```bash
ninja -C build
```
Expected: clean build.

- [ ] **Step 7: Run tests**

```bash
ctest --output-on-failure
```
Expected: all 60 tests pass.

- [ ] **Step 8: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp src/net/connection_pool.cpp include/hpactor/net/connection_pool.hpp include/hpactor/net/tcp_transport.hpp src/net/tcp_transport.cpp
git commit -m "feat(net): implement deliver_remote() bridge and wire on_frame_received()

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 7: Set current_sender_ in EventBasedActor::receive()

**Files:**
- Modify: `src/actor/event_based_actor.cpp`

- [ ] **Step 1: Capture sender in receive()**

In `src/actor/event_based_actor.cpp`, update the `receive()` method (line 27) to capture the sender. Add after line 30 (`initialize_proto_handlers()`):

```cpp
    // Capture sender for reply() tracking
    auto* ctx = context();
    if (ctx) {
        ctx->set_current_sender(msg.sender_address());
    }
```

Also resolve the TODO at line 38 (`(void)response; // TODO: integrate with reply routing`). The response from `on_request<>()` can now be routed via `context()->reply()`:

Replace line 38:
```cpp
            (void)response; // TODO: integrate with reply routing
```
With:
```cpp
            if (!response.empty() && ctx) {
                TypedMessage reply_msg(it->first, response);
                ctx->reply(std::move(reply_msg));
            }
```

And fix the same TODO in `on_proto_message()` at line 78 similarly.

- [ ] **Step 2: Build and run tests**

```bash
ninja -C build && ctest --output-on-failure
```
Expected: all 60 tests pass.

- [ ] **Step 3: Commit**

```bash
git add src/actor/event_based_actor.cpp
git commit -m "feat(actor): capture current_sender_ in receive(), wire reply routing

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 8: Add ErrorMsg TypeTag and implement reply_with_error()

**Files:**
- Modify: `include/hpactor/types/types.hpp` — add ErrorMsg = 7
- Modify: `src/actor/actor_context.cpp` — implement reply_with_error() body

- [ ] **Step 1: Add ErrorMsg to TypeTag**

In `include/hpactor/types/types.hpp`, add after line 441 (`SpawnResponseTag = 6`):

```cpp
    ErrorMsg = 7,
```

- [ ] **Step 2: Implement reply_with_error()**

In `src/actor/actor_context.cpp`, replace the placeholder `reply_with_error()`. Use a minimal wire format: big-endian 4-byte error code followed by the error message string. A full protobuf error message definition is deferred (the `ErrorMsg` TypeTag reserves the dispatch slot; the payload format can be upgraded later without changing the TypeTag):

```cpp
void ActorContext::reply_with_error(error err) {
    if (current_sender_.id == ActorId{0}) return;

    // Wire format: [4 bytes: error code BE][error message string]
    // A protobuf error message can replace this payload later without
    // changing the TypeTag or dispatch path.
    bytes payload;
    uint32_t code = err.code();
    payload.push_back(static_cast<uint8_t>((code >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((code >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code & 0xFF));
    const auto& msg = err.message();
    payload.insert(payload.end(), msg.begin(), msg.end());

    TypedMessage error_msg(TypeTag::ErrorMsg, std::move(payload));
    send(current_sender_, std::move(error_msg));
}
```

- [ ] **Step 3: Build and run tests**

```bash
ninja -C build && ctest --output-on-failure
```
Expected: all 60 tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/types/types.hpp src/actor/actor_context.cpp
git commit -m "feat(types): add ErrorMsg TypeTag, implement reply_with_error()

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 9: Integration tests — unified message passing

**Files:**
- Create: `tests/actor/test_unified_message_passing.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write integration test**

Create `tests/actor/test_unified_message_passing.cpp`:

```cpp
#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/core/actor_ref_cache.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/net/frame.hpp>

#include <cassert>
#include <string>

using namespace hpactor;

// Test: deliver_remote() correctly bridges WireFrame → TypedMessage → deliver_local()
void test_deliver_remote_bridge() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    // Spawn a target actor
    auto target = system.spawn([](ActorContext&) {});

    // Build a simulated remote WireFrame
    net::WireFrame frame;
    frame.sender = ActorAddress{endpoint_ops::parse_endpoint("10.0.0.1:9999"),
                                ActorType{1}, ActorId{99}, 0};
    frame.receiver = target.address();
    frame.type_tag = static_cast<uint32_t>(TypeTag::User);
    frame.payload = bytes{1, 3, 3, 7};  // pretend protobuf payload

    system.deliver_remote(frame);

    // Message should be in target's mailbox
    auto* mailbox = system.get_mailbox(target.id());
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.type_id() == TypeTag::User);
    assert(received.payload().size() == 4);
    assert(received.sender_address().id == ActorId{99});
    assert(received.sender_address().endpoint == frame.sender.endpoint);
}

// Test: full local send → mailbox → receive → reply loop
void test_unified_send_reply_loop() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto alice = system.spawn([](ActorContext&) {});
    auto bob = system.spawn([](ActorContext&) {});

    // Alice sends to Bob
    ActorContext alice_ctx(alice, &system);
    TypedMessage msg(TypeTag::User, bytes{42});
    alice_ctx.send(ActorRef(bob), std::move(msg));

    // Bob receives
    auto* bob_mailbox = system.get_mailbox(bob.id());
    assert(bob_mailbox != nullptr);
    TypedMessage received;
    bool popped = bob_mailbox->try_pop(received);
    assert(popped);
    assert(received.sender_address().id == alice.id());

    // Bob's context captures sender, then replies
    ActorContext bob_ctx(bob, &system);
    bob_ctx.set_current_sender(received.sender_address());

    TypedMessage reply_msg(TypeTag::User, bytes{24});
    bob_ctx.reply(std::move(reply_msg));

    // Alice should receive the reply
    auto* alice_mailbox = system.get_mailbox(alice.id());
    assert(alice_mailbox != nullptr);
    TypedMessage reply_received;
    popped = alice_mailbox->try_pop(reply_received);
    assert(popped);
    assert(reply_received.sender_address().id == bob.id());
    assert(reply_received.payload()[0] == 24);
}

// Test: reply_with_error sends ErrorMsg TypeTag
void test_reply_with_error() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto alice = system.spawn([](ActorContext&) {});
    auto bob = system.spawn([](ActorContext&) {});

    ActorContext bob_ctx(bob, &system);
    bob_ctx.set_current_sender(alice.address());
    bob_ctx.reply_with_error(error(42, "something went wrong"));

    auto* alice_mailbox = system.get_mailbox(alice.id());
    assert(alice_mailbox != nullptr);
    TypedMessage reply_received;
    bool popped = alice_mailbox->try_pop(reply_received);
    assert(popped);
    assert(reply_received.type_id() == TypeTag::ErrorMsg);
    // First 4 bytes of payload = error code 42 in big-endian
    assert(reply_received.payload().size() >= 4);
    assert(reply_received.payload()[0] == 0);
    assert(reply_received.payload()[1] == 0);
    assert(reply_received.payload()[2] == 0);
    assert(reply_received.payload()[3] == 42);
    assert(reply_received.sender_address().id == bob.id());
}

// Test: send to self works correctly
void test_send_to_self() {
    Config config;
    config.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    ActorSystem system(config);

    auto actor = system.spawn([](ActorContext&) {});
    ActorContext ctx(actor, &system);

    TypedMessage msg(TypeTag::User, bytes{7});
    ctx.send(ActorRef(actor), std::move(msg));

    auto* mailbox = system.get_mailbox(actor.id());
    assert(mailbox != nullptr);
    TypedMessage received;
    bool popped = mailbox->try_pop(received);
    assert(popped);
    assert(received.sender_address().id == actor.id());
}

int main() {
    test_deliver_remote_bridge();
    test_unified_send_reply_loop();
    test_reply_with_error();
    test_send_to_self();
    return 0;
}
```

- [ ] **Step 2: Register test in CMakeLists.txt**

Add to the Actor tests section in `tests/CMakeLists.txt`:

```cmake
add_executable(test_unified_message_passing actor/test_unified_message_passing.cpp)
target_link_libraries(test_unified_message_passing hpactor hpactor_proto pthread)
add_test(NAME test_unified_message_passing COMMAND test_unified_message_passing)
```

- [ ] **Step 3: Build and run integration tests**

```bash
ninja -C build && ctest -R test_unified_message_passing --output-on-failure
```
Expected: all 4 subtests PASS.

- [ ] **Step 4: Run full test suite**

```bash
ctest --output-on-failure
```
Expected: all 61 tests pass (60 previous + test_unified_message_passing).

- [ ] **Step 5: Commit**

```bash
git add tests/actor/test_unified_message_passing.cpp tests/CMakeLists.txt
git commit -m "test: add unified message passing integration tests

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 10: Final verification and cleanup

- [ ] **Step 1: Build with sanitizers**

```bash
cmake -DENABLE_ASAN=ON -S . -B build_asan -GNinja && ninja -C build_asan && ctest --test-dir build_asan --output-on-failure
```
Expected: tests pass. Known false positives in `test_mailbox_awaiter` and `test_priority_scheduler` are pre-existing.

- [ ] **Step 2: Final clean build check**

```bash
cmake -S . -B build -GNinja && ninja -C build && ctest --output-on-failure
```
Expected: 61/61 tests pass, clean build.

- [ ] **Step 3: Commit any remaining changes**

```bash
git add -A
git commit -m "chore: final verification, all 61 tests passing

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```
