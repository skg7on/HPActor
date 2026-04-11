---
name: cpp-actor-framework
description: Production C++20 Actor Framework for near real-time embedded systems
type: design spec
created: 2026-04-10
owner: SKG7ON
status: draft
---

# C++ Actor Framework Design Specification

## 1. Executive Summary

**Project:** Production-grade C++20 Actor Framework for near real-time embedded systems with high-throughput messaging.

**Target Profile:**
- Environment: Embedded/Near real-time (bounded jitter in microseconds)
- Throughput: Millions of messages/second
- C++ Standard: C++20
- Actor Model: Pooled actors with arena allocation
- Memory Model: Arena allocator with ownership tracking

**Design Philosophy:** Earn lock-free complexity through testing. Use swap-in interfaces for performance upgrade paths.

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      Application                                │
├─────────────────────────────────────────────────────────────────┤
│  Actor Pool (pre-allocated N actors)                            │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐                          │
│  │ Actor 1 │  │ Actor 2 │  │ Actor N │                          │
│  │   ▼     │  │   ▼     │  │   ▼     │                          │
│  │ Mailbox │  │ Mailbox │  │ Mailbox │  ← MPSC Queues           │
│  └──────── ┘  └── ──────┘  └──────── ┘                          │
├─────────────────────────────────────────────────────────────────┤
│                    Message Bus                                  │
│  ┌──────────────────────────────────────┐                       │
│  │ std::variant<MessageTypeA, TypeB...> │ ← Type-safe, no RTTI  │
│  └──────────────────────────────────────┘                       │
├─────────────────────────────────────────────────────────────────┤
│            Work-Stealing Scheduler                              │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐                    │
│  │Thread 1│ │Thread 2│ │Thread 3│ │Thread N│ ← N = CPU cores    │
│  │  LQ    │ │  LQ    │ │  LQ    │ │  LQ    │ ← Local queues     │
│  └────────┘ └────────┘ └────────┘ └────────┘                    │
│         │        │        │        │                            │
│         └────────┴────────┴────────┘ ← Work stealing            │
├─────────────────────────────────────────────────────────────────┤
│                    Arena Allocator                              │
│  ┌──────────────���──────────────────────┐                      │
│  │ Pre-allocated message buffers         │ ← Zero runtime malloc│
│  │ Per-actor message storage             │                      │
│  │ Ownership tracking & lifetime         │                      │
│  └─────────────────────────────────────  ┘                      │
└─────────────────────────────────────────────────────────────────┘
```

### 2.1 Component Design Decisions

| Component | Design Decision | Rationale |
|-----------|-----------------|----------|
| Mailbox | MPSC queue with swap-in interface | Multiple producers, single consumer; swap-in allows lock-free upgrade |
| Message Bus | std::variant, no RTTI | C++ type-safe, zero overhead, no dynamic_cast |
| Scheduler | Work-stealing thread pool | Minimize lock contention; dynamic scaling for load |
| Allocator | Arena-based pre-allocation | Bounded latency, no runtime malloc |
| Actors | Pooled, not created/destroyed | Deterministic lifetime, reuse memory |

---

## 3. Implementation Phases

### Phase 1: Mailbox (MPSC Queue)

**Goal:** Multiple-Producer, Single-Consumer queue for actor mailboxes.

**Approach:**
- Start with `std::mutex` or lightweight `std::atomic_flag` spinlock
- **NOT** lock-free on day one
- Design swap-in interface for lock-free replacement later

**Interface:**
```cpp
template<typename T>
class IMailbox {
public:
    virtual void push(Message<T>&& msg) = 0;
    virtual bool pop(Message<T>& out) = 0;
    virtual bool try_pop(Message<T>& out) = 0;
    virtual ~IMailbox() = default;
};
```

**Testing:**
- 100 threads hammering one mailbox with 1 million messages
- Must pass 100% before lock-free refactor

### Phase 2: Message Bus (Type-Safety without RTTI)

**Goal:** Type-safe message passing using modern C++17/C++20 features.

**Approach:**
- Use `std::variant` to define strict message type set
- Use `std::visit` for pattern matching
- Strict move semantics (`std::move`) for zero-copy

**Example:**
```cpp
using MessageVariant = std::variant<PingMsg, PongMsg, QuitMsg, CustomMsg>;

class Actor {
    void handle(MessageVariant& msg) {
        std::visit([this](auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, PingMsg>) {
                // handle PingMsg
            } else if constexpr (std::is_same_v<T, PongMsg>) {
                // handle PongMsg
            }
            // ...
        }, msg);
    }
};
```

### Phase 3: Scheduler (Work-Stealing Thread Pool)

**Goal:** Multiplex actors onto worker threads with minimal lock contention.

**Design:**
- Each OS thread has its own local queue of actors
- If thread finishes local queue, "steals" from another thread's queue
- Dynamic scaling: core pool + optional burst threads

**Considerations:**
- Fixed core pool for deterministic latency
- Adaptive scaling based on work queue depth
- Thread affinity for embedded consistency

### Phase 4: C++20 Coroutines

**Goal:** Non-blocking actors that can suspend/resume without blocking OS threads.

**Approach:**
- Coroutine-native design integration
- Suspend on I/O wait, resume on message arrival
- Stackful coroutines for bounded stack allocation

**Interface:**
```cpp
task<void> MyActor::run() {
    while (true) {
        auto msg = co_await receive<MessageType>();
        // process message, yielding thread if waiting
    }
}
```

---

## 4. Design Decisions Summary

### Selected Approach: Hybrid with Swap-Ins

**Why:** Production embedded requires safety + upgrade path.

- Phases built in order: Mailbox → Message Bus → Scheduler → Coroutines
- Lock-free optimization earned through stress testing
- Interfaces designed generic for swap-in replacements

### Key Constraints Applied

| Constraint | Selection | Impact |
|------------|-----------|--------|
| Environment | Embedded/Near real-time | Bounded latency required |
| Memory | Arena allocator | Pre-allocated, zero runtime malloc |
| Actor lifecycle | Pooled | Reuse, no create/destroy |
| Throughput | High (M+ msg/sec) | Lock-free upgrade needed |
| C++ Standard | C++20 | Coroutines from phase 1 |

### Testing Requirements

- All code compiled with ThreadSanitizer: `clang++ -fsanitize=thread`
- TSan must pass 100% before any commit
- Stress test: 100 threads, 1M messages, mailbox only

---

## 5. Interface Contracts

### IMailbox<T>

```cpp
template<typename T>
class IMailbox {
public:
    virtual void push(T&& msg) = 0;
    virtual bool pop(T& out) = 0;
    virtual bool try_pop(T& out) = 0;
    virtual size_t size() const = 0;
    virtual ~IMailbox() = default;
};
```

### IMessageBus

```cpp
template<typename... MessageTypes>
class IMessageBus {
public:
    template<typename T>
    void send(ActorId target, T&& msg);
    
    template<typename T>
    task<T> receive();
    
    virtual ~IMessageBus() = default;
};
```

### IScheduler

```cpp
class IScheduler {
public:
    virtual void schedule(Actor& actor) = 0;
    virtual void schedule(Actor& actor, std::chrono::milliseconds delay) = 0;
    virtual void deschedule(Actor& actor) = 0;
    virtual void set_thread_count(size_t count) = 0;
    virtual ~IScheduler() = default;
};
```

### IArenaAllocator

```cpp
class IArenaAllocator {
public:
    virtual void* allocate(size_t size, size_t align) = 0;
    virtual void deallocate(void* ptr) = 0;
    virtual void reset() = 0;
    virtual ~IArenaAllocator() = default;
};
```

---

## 6. Error Handling

- **No exceptions in hot path:** Use error codes for mailbox/queue operations
- **Actor-level handling:** Each actor catches and logs its own exceptions
- **Scheduler recovery:** Failed actors are logged and descheduled, not crashed

---

## 7. Implementation Order

1. **ArenaAllocator:** Foundation, no dependencies
2. **IMailbox interface + Mutex implementation:** First mailbox
3. **Message definitions + std::variant:** Message types
4. **Actor base class:** Using mailbox + message bus
5. **IScheduler interface + basic implementation:** Simple round-robin first
6. **Work-stealing upgrade:** After basic pass tests
7. **Coroutine integration:** Replace synchronous receive with co_await

---

## 8. Review Notes

- Phase 1 (Mailbox) must be rock-solid before Phase 3 (Scheduler)
- TSan is mandatory — reject any code that triggers warnings
- Coroutines are C++20 native but require careful lifetime management in pooled actors
- Arena allocator reset must happen at known safe points (no in-flight messages)

---

## 9. Pending Decisions

- [ ] Exact buffer sizes for arena (message count / size limits)
- [ ] Thread count auto-scaling thresholds
- [ ] Whether to use stackful vs stackless coroutines
- [ ] Lock-free algorithm selection (if pursuing Phase 1 upgrade)

---

**Status:** Design draft — awaiting approval before writing implementation plan.
