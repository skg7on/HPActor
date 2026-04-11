# HPActor Project Memory

This project has a persistent memory system in `.claude/projects/-Users-skg7on-Workspace-Projects-HPActor/memory/`.

## Quick Reference

| Topic | File | Description |
|-------|------|-------------|
| Architectural Decisions | `architectural_decisions.md` | Actor model, type system, supervision strategy |
| Implemented Features | `implemented_features.md` | Phase 1 mailbox - what's built |
| Project Status | `project_status.md` | Current phase, next steps, build commands |

## Current State

**Phase 1 (Mailbox):** ✅ Complete
- `Message<T>`, `IMailbox<T>`, `MutexMailbox<T>`, factory
- 1M message stress test passes
- ThreadSanitizer clean

**Actor Core (Phase A-F):** ✅ Complete
- Fundamental types: ActorId, error, Clock, AlarmHandle, TraceContext, MessageId, result<T>
- Actor base classes: abstract_actor, local_actor, event_based_actor
- ActorContext, ActorSystem, actor_registry
- Blocking actors: blocking_actor, scoped_actor
- Stateful actor: stateful_actor<T>
- Typed actors: typed_event_based_actor, typed_behavior
- ActorMailbox integration
- 22 tests passing

**Phase C (Supervision):** ✅ Complete
- SupervisionDirective, ChildFailure, SupervisionPolicy
- Supervisor interface
- OneForOneSupervisor, AllForOneSupervisor
- supervisor_actor, self_supervising_actor

**Actor Design:** ✅ Implemented
- Spec: `docs/superpowers/specs/2026-04-11-actor-design.md`

## Key Decisions

- Event-based actors (caf-style) with cooperative scheduling
- Explicit lifecycle with optional hibernation
- Both statically and dynamically typed actors
- Hierarchical supervision (OneForOne, AllForOne)
- Swap-in mailbox interface (earn lock-free through testing)

## Next Steps

- Phase E: Lifecycle & Hibernation (ActorLifecycle, ActorHost, IHibernationManager)
- Phase F continued: ActorProxy for remote actors

## Key Decisions (Confirmed)

- Event-based actors (caf-style) with cooperative scheduling ✅
- Explicit lifecycle with optional hibernation
- Both statically and dynamically typed actors ✅
- Hierarchical supervision (OneForOne, AllForOne) ✅
- Swap-in mailbox interface (earn lock-free through testing) ✅

## Build Commands

```bash
# From project root or worktree
cmake -S . -B build -GNinja && ninja -C build
ctest --output-on-failure
# Or run tests directly
./build/tests/test_*
