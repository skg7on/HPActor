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

**Actor Design:** ✅ Designed
- Spec: `docs/superpowers/specs/2026-04-11-actor-design.md`
- Ready for Phase A implementation

## Key Decisions

- Event-based actors (caf-style) with cooperative scheduling
- Explicit lifecycle with optional hibernation
- Both statically and dynamically typed actors
- Hierarchical supervision (OneForOne, AllForOne)
- Swap-in mailbox interface (earn lock-free through testing)

## Next Step

Phase A: Core Actor implementation
- Fundamental types (ActorId, NodeId, etc.)
- abstract_actor, local_actor, event_based_actor
- ActorSystem, ActorContext
- Behavior, message_handler

## Build Commands

```bash
mkdir -p build && cd build && cmake .. && cmake --build .
ctest --output-on-failure
```
