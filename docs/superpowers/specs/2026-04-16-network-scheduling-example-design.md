# Network Scheduling Integration Example Design

**Date:** 2026-04-16
**Status:** Draft
**Goal:** Create a complete end-to-end example demonstrating actors sending different message types with priorities/deadlines over socket communication, with the scheduling subsystem properly integrated.

## Overview

The existing examples (01-05) are **API demonstrations** showing intended usage patterns but not actually running. This example will be **functional**, demonstrating:

1. **Multi-priority message scheduling** — actors send messages at priorities 0-3
2. **Deadline-aware scheduling** — EDF queue with real deadlines
3. **Network communication** — TCP socket messaging between actors
4. **Work-stealing** — Multiple workers processing from priority queues

## Architecture

### Example: Networked Priority Worker

```
                    +-----------------+
                    |  ActorSystem    |
                    |  (4 workers,    |
                    |   priority 0-3) |
                    +-----------------+
                           |
         +-----------------+-----------------+
         |                                   |
+--------v--------+              +-----------v---------+
| TcpTransport    |              | Scheduler            |
| (TCP server)    |              | - Priority queues    |
+--------+--------+              | - EDF deadlines      |
         |                       | - A2WS work-stealing |
         |                       +---------------------+
         |
+--------v--------+
| Remote Actor     |
| (PriorityWorker) |
| - Handles msgs   |
| - Reports timing |
+--------+--------+
         |
         v
  Socket Connection
```

### Message Flow

1. `PriorityClient` sends messages to `PriorityWorker` via TCP
2. Each message has a priority (0-3) and optional deadline
3. `HybridScheduler` enqueues based on priority, EDF for deadlines
4. Workers process from queues, stealing when idle
5. Completion messages flow back through EventLoop

## Key Components to Implement

### 1. PriorityWorker Actor

An actor that receives messages at different priorities and processes them:

```cpp
struct WorkMessage {
    int work_id;
    int priority;       // 0-3
    int64_t deadline_ns; // for EDF
    std::string data;
};

struct WorkResult {
    int work_id;
    bool completed_on_time;
    int64_t latency_ns;
};
```

Behavior: Higher priority messages processed first. With EDF, late messages are marked as missed.

### 2. Network Server (within ActorSystem)

The TcpTransport listens on a port and routes incoming frames to actors. This is already implemented — we just need to use it.

### 3. Priority Scheduling Demonstration

Create a client that sends messages at different priorities:

```cpp
// Send high priority work
scheduler_->notify_ready(worker_id, 0, INT64_MAX);  // priority 0 = highest

// Send low priority work
scheduler_->notify_ready(worker_id, 3, INT64_MAX);  // priority 3 = lowest

// Send deadline-sensitive work
scheduler_->notify_ready(worker_id, priority, deadline_ns);  // EDF tracking
```

### 4. Work-Stealing Demonstration

With multiple workers, demonstrate that idle workers steal from busy workers:

- Worker 1: High priority queue full, Worker 2 steals
- Worker 1: EDF items expire, Worker 2 picks them up

## Example File Structure

Create `examples/06_networked_priority_worker.cpp`:

```cpp
// =============================================================================
// HPActor Example 06: Networked Priority Worker
// =============================================================================
//
// This example demonstrates a complete end-to-end system with:
//   - Actor spawning and message passing
//   - Multi-priority scheduling (4 priority levels)
//   - Deadline-aware scheduling (EDF queue)
//   - TCP network communication
//   - Work-stealing across multiple scheduler threads
//
// Run two instances:
//   Node A (server): ./examples/06_networked_priority_worker --server --port 8080
//   Node B (client): ./examples/06_networked_priority_worker --client --port 8080
//
// =============================================================================

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <random>
```

## What's Implemented vs What's Stub

### Implemented:
- `ActorSystem` with scheduler threads
- `HybridScheduler` with priority queues and EDF
- `EventLoop` with epoll/kqueue backend
- `TcpTransport` for network communication
- `deliver_local()` with `notify_ready()` integration
- `completion_msg` delivery to actor mailboxes

### Still Needed for Full Example:
- **Actor spawn API** — `system.spawn<ActorType>()` returns `Actor`
- **Actor mailbox association** — mapping `ActorId` to `ActorMailbox`
- **Message routing from transport** — `TcpTransport` delivering to actor mailboxes
- **Response path** — actor sending response back through transport

## Realistic Scope for Example

Given the current implementation state, the example should demonstrate:

1. **Scheduler with priorities** — show that messages are processed in priority order
2. **EDF deadline tracking** — show that deadlines are tracked even if not strictly enforced
3. **Work-stealing** — show multiple workers distributing work

The network aspect can be **simulated** (local socket pair) or **partial** (transport exists but full routing not wired).

## Alternative: Local Socket Pair Example

For a fully functional example without depending on full transport routing:

```cpp
// Create socket pair (模拟网络)
int sv[2];
socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

// Actor A sends to Actor B through the socket
// HybridScheduler processes with priorities
// EDF tracks deadlines
```

## Files to Create

| File | Purpose |
|------|---------|
| `examples/06_networked_priority_worker.cpp` | Main example with scheduling demonstration |
| `examples/07_socket_priority_demo.cpp` | Local socket pair demo (simpler, fully functional) |

## Implementation Approach

Given the current state, create `examples/07_socket_priority_demo.cpp` that:

1. Creates `ActorSystem` with 4 scheduler threads
2. Spawns a `PriorityWorker` actor
3. Sends messages at different priorities
4. Verifies they are processed in priority order
5. Demonstrates EDF deadline tracking
6. Shows work-stealing across threads

The network aspect can be added once transport routing is fully wired.

## Testing Verification

After implementation:
1. Build: `ninja -C build`
2. Run example: `./build/examples/07_socket_priority_demo`
3. Verify:
   - Priority 0 messages processed before priority 3
   - EDF queue receives deadline messages
   - Multiple workers show work-stealing in logs
