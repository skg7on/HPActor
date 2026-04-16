# Network Event Loop Integration Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire EventLoop I/O completions into the actor scheduling subsystem by implementing ActorSystem::enqueue_completion() and fixing the network thread loop.

**Architecture:** I/O completions from EventLoop are delivered as CompletionMessage objects to the target actor's mailbox via ActorSystem::deliver_local(). The scheduler is notified automatically via notify_ready() inside deliver_local().

**Tech Stack:** C++20, no exceptions, no RTTI

---

## Files Modified

| File | Change |
|------|--------|
| `include/hpactor/actor/abstract_actor.hpp` | Add CompletionMessage struct; add to MessageVariant |
| `include/hpactor/core/actor_system.hpp` | Add is_running() method; add CompletionMessage forward declaration |
| `src/actor/actor_system.cpp` | Fix network thread loop; implement enqueue_completion(); set actor system on event loop |

---

## Task 1: Add CompletionMessage to MessageVariant

**Files:**
- Modify: `include/hpactor/actor/abstract_actor.hpp`
- No new tests required (structural change only)

- [ ] **Step 1: Read current abstract_actor.hpp**

```bash
cat include/hpactor/actor/abstract_actor.hpp
```

- [ ] **Step 2: Add CompletionMessage struct before MessageVariant**

After line 46 (after `unlink_msg` struct), add:

```cpp
// CompletionMessage carries I/O completion data from EventLoop to actors
struct CompletionMessage {
    ActorId actor;       // target actor that initiated the operation
    OpType  type;        // Send, Recv, Accept, Connect, TimerFired, RecvFrom, SendTo
    int     fd;          // file descriptor
    int     result;       // bytes transferred (>= 0) or -errno on failure
    uint64_t user_data;  // original user data from the async operation
};
```

Note: `OpType` is in `hpactor::net` namespace and is defined in `async_io_backend.hpp`. We need to forward-declare or include it. Since `abstract_actor.hpp` currently only includes `ref/actor_address.hpp` and `types/types.hpp`, we need to check if `OpType` is accessible.

- [ ] **Step 3: Check OpType accessibility**

```bash
grep -n "namespace\|OpType" include/hpactor/net/async_io_backend.hpp | head -20
```

If `OpType` is in `hpactor::net` namespace, add a forward declaration at the top of `abstract_actor.hpp` after the includes:

```cpp
namespace hpactor::net {
enum class OpType : uint32_t;
}  // namespace hpactor::net
```

- [ ] **Step 4: Add CompletionMessage to MessageVariant**

Update the MessageVariant definition to include CompletionMessage:

```cpp
using MessageVariant = std::variant<
    CompletionMessage,
    down_msg,
    exit_msg,
    link_msg,
    unlink_msg
    // ... user-defined types
>;
```

- [ ] **Step 5: Verify compilation**

```bash
ninja -C build 2>&1 | head -30
# Expected: errors if OpType forward declaration is missing or misplaced
```

---

## Task 2: Add is_running() to ActorSystem

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Read actor_system.hpp**

```bash
cat include/hpactor/core/actor_system.hpp
```

- [ ] **Step 2: Add is_running() method to ActorSystem class**

Find the `is_running()` declaration (if exists) or add it near other status methods. The class already has `running_` as `std::atomic<bool>`. Add:

```cpp
// Check if actor system is running
bool is_running() const { return running_.load(std::memory_order_acquire); }
```

- [ ] **Step 3: Verify compilation**

```bash
ninja -C build 2>&1 | head -20
```

---

## Task 3: Fix Network Thread Loop and Implement enqueue_completion()

**Files:**
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Read current actor_system.cpp**

```bash
cat src/actor/actor_system.cpp
```

- [ ] **Step 2: Fix network thread loop**

Find the network thread creation (around lines 82-86):

```cpp
// Start network event loop in background thread
network_thread_ = std::thread([this]() {
    while (network_loop_->wait(100) >= 0) {
        // Process events until stopped
    }
});
```

Replace with:

```cpp
// Start network event loop in background thread
network_thread_ = std::thread([this]() {
    while (network_loop_->wait(100) >= 0) {
        network_loop_->process_completions();
        if (!is_running()) break;
    }
});
```

- [ ] **Step 3: Set actor system on event loop before starting network thread**

After `network_loop_` is created (around line 61), add:

```cpp
network_loop_->set_actor_system(this);
```

This should be added right after `network_loop_ = std::make_unique<net::EventLoop>();` so that when the network thread starts processing completions, it can route them to the actor system.

- [ ] **Step 4: Implement enqueue_completion()**

Find the stub implementation (around lines 188-193):

```cpp
void ActorSystem::enqueue_completion(net::OpCompletion completion) {
    // TODO: Route completion to the actor's mailbox as a CompletionMessage
    // This is Phase 5.5+ work - for now completions are handled via
    // EventLoop's timer callback bridging
    (void)completion;
}
```

Replace with:

```cpp
void ActorSystem::enqueue_completion(net::OpCompletion completion) {
    CompletionMessage msg;
    msg.actor = completion.actor;
    msg.type = completion.type;
    msg.fd = completion.fd;
    msg.result = completion.result;
    msg.user_data = completion.user_data;

    deliver_local(completion.actor, std::move(msg));
}
```

- [ ] **Step 5: Verify compilation**

```bash
ninja -C build 2>&1 | head -50
# Expected: no errors
```

---

## Task 4: Build and Test

- [ ] **Step 1: Full build**

```bash
ninja -C build 2>&1
# Expected: BUILD SUCCEEDED
```

- [ ] **Step 2: Run tests**

```bash
ctest --output-on-failure
# Expected: all tests pass
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/actor/abstract_actor.hpp include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "$(cat <<'EOF'
feat(net): wire I/O completions into actor scheduling

CompletionMessage added to MessageVariant.
ActorSystem::enqueue_completion() now delivers OpCompletion
as a message to the target actor's mailbox.
Network thread loop now calls process_completions().
EventLoop set_actor_system() called before network thread starts.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Verification Checklist

- [ ] Task 1: CompletionMessage struct compiles and is in MessageVariant
- [ ] Task 2: is_running() method accessible
- [ ] Task 3: Network thread loop calls process_completions()
- [ ] Task 3: enqueue_completion() creates CompletionMessage and calls deliver_local()
- [ ] Task 3: set_actor_system() is called before network thread starts
- [ ] Task 4: Full build succeeds
- [ ] Task 4: All tests pass
- [ ] Task 4: Changes committed
