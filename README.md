# HPActor

A High performance distributed Actor framework and runtime. Support million-level concurrency in restricted memory, manage and scheduling millions of Actor states. It uses a decentralized network "work thread", This scheduler uniquely integrates Work-Stealing for throughput, EDF for real-time response, and Multi-priority queues to ensure mathematical determinism in response times.

## Features

- **Actor Type Hierarchy**: Event-based and blocking actors with strong typing support
- **Dynamic Behavior**: Actors can change their message handlers at runtime via `become()`
- **Supervision Strategies**: OneForOne and AllForOne fault-tolerance policies
- **Header-only Types**: Actor types, behaviors, and messages in headers; runtime in source files

## Architecture

### Actor Type Hierarchy

```
AbstractActor (interface base)
└── LocalActor (has ActorContext access)
        ├── EventBasedActor (cooperative, behavior-based)
        │       ├── StatefulActor<T> (explicit state)
        │       └── TypedEventBasedActor<Signatures...> (statically typed)
        └── BlockingActor (thread-based, blocking receive)
                └── ScopedActor (for main/non-actor contexts)
```

### Key Components

| Component | Location | Purpose |
|-----------|----------|---------|
| `ActorSystem` | `actor_system.hpp` | Actor environment, spawn, registry, clock |
| `ActorContext` | `actor_context.hpp` | Actor execution context, message sending |
| `Behavior` | `behavior.hpp` | Message handler with `become()` support |
| `TypedBehavior` | `typed_behavior.hpp` | Statically typed handlers |
| `Supervision` | `supervision/*.hpp` | Fault-tolerance (OneForOne, AllForOne) |

### Message Flow

Actors communicate via `MessageVariant` (std::variant of all message types). From within an actor:

```cpp
context()->send(addr, msg);   // send message
context()->reply(msg);         // reply to sender
become(Behavior);              // change behavior dynamically
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

# Run tests
ctest --output-on-failure

# Run a single test
./build/tests/test_<name>
```

### Build Options

- `-DENABLE_TSAN=ON` — enable ThreadSanitizer
- `-DENABLE_ASAN=ON` — enable AddressSanitizer
- `-DENABLE_EXAMPLES=OFF` — disable examples

## Design Constraints

- C++20 only
- No exceptions (`-fno-exceptions`)
- No RTTI (`-fno-rtti`)
- LLVM coding standards

## Project Structure

```
include/hpactor/   — public headers
src/               — implementation files (linked into hpactor_lib)
tests/             — unit tests
examples/          — API usage examples
docs/              — architecture docs and tutorials
```

## Status

Actor runtime infrastructure (spawn, send, scheduler) is under development.
