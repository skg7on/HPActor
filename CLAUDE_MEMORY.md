# HPActor Project Memory

This project has a persistent memory system in `.claude/projects/-Users-skg7on-Workspace-Projects-HPActor/memory/`.

## Quick Reference

| Topic | File | Description |
|-------|------|-------------|
| Architectural Decisions | `architectural_decisions.md` | Actor model, type system, supervision strategy |
| Implemented Features | `implemented_features.md` | Complete implementation - what's built |
| Project Status | `project_status.md` | Current phase, next steps, build commands |

## Current State

**Actor Core Framework:** ✅ Complete (30 tests passing)
- Fundamental types: ActorId, error, Clock, AlarmHandle, TraceContext, MessageId, result<T>
- Actor base classes: abstract_actor, local_actor, event_based_actor
- ActorContext, ActorSystem, actor_registry
- Blocking actors: blocking_actor, scoped_actor
- Stateful actor: stateful_actor<T>
- Typed actors: typed_event_based_actor, typed_behavior
- ActorMailbox integration
- Supervision: OneForOne, AllForOne, supervisor_actor, self_supervising_actor

**Network Layer:** ✅ Complete (Phase 4)
- TlsContext — certificate loading, RSA signing, pre-master secret decryption
- TlsConnection — TLS state machine, AES-256-CBC encryption
- ConnectionPool — dynamic pooling per node, round-robin, exponential backoff
- TcpTransport — updated to use ConnectionPool + TLS
- EventLoop timer support — EVFILT_TIMER/timerfd for reconnect backoff

**Examples:** ✅ Complete
- `examples/01_echo_actor.cpp` — EventBasedActor, make_behavior(), become()
- `examples/02_counter_stateful.cpp` — StatefulActor<T>, state management
- `examples/03_typed_calculator.cpp` — typed_actor<>, TypedBehavior
- `examples/04_supervision_tree.cpp` — OneForOne/AllForOne, SupervisorActor
- `examples/05_ping_pong.cpp` — Actor communication, ScopedActor, linking
- Built via `ENABLE_EXAMPLES` CMake option (default ON)

**Documentation:** ✅ Complete
- Tutorial: `docs/superpowers/tutorials/actor-framework-tutorial.md`
- Spec: `docs/superpowers/specs/2026-04-11-actor-design.md`
- Plan: `docs/superpowers/plans/2026-04-11-actor-core-impl.md`

## Key Decisions

- Event-based actors (caf-style) with cooperative scheduling
- Explicit lifecycle with optional hibernation
- Both statically and dynamically typed actors
- Hierarchical supervision (OneForOne, AllForOne)
- Swap-in mailbox interface (earned lock-free through testing)
- Header-only library, C++20, no external dependencies

## Current Progress

**Phase 0-4 Complete** (30 tests passing)
- Phase 0: Local Message Delivery — actor spawn and local message routing
- Phase 1: ActorRef and Unified References — ActorRef as variant<Actor, ActorProxy>
- Phase 2: TCP Transport Implementation — kqueue/epoll event loop, TcpTransport, Connection
- Phase 3: Message Serialization — TypeTag enum, DefaultSerializer, Frame encode/decode
- Phase 4: Connection Pool and Handshake — TlsContext, TlsConnection, ConnectionPool, TLS handshake, AES-256-CBC encryption

**Next Steps (Phase 5-6)**
- Phase 5: Service Discovery — UDP registrar, static routes
- Phase 6: Remote Actor Spawn — Spawn actors on remote nodes

**Source Reorganization**
- `src/actor/` — actor_system.cpp, abstract_actor.cpp, actor_context.cpp, scheduler.cpp, event_based_actor.cpp, local_actor.cpp
- `src/net/` — event_loop.cpp, acceptor.cpp, connection.cpp, tcp_transport.cpp, frame.cpp, tls_context.cpp, tls_connection.cpp, connection_pool.cpp
- `src/ref/` — actor_proxy.cpp
- Tests: `tests/{actor,core,mailbox,net,ref,supervision}/`

## Next Steps

- Phase 5: Service Discovery
- Phase 6: Remote Actor Spawn

## Build Commands

```bash
# From project root
cmake -S . -B build -GNinja && ninja -C build
ctest --output-on-failure

# With examples (default)
cmake -DENABLE_EXAMPLES=ON ..
# Without examples
cmake -DENABLE_EXAMPLES=OFF ..
```
