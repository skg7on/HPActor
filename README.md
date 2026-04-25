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
| `RpcChannel` | Async RPC with at-least-once delivery and retry |
| `WireFrame` | Protobuf-serialized network frame (IPv4/IPv6) |

### Protobuf Serialization

| Component | Purpose |
|-----------|---------|
| `common.proto` | Shared endpoint types (ActorEndpoint, ActorAddress) |
| `frame.proto` | WireFrame transport format |
| `messages.proto` | System message types (Down, Exit, Link, Unlink, Spawn) |
| `DefaultSerializer` | Protobuf-based encode/decode for all message types |

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

# Run tests (51 tests)
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

These constraints are not arbitrary — each enables a specific architectural property:

### No exceptions in hot path
Actor message handling is on the critical path. Throwing exceptions would impose try/catch overhead on every message dispatch. Instead, errors use `error` codes (returned via `result<T>`) and are handled through the supervision hierarchy. This keeps message dispatch predictable and allocation-free.

### No RTTI — TypeTag replaces it
Distributed actors cannot rely on C++ RTTI since actor instances cross process boundaries. A `TypeTag` enum (0-99 for system messages, 100+ for user types) identifies message types for serialization dispatch. This is also faster than `dynamic_cast` and works across the network.

### C++20 coroutines for actor suspend/resume
Actors spend most of their time waiting for messages or I/O. C++20 stackless coroutines allow actors to suspend without a full thread stack — thousands of actors can be multiplexed onto a small thread pool. This is the foundation of million-level concurrency.

### Header-only types, linked runtime
Actor types, behaviors, and message definitions are header-only templates — zero overhead, inlined by the compiler. The actor runtime (scheduler, event loop, connection pool) is compiled into a shared library. This separation means actors pay no abstraction cost while the runtime can evolve independently.

### constexpr ActorId initialization
`ActorId` has a `constexpr` constructor enabling constant initialization of well-known actor IDs (e.g., `SpawnReceiverId`). This avoids static initialization order problems and makes test fixtures simpler.

### Lock-free mailbox earned through testing
The mailbox uses a Vyukov MPSC queue with an edge-trigger `CAS` wakeup mechanism. This was designed through iterative testing rather than upfront theory — the "swap-in mailbox interface" means the implementation can be replaced if the lock-free approach proves problematic on new hardware.

### No external dependencies except OpenSSL
TLS is the only external dependency. All other functionality (event loops, schedulers, actors, serialization) is self-contained. This reduces attack surface, simplifies distribution, and eliminates dependency version conflicts.

### LLVM coding standards
The codebase uses LLVM style (`clang-format`) with strict warnings (`-Wall -Wextra -Wpedantic`). This ensures the code is clean, portable, and compatible with the clang toolchain used for development.

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
└── types/        — Type system, protobuf serialization

src/
├── actor/        — ActorSystem, EventBasedActor, spawn receiver
├── core/         — serialization.cpp (protobuf-based)
├── net/          — EventLoop, TcpTransport, TLS, connection pool
├── ref/          — ActorRef, ActorProxy implementations
└── sched/        — HybridScheduler, timing wheel, EDF queue

protos/hpactor/
├── common.proto  — Shared endpoint types
├── frame.proto   — WireFrame transport format
└── messages.proto — System message types

tests/            — 51 unit tests (actor, core, mailbox, net, sched, spawn)
examples/         — 5 API usage examples
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
- Serialization: Protobuf-based for all system messages (WireFrame, Down, Exit, Link, Unlink, Spawn)
