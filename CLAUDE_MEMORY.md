# HPActor Project Memory

This project has a persistent memory system in `.claude/projects/-Users-skg7on-Workspace-Projects-HPActor/memory/`.

## Quick Reference

| Topic | File | Description |
|-------|------|-------------|
| Architectural Decisions | `architectural_decisions.md` | Actor model, type system, supervision strategy |
| Implemented Features | `implemented_features.md` | Complete implementation - what's built |
| Project Status | `project_status.md` | Current phase, next steps, build commands |

## Current State

**Actor Core Framework:** ✅ Complete (Phase A-G)
- Fundamental types: ActorId, error, Clock, AlarmHandle, TraceContext, MessageId, result<T>
- Actor base classes: abstract_actor, local_actor, event_based_actor
- ActorContext, ActorSystem, actor_registry
- Blocking actors: blocking_actor, scoped_actor
- Stateful actor: stateful_actor<T>
- Typed actors: typed_event_based_actor, typed_behavior
- ActorMailbox integration
- Supervision: OneForOne, AllForOne, supervisor_actor, self_supervising_actor

**Network Layer:** ✅ Complete (Phase 4-5)
- TlsContext — certificate loading, RSA signing, pre-master secret decryption
- TlsConnection — TLS state machine, AES-256-CBC encryption
- ConnectionPool — dynamic pooling per node, round-robin, exponential backoff
- TcpTransport — updated to use ConnectionPool + TLS
- EventLoop timer support — EVFILT_TIMER/timerfd for reconnect backoff
- EventLoop backend fallback — EpollBackend (Linux), KqueueBackend (macOS) with explicit run()/stop()
- UdpRegistrar — UDP-based node discovery with server/client dual mode
- HostResolver — DNS resolution with caching
- NodeRegistry — registry of known nodes with static routes
- RegistrarServer — TCP server for node registration, heartbeat, broadcasts
- RegistrarClient — TCP client with failover, local IP detection, AcceptorInfo

**Phase 6: Remote Actor Spawn** ✅ Complete (34 tests passing)
- AsyncActor handle for non-blocking spawn with get(), ready(), cancel()
- SpawnRequest/SpawnResponse message types
- ActorTypeRegistry for registering spawnable actor types
- SpawnReceiver system actor for handling spawn requests
- ActorSystem::spawn_remote() and spawn_remote_async()
- Well-known system ActorIds (SpawnReceiverId, SystemActorType)

**Examples:** ✅ Complete
- `examples/01_echo_actor.cpp` — EventBasedActor, make_behavior(), become()
- `examples/02_counter_stateful.cpp` — StatefulActor<T>, state management
- `examples/03_typed_calculator.cpp` — typed_actor<>, TypedBehavior
- `examples/04_supervision_tree.cpp` — OneForOne/AllForOne, SupervisorActor
- `examples/05_ping_pong.cpp` — Actor communication, ScopedActor, linking
- Built via `ENABLE_EXAMPLES` CMake option (default ON)

**Scheduling Subsystem:** ✅ Complete (Phase 0-7, 2026-04-15)
- `ChaselevDeque<T>` — Lock-free work-stealing deque (LIFO owner pop, FIFO thief steal)
- `MultiPriorityWorkQueue` — Array of ChaseLev deques, one per priority level (0=highest)
- `EDFQueue` — Earliest Deadline First min-heap, O(log n) push/pop, FIFO tiebreaker
- `A2WS` — Adaptive Two-Level Work Stealing with pool-based locality
- `TimingWheel` — Hierarchical timer wheel (O(1) insert/cancel), 4 levels, cascading
- `CoroutineFramePool` — Lock-free stack pool for coroutine frames, O(1) acquire/release
- `HybridScheduler` — Work-stealing scheduler with IScheduler interface, wired to ActorSystem
- `WorkerThread` — Per-thread worker with local queue and frame pool integration
- `IScheduler` interface: `notify_ready()`, `notify_idle()`, `schedule_after()`, `schedule_every()`, `cancel_timer()`, `worker_count()`
- Timer advancement thread with proper cancellation for recurring timers
- `ActorState` — Atomic state machine (Idle/Ready/Running/IOWaiting/Terminated) with CAS transitions
- `CoroutineTask` / `CoroutinePromise` — C++20 coroutine handle wrapper for actor coroutines
- `MailboxAwaiter`, `TimerAwaiter`, `BlockingMailboxAwaiter` — awaiters for co_await patterns
- `MPSCMailbox<T>` — Vyukov lock-free MPSC queue (wait-free enqueue, lock-free dequeue)
- `execute_actor()` dispatch layer for coroutine resumption with state transitions

**Tests:** ✅ 41 tests passing
- Scheduling: test_chaselev_deque, test_multi_priority_work_queue, test_hybrid_scheduler, test_edf_queue, test_a2ws

**Documentation:** ✅ Complete
- Tutorial: `docs/superpowers/tutorials/actor-framework-tutorial.md`
- Spec: `docs/superpowers/specs/2026-04-11-actor-design.md`
- Plan: `docs/superpowers/plans/2026-04-11-actor-core-impl.md`
- Spec: `docs/superpowers/specs/2026-04-14-registrar-refactor-design.md` (registrar bug fixes)
- Plan: `docs/superpowers/plans/2026-04-14-registrar-refactor-impl.md`
- Spec: `docs/superpowers/specs/2026-04-14-event-loop-backend-fallback-design.md`
- Plan: `docs/superpowers/plans/2026-04-14-event-loop-backend-fallback-impl.md`
- Spec: `docs/superpowers/specs/2026-04-15-coroutine-scheduling-design.md`
- Plan: `docs/superpowers/plans/2026-04-15-coroutine-scheduling-impl.md`

## Key Decisions

- Event-based actors (caf-style) with cooperative scheduling
- Explicit lifecycle with optional hibernation
- Both statically and dynamically typed actors
- Hierarchical supervision (OneForOne, AllForOne)
- Swap-in mailbox interface (earned lock-free through testing)
- Header-only library, C++20, no external dependencies (except OpenSSL for TLS)
- No exceptions (-fno-exceptions), no RTTI (-fno-rtti)
- constexpr ActorId constructor for constant initialization

## Current Progress

**Phase 0-7 Complete** (41 tests passing)
- Phase 0: Local Message Delivery — actor spawn and local message routing
- Phase 1: ActorRef and Unified References — ActorRef as variant<Actor, ActorProxy>
- Phase 2: TCP Transport Implementation — kqueue/epoll event loop, TcpTransport, Connection
- Phase 3: Message Serialization — TypeTag enum, DefaultSerializer, Frame encode/decode
- Phase 4: Connection Pool and Handshake — TlsContext, TlsConnection, ConnectionPool, TLS handshake, AES-256-CBC encryption
- Phase 5: Service Discovery — UdpRegistrar, HostResolver, NodeRegistry, static routes, DNS resolution, RegistrarServer/RegistrarClient with TCP registration, heartbeat, failover
- Phase 6: Remote Actor Spawn — AsyncActor, ActorTypeRegistry, SpawnReceiver, spawn_remote()
- Scheduling Subsystem: ChaseLev deque, MultiPriorityWorkQueue, EDFQueue, A2WS, TimingWheel, CoroutineFramePool, HybridScheduler, WorkerThread, ActorState, CoroutineTask/CoroutinePromise, awaiters, MPSCMailbox

**Next Steps (Phase 7 completion)**
- Full serialization integration (SpawnRequest/SpawnResponse as MessageVariant)
- Transport response routing (Transport calling AsyncActor::set_response)
- Registrar node lookup (translating node names to NodeId)
- Argument deserialization (passing constructor args through spawn)
- Integration test (two-process remote spawn test)
- MPSCMailbox integration into EventBasedActor (mailbox_has_messages() stub)
- WorkerThread integration into HybridScheduler (currently HybridScheduler uses inline WorkerState, WorkerThread is separate)

**Source Reorganization**
- `include/hpactor/` — header-only library, organized by architectural group:
  - `actor/` — Actor base classes, behaviors, typed actors, spawn
  - `ref/` — Actor references (address, ref, proxy)
  - `net/` — Networking (event loop, TLS, connection pool, transports)
  - `supervision/` — Supervision strategies
  - `core/` — Core runtime (actor_system, scheduler, mailbox, registry)
  - `sched/` — Scheduling subsystem (work_queue, edf_queue, a2ws, timing_wheel, coroutine_frame_pool)
  - `types/` — Type system (types, types_fwd, serialization)
- `src/actor/` — actor_system.cpp, abstract_actor.cpp, actor_context.cpp, scheduler.cpp, event_based_actor.cpp, local_actor.cpp, spawn_receiver.cpp
- `src/net/` — event_loop.cpp, acceptor.cpp, connection.cpp, tcp_transport.cpp, frame.cpp, tls_context.cpp, tls_connection.cpp, connection_pool.cpp, registrar.cpp
- `src/ref/` — actor_proxy.cpp, actor_ref.cpp
- `src/sched/` — scheduler.cpp, worker_thread.cpp, edf_queue.cpp, a2ws.cpp, timing_wheel.cpp, coroutine_frame_pool.cpp
- `src/spawn.cpp` — AsyncActor implementation
- `src/actor_type_registry.cpp` — ActorTypeRegistry implementation
- `src/core/serialization.cpp` — DefaultSerializer implementation
- Tests: `tests/{actor,core,mailbox,net,ref,supervision,spawn,sched}/`

## Build Commands

```bash
# From project root
cmake -S . -B build -GNinja && ninja -C build
ctest --output-on-failure

# With sanitizers
cmake -DENABLE_TSAN=ON ..  # ThreadSanitizer
cmake -DENABLE_ASAN=ON ..  # AddressSanitizer

# Enable/disable examples (default ON)
cmake -DENABLE_EXAMPLES=OFF ..
```
