# Network Event Loop Integration Design

**Date:** 2026-04-16
**Status:** Draft
**Goal:** Wire EventLoop I/O completions into the actor scheduling subsystem

## Overview

The `ActorSystem` constructor starts a background network thread that calls `EventLoop::wait()`, but the loop body is empty — it never calls `process_completions()` to drain the completion queue. Additionally, `ActorSystem::enqueue_completion()` is an empty stub. This means I/O completions (send, recv, accept, connect, timer) are never routed to actors.

After this design is implemented, I/O completions will be delivered as `CompletionMessage` objects to the target actor's mailbox, and the scheduler will be notified to wake the actor.

## Message Type

### CompletionMessage

Added to `abstract_actor.hpp`:

```cpp
struct CompletionMessage {
    ActorId actor;       // target actor that initiated the operation
    OpType  type;        // Send, Recv, Accept, Connect, TimerFired, RecvFrom, SendTo
    int     fd;          // file descriptor
    int     result;      // bytes transferred (>= 0) or -errno on failure
    uint64_t user_data;  // original user data from the async operation
};
```

Added to `MessageVariant` so it can be delivered via `deliver_local()`.

## ActorSystem Changes

### 1. Expose running flag

In `actor_system.hpp`, add a method to check if the actor system is running:

```cpp
bool is_running() const { return running_.load(std::memory_order_acquire); }
```

### 2. Network thread loop

The network thread loop in `ActorSystem::ActorSystem()` is fixed:

```cpp
network_thread_ = std::thread([this]() {
    while (network_loop_->wait(100) >= 0) {
        network_loop_->process_completions();
        if (!is_running()) break;
    }
});
```

### 3. Implement enqueue_completion

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

The `EventLoop::enqueue_completion()` already calls `ActorSystem::enqueue_completion()` when it has an `actor_system_` set (see `event_loop.cpp:276-278`). This means completions automatically route to the actor system after `set_actor_system()` is called.

### 4. Order of operations

In `ActorSystem::ActorSystem()`:
1. Create `scheduler_` and call `scheduler_->start()`
2. Create `network_loop_`
3. Set `network_loop_->set_actor_system(this)`
4. Create `transport_` and `registrar_`
5. Start network thread
6. Spawn system actors

## Files Modified

| File | Change |
|------|--------|
| `include/hpactor/actor/abstract_actor.hpp` | Add `CompletionMessage` struct; add to `MessageVariant` |
| `include/hpactor/core/actor_system.hpp` | Add `is_running()` method; add forward declaration for `CompletionMessage` |
| `src/actor/actor_system.cpp` | Fix network thread loop; implement `enqueue_completion()`; set actor system on event loop |

## Implementation Notes

- `OpType` is already defined in `async_io_backend.hpp` and is accessible via `net::OpType`
- `deliver_local()` already handles looking up the mailbox and enqueueing the message
- `scheduler_->notify_ready()` is called inside `deliver_local()` — no extra wake-up call needed
- Timer completions (`OpType::TimerFired`) are handled separately by `EventLoop::deliver_timer_completion()` and do not go through `ActorSystem::enqueue_completion()`

## Testing

After implementation:
1. Build: `ninja -C build`
2. Run tests: `ctest --output-on-failure`
3. Integration test: spawn an actor that initiates network I/O and verify it receives completion messages
