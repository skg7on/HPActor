# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Configure and build
cmake -S . -B build -GNinja
ninja -C build

# Run tests
ctest --output-on-failure

# Run a single test
./build/tests/test_<name>

# Build with sanitizers
cmake -DENABLE_TSAN=ON ..  # ThreadSanitizer
cmake -DENABLE_ASAN=ON ..  # AddressSanitizer

# Enable/disable examples (default ON)
cmake -DENABLE_EXAMPLES=OFF ..
```

## Architecture

HPActor is a C++20 event-based actor framework inspired by CAF (C++ Actor Framework).

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
- `context()->send(addr, msg)` — send message
- `context()->reply(msg)` — reply to sender
- `become(Behavior)` — change behavior dynamically

### Supervision

- `OneForOneSupervisor` — only failed child restarts
- `AllForOneSupervisor` — all children restart when one fails
- `SupervisorActor` — supervises children via strategy pattern
- `SelfSupervisingActor` — manages own children with policy

### Design Constraints

- C++20, no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`)
- Header-only library (types, behaviors) + implementation .cpp files
- LLVM coding standards (see CMakeLists.txt compiler flags)
- Actor runtime infrastructure (spawn, send, scheduler) is not yet implemented

## Important Files

- `include/hpactor/` — public headers
- `src/` — implementation files (linked into hpactor_lib)
- `tests/` — 23 unit tests
- `examples/` — 5 API usage examples
- `docs/superpowers/tutorials/actor-framework-tutorial.md` — usage guide
- `.claude/projects/*/memory/` — persistent project memory
