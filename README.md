# HPActor

A high-performance distributed Actor framework with million-level concurrency support. Combines work-stealing schedulers, EDF (Earliest Deadline First) real-time scheduling, and Multi-priority queues for mathematically deterministic response times.

## Features

- **Actor Type Hierarchy**: Event-based and blocking actors with strong typing support
- **Coroutine Scheduling**: C++20 stackless coroutines with `HybridScheduler` (work-stealing + EDF)
- **Edge-Trigger Mailbox**: Lock-free MPSC queue with `mailbox_was_empty_` CAS wakeup — no lost wakeups
- **Dynamic Behavior**: Actors change message handlers at runtime via `become()`
- **Supervision Strategies**: OneForOne and AllForOne fault-tolerance policies
- **Network Layer**: TLS 1.3, connection pooling, service discovery via registrar
- **Header-only Types**: Actor types, behaviors, and messages in headers; runtime in source files

## Architecture

### Actor Type Hierarchy

```
AbstractActor (interface base)
└── LocalActor (has ActorContext access)
        ├── EventBasedActor (cooperative, behavior-based, coroutine-powered)
        │       ├── StatefulActor<T> (explicit state)
        │       └── TypedEventBasedActor<Signatures...> (statically typed)
        └── BlockingActor (thread-based, blocking receive)
                └── ScopedActor (for main/non-actor contexts)
```

### Scheduling Subsystem

| Component | Purpose |
|-----------|---------|
| `HybridScheduler` | Work-stealing scheduler with A2WS adaptive victim selection |
| `ChaselevDeque<T>` | Lock-free work-stealing deque (LIFO owner, FIFO thief) |
| `MultiPriorityWorkQueue` | Per-priority ChaseLev arrays (0=highest) |
| `EDFQueue` | Earliest Deadline First min-heap for real-time work |
| `TimingWheel` | Hierarchical O(1) timer wheel with cascading |
| `MPSCMailbox<T>` | Vyukov lock-free MPSC queue |
| `MPSCActorMailbox<T>` | Edge-trigger wrapper with CAS wakeup |
| `CoroutineTask` | C++20 coroutine handle wrapper for actor coroutines |

### Network Layer

| Component | Purpose |
|-----------|---------|
| `EventLoop` | kqueue (macOS) / epoll (Linux) event loop |
| `TcpTransport` | TCP transport with TLS 1.3 support |
| `TlsConnection` | AES-256-CBC encryption, RSA key exchange |
| `ConnectionPool` | Dynamic pooling with exponential backoff |
| `Registrar` | UDP discovery + TCP registration with heartbeat |
| `HostResolver` | DNS resolution with caching |

### Key Components

| Component | Location | Purpose |
|-----------|----------|---------|
| `ActorSystem` | `core/actor_system.hpp` | Actor environment, spawn, registry, clock |
| `ActorContext` | `actor/actor_context.hpp` | Actor execution context, message sending |
| `Behavior` | `actor/behavior.hpp` | Message handler with `become()` support |
| `TypedBehavior` | `actor/typed_behavior.hpp` | Statically typed handlers |
| `ActorState` | `actor/actor_state.hpp` | Atomic state machine (Idle/Ready/Running/IOWaiting/Terminated) |
| `Supervision` | `supervision/*.hpp` | Fault-tolerance (OneForOne, AllForOne) |

### Message Flow

Actors communicate via `MessageVariant` (std::variant of all message types):

```cpp
context()->send(addr, msg);   // send message
context()->reply(msg);        // reply to sender
become(Behavior);             // change behavior dynamically
co_await mailbox_awaiter;     // suspend until message arrives
```

### Supervision

- `OneForOneSupervisor` — only the failed child restarts
- `AllForOneSupervisor` — all children restart when one fails
- `SupervisorActor` — supervises children via strategy pattern
- `SelfSupervisingActor` — manages own children with policy

## Build

```bash
# Configure and build
cmake -S . -B build -GNinja
ninja -C build

# Run tests (44 tests)
ctest --output-on-failure

# Run a single test
./build/tests/test_<name>
```

### Build Options

| Option | Description |
|--------|-------------|
| `-DENABLE_TSAN=ON` | Enable ThreadSanitizer |
| `-DENABLE_ASAN=ON` | Enable AddressSanitizer |
| `-DENABLE_EXAMPLES=OFF` | Disable examples (default ON) |

## Design Constraints

- C++20 only (no exceptions, no RTTI)
- LLVM coding standards
- No external dependencies (except OpenSSL for TLS)

## Project Structure

```
include/hpactor/
├── actor/        — Actor base classes, behaviors, typed actors
├── core/         — ActorSystem, ActorContext, registry, mailbox interface
├── mailbox/      — MPSCMailbox, MPSCActorMailbox (lock-free queues)
├── net/          — EventLoop, TLS, connection pool, registrar
├── ref/          — Actor references (address, ref, proxy)
├── sched/        — HybridScheduler, work queues, timing wheel, coroutines
├── spawn/        — AsyncActor for non-blocking spawn
├── supervision/  — OneForOne, AllForOne supervisors
└── types/        — Type system, serialization

src/
├── actor/        — ActorSystem, EventBasedActor, spawn receiver
├── net/          — EventLoop, TcpTransport, TLS, connection pool
├── ref/          — ActorRef, ActorProxy implementations
└── sched/        — HybridScheduler, timing wheel, EDF queue

tests/            — 44 unit tests (actor, core, mailbox, net, sched, spawn)
examples/         — 6 API usage examples
```

## Status

### Complete
- Actor core: spawn, send, receive, behaviors, typed actors
- Supervision: OneForOne, AllForOne, SupervisorActor
- Scheduling: HybridScheduler with work-stealing + EDF + timing wheel
- Coroutine support: CoroutineTask, ActorCoroutine, MailboxAwaiter, TimerAwaiter, YieldAwaiter
- Mailbox: MPSCMailbox (Vyukov), MPSCActorMailbox (edge-trigger)
- Network: TLS 1.3, connection pooling, registrar-based service discovery
- Remote spawn: AsyncActor with spawn_remote()
