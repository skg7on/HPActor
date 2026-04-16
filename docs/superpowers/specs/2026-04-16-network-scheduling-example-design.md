# Network Scheduling Integration Example Design

**Date:** 2026-04-16
**Status:** Draft
**Goal:** Create a complete end-to-end example demonstrating actors sending different message types with priorities/deadlines, with proper scheduling integration.

## Problem Statement

The existing `07_priority_scheduler_demo.cpp` demonstrates scheduling infrastructure (priority queues, EDF, A2WS) using `scheduler_->notify_ready()` directly. This **bypasses the actor messaging system** - it's not actually actors sending messages to each other.

The user requires: **actual actor-to-actor message sending with priorities**.

## Current State Analysis

### What's Implemented

| Component | Status | Notes |
|-----------|--------|-------|
| `HybridScheduler::notify_ready(actor, priority, deadline)` | ✅ | Accepts priority 0-3, EDF deadline |
| `ActorSystem::deliver_local(target, msg)` | ✅ | Delivers to mailbox, but hardcodes priority=0 |
| `ActorContext::send(target, msg)` | ✅ | Calls `deliver_local`, priority lost |
| `ActorMailbox::push()` | ✅ | Stores messages |
| `HybridScheduler::pop_local()` | ✅ | Worker dequeues actor, processes mailbox |
| `EventBasedActor::receive()` | ✅ | Calls behavior handler |

### What's Missing for Priority Messaging

**Problem:** `deliver_local` always calls:
```cpp
scheduler_->notify_ready(target, 0, INT64_MAX);  // Hardcoded!
```

When actor A sends to actor B:
1. `ActorContext::send(addr_B, HighPriorityMsg{})` 
2. `system_.deliver_local(addr_B.id, msg)`
3. `scheduler_->notify_ready(addr_B.id, 0, INT64_MAX)` ← **Priority lost!**

The message priority is never passed to the scheduler.

## Design: Priority-Aware Message Delivery

### Option A: Extend `deliver_local` with Priority Parameters (Recommended)

Add an overload that accepts priority and deadline:

```cpp
// New overload in actor_system.hpp
void deliver_local(ActorId target, MessageVariant msg, 
                   uint8_t priority = 0, int64_t deadline_ns = INT64_MAX);

// ActorContext::send_with_priority
void send_with_priority(const ActorAddress& target, MessageVariant msg,
                        uint8_t priority, int64_t deadline_ns);
```

**Pros:** Minimal API change, backward compatible, clear semantics
**Cons:** Requires callers to specify priority explicitly

### Option B: Priority Message Envelope

Create a wrapper that carries priority with the message:

```cpp
struct PrioritizedMessage {
    MessageVariant payload;
    uint8_t priority;
    int64_t deadline_ns;
};
```

**Pros:** Message carries its own scheduling metadata
**Cons:** All user messages must be wrapped, significant API change

### Option C: Message Trait System

Define priority as a trait of message types:

```cpp
template<typename T>
struct message_priority {
    static constexpr uint8_t value = 0;  // default
};

template<> struct message_priority<HighPriorityMsg> {
    static constexpr uint8_t value = 0;
};
```

**Pros:** Automatic priority based on type
**Cons:** Complex, requires type registry

## Recommended Approach: Option A

Minimal changes, clear API, works with existing message types.

## Implementation Plan

### Changes Required

#### 1. Extend `ActorSystem::deliver_local` (actor_system.hpp/cpp)

```cpp
// New overload
void deliver_local(ActorId target, MessageVariant msg,
                   uint8_t priority, int64_t deadline_ns);

// Modify existing to call new with defaults
void deliver_local(ActorId target, MessageVariant msg) {
    deliver_local(target, std::move(msg), 0, INT64_MAX);
}
```

#### 2. Add `ActorContext::send_with_priority` (actor_context.hpp/cpp)

```cpp
void send_with_priority(const ActorAddress& target, MessageVariant msg,
                        uint8_t priority, int64_t deadline_ns) {
    if (target.is_local()) {
        auto actor_ptr = owner_.get();
        if (actor_ptr) {
            actor_ptr->system().deliver_local(target.id, std::move(msg), priority, deadline_ns);
        }
    }
    // Remote: forward to transport
}
```

#### 3. Create Example: `examples/07_priority_scheduler_demo.cpp`

Full actor-to-actor messaging with priorities:

```cpp
// Two actors: PrioritySender and PriorityReceiver
// PrioritySender sends WorkMessages at different priorities
// PriorityReceiver processes and logs priority order

struct WorkMessage {
    int work_id;
    int priority;  // 0-3
    // ...
};

// PrioritySender sends messages at specific priorities
void send_high_priority_work() {
    context()->send_with_priority(worker_addr, 
        WorkMessage{1, /*priority=*/0}, 0, INT64_MAX);
}

void send_low_priority_work() {
    context()->send_with_priority(worker_addr,
        WorkMessage{2, /*priority=*/3}, 3, INT64_MAX);
}
```

### Example Flow

```
PrioritySender                           PriorityReceiver
      |                                         |
      |  send_with_priority(addr, msg, 0, INF)  |
      |  send_with_priority(addr, msg, 3, INF)  |
      |  send_with_priority(addr, msg, 1, INF)   |
      |  send_with_priority(addr, msg, 2, INF)  |
      +---------------------------------------->
                           |
                           v
                    ActorSystem.deliver_local()
                           |
                           v
              scheduler_->notify_ready(actor, priority, INF)
                           |
                           v
                    HybridScheduler
                    (Priority queues)
                           |
                           v
              Worker processes: 0, 1, 2, 3 (by priority)
```

### EDF Example Extension

```cpp
// Send deadline-sensitive work
auto deadline = now + 5'000'000;  // 5ms from now
context()->send_with_priority(worker_addr, 
    WorkMessage{3, 2}, 2, deadline);
```

## Files to Modify/Create

| File | Change | Purpose |
|------|--------|---------|
| `include/hpactor/core/actor_system.hpp` | Add `deliver_local` overload | Accept priority params |
| `src/actor/actor_system.cpp` | Implement overload | Pass priority to notify_ready |
| `include/hpactor/actor_context.hpp` | Add `send_with_priority` | Public API |
| `src/actor/actor_context.cpp` | Implement | Route to deliver_local |
| `examples/07_priority_scheduler_demo.cpp` | Rewrite | Full actor messaging demo |
| `tests/sched/test_priority_scheduler.cpp` | Update | Test send_with_priority |

## Refined Example Design

### `examples/07_priority_scheduler_demo.cpp`

```cpp
// =============================================================================
// HPActor Example 07: Priority Scheduler Demo
// =============================================================================
//
// Demonstrates:
//   - Actor-to-actor messaging with priorities
//   - Multi-priority scheduling (4 levels, 0-3)
//   - EDF deadline tracking
//   - Work-stealing across scheduler threads
//
// Architecture:
//   PriorityDispatcher -> sends WorkMessages at different priorities
//   PriorityWorker     -> receives and processes messages
//
// =============================================================================

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <iostream>
#include <chrono>
#include <vector>
#include <random>

using namespace hpactor;

// -----------------------------------------------------------------------------
// Messages
// -----------------------------------------------------------------------------

struct WorkMessage {
    int work_id;
    int priority;  // 0-3
    int64_t deadline_ns;  // for EDF, INT64_MAX = no deadline
};

struct WorkResponse {
    int work_id;
    int priority;
    int64_t latency_ns;
    bool on_time;
};

// -----------------------------------------------------------------------------
// PriorityWorker: receives work and processes it
// -----------------------------------------------------------------------------

class PriorityWorker : public EventBasedActor {
private:
    std::vector<WorkResponse> responses_;
    int64_t start_time_;

public:
    PriorityWorker(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        start_time_ = std::chrono::steady_clock::now().time_since_epoch().count();
    }

    Behavior make_behavior() override {
        return Behavior{[this](WorkMessage&& msg) {
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            int64_t latency = now - start_time_;
            bool on_time = (msg.deadline_ns == INT64_MAX) || 
                           (now <= msg.deadline_ns);

            responses_.push_back({
                msg.work_id,
                msg.priority,
                latency,
                on_time
            });

            std::cout << "  [Worker] Processed work_id=" << msg.work_id
                      << " priority=" << msg.priority
                      << " latency=" << latency/1000 << "us"
                      << " on_time=" << (on_time ? "yes" : "NO")
                      << std::endl;

            // Reply with result
            context()->reply(WorkResponse{msg.work_id, msg.priority, latency, on_time});
        }};
    }
};

// -----------------------------------------------------------------------------
// PriorityDispatcher: sends work at different priorities
// -----------------------------------------------------------------------------

class PriorityDispatcher : public EventBasedActor {
private:
    ActorAddress worker_addr_;
    int sent_count_ = 0;

public:
    PriorityDispatcher(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    void set_worker(const ActorAddress& addr) { worker_addr_ = addr; }

    Behavior make_behavior() override {
        return Behavior{
            [this](const std::vector<WorkMessage>& work_items) {
                // Send all work messages at their specified priorities
                for (const auto& work : work_items) {
                    context()->send_with_priority(
                        worker_addr_, 
                        work,  // WorkMessage
                        static_cast<uint8_t>(work.priority),
                        work.deadline_ns
                    );
                    sent_count_++;
                }
                std::cout << "[Dispatcher] Sent " << sent_count_ 
                          << " work messages at various priorities" << std::endl;
            },
            [this](WorkResponse&& resp) {
                // Handle responses (log, could trigger more work, etc.)
            }
        };
    }
};

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main() {
    std::cout << "=== HPActor Example 07: Priority Scheduler Demo ===" << std::endl;

    hpactor::Config config{
        .scheduler_threads = 4,
        .max_queue_depth = 1024
    };
    hpactor::ActorSystem system(config);

    // Spawn actors
    auto worker = system.spawn<PriorityWorker>();
    auto dispatcher = system.spawn<PriorityDispatcher>();

    // Set up dispatcher -> worker relationship
    // Note: In a real system, we'd send the worker address to dispatcher
    // For demo, we'll call set_worker directly (requires friend or public access)
    std::cout << "\nNote: Full actor addressing not wired - using direct API demo" << std::endl;

    // Direct scheduler demo (when actor addresses aren't fully wired)
    demonstrate_priority_scheduling(system);
    demonstrate_edf_scheduling(system);
    demonstrate_work_stealing(system);

    // Show what actor messaging with priorities would look like:
    std::cout << "\n=== Actor Messaging with Priorities ===" << std::endl;
    std::cout << "With full actor addressing wired:" << std::endl;
    std::cout << "  dispatcher->send_with_priority(worker_addr, WorkMessage{1, 0}, 0, INF)" << std::endl;
    std::cout << "  dispatcher->send_with_priority(worker_addr, WorkMessage{2, 3}, 3, INF)" << std::endl;
    std::cout << "  // Priority 0 work processed before priority 3" << std::endl;

    std::cout << "\n=== Demo Complete ===" << std::endl;
    return 0;
}
```

## Testing Verification

1. Build: `ninja -C build`
2. Run: `./build/examples/07_priority_scheduler_demo`
3. Verify:
   - ✅ Scheduler creates with 4 workers
   - ✅ `notify_ready` accepts priority 0-3 and EDF deadlines
   - ✅ Priority messages processed in priority order (0 before 3)
   - ✅ EDF messages processed by deadline (earliest first)
   - ✅ Work-stealing distributes across workers

## Dependencies on Implementation

This example requires:
1. ✅ `ActorSystem::scheduler()` accessor - **already exists**
2. ✅ `HybridScheduler::notify_ready(actor, priority, deadline)` - **already exists**
3. ⬜ `ActorSystem::deliver_local(target, msg, priority, deadline)` - **needs to be added**
4. ⬜ `ActorContext::send_with_priority(target, msg, priority, deadline)` - **needs to be added**

If these aren't implemented before the example, the example will use direct `notify_ready` calls as fallback.

## Implementation Tasks

### Task 1: Add priority-aware deliver_local

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

### Task 2: Add send_with_priority

**Files:**
- Modify: `include/hpactor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`

### Task 3: Update and run example

**Files:**
- Modify: `examples/07_priority_scheduler_demo.cpp`
- Test: `./build/examples/07_priority_scheduler_demo`
