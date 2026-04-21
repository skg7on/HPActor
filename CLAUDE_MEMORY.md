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

**Phase 7: Async RPC Channel** ✅ Complete (48 tests)
- RpcChannel — async RPC with at-least-once delivery, retry on timeout
- RpcFuture<bytes> — future wrapper with timeout-enforced get()
- Frame flags: RpcRequest, RpcResponse, RpcIdempotent
- ConnectionPool::set_rpc_handler() for RPC response routing
- Transport::set_rpc_handler() interface propagated to TcpTransport
- ActorContext::rpc() and ActorSystem::rpc_channel() for non-actor thread access

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
- `MPSCMailbox<T>` — Vyukov lock-free MPSC queue (wait-free enqueue, lock-free dequeue), includes cyclic queue fix when returning last element
- `execute_actor()` dispatch layer for coroutine resumption with state transitions

**Tests:** ✅ 48 tests passing
- Scheduling: test_chaselev_deque, test_multi_priority_work_queue, test_hybrid_scheduler, test_edf_queue, test_a2ws, test_mailbox_awaiter, test_coroutine_scheduling, test_priority_scheduler

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
- Spec: `docs/superpowers/specs/2026-04-20-rpc-channel-design.md` (async RPC channel)
- Plan: `docs/superpowers/plans/2026-04-20-rpc-channel-impl.md`

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

**Phase 0-7 Complete** (48 tests passing)
- Phase 0: Local Message Delivery — actor spawn and local message routing
- Phase 1: ActorRef and Unified References — ActorRef as variant<Actor, ActorProxy>
- Phase 2: TCP Transport Implementation — kqueue/epoll event loop, TcpTransport, Connection
- Phase 3: Message Serialization — TypeTag enum, DefaultSerializer, Frame encode/decode
- Phase 4: Connection Pool and Handshake — TlsContext, TlsConnection, ConnectionPool, TLS handshake, AES-256-CBC encryption
- Phase 5: Service Discovery — UdpRegistrar, HostResolver, NodeRegistry, static routes, DNS resolution, RegistrarServer/RegistrarClient with TCP registration, heartbeat, failover
- Phase 6: Remote Actor Spawn — AsyncActor, ActorTypeRegistry, SpawnReceiver, spawn_remote()
- Scheduling Subsystem: ChaseLev deque, MultiPriorityWorkQueue, EDFQueue, A2WS, TimingWheel, CoroutineFramePool, HybridScheduler, WorkerThread, ActorState, CoroutineTask/CoroutinePromise, awaiters, MPSCMailbox

**Phase 8: Spawn Serialization Integration** ✅ Complete (2026-04-21)
- SpawnRequest/SpawnResponse integrated with TypeTag (SpawnRequestTag=5, SpawnResponseTag=6)
- SpawnMessageVariant (separate from main MessageVariant to avoid circular includes via spawn.hpp → serialization.hpp → abstract_actor.hpp)
- DefaultSerializer::encode_spawn()/decode_spawn() for spawn type serialization
- ActorSystem::spawn_remote_async() uses DefaultSerializer for request encoding
- ConnectionPool hybrid routing: SpawnResponse → spawn_handler, other RPC → rpc_handler
- SpawnReceiver sends SpawnResponse via transport with Frame context for reply routing
- AsyncActor gains message_id_ field for response correlation
- ActorTypeRegistry::spawn() updated to accept args and args_type parameters
- Remote child tracking added to SelfSupervisingActor (remote_children_, remote_child_addresses_, add_remote_child, etc.)
- ActorContext gains add_remote_child(ActorRef) method
- Integration test (test_spawn_integration) validates frame encoding and message correlation

**Next Steps (Phase 9 - remaining items)**
- Argument deserialization (passing constructor args through spawn)
- Full two-process integration test with TCP transport
- Typed RPC API (template call<Request, Response> with serialization)

**Source Reorganization**
- `include/hpactor/` — header-only library, organized by architectural group:
  - `actor/` — Actor base classes, behaviors, typed actors, spawn
  - `ref/` — Actor references (address, ref, proxy)
  - `net/` — Networking (event loop, TLS, connection pool, transports)
  - `supervision/` — Supervision strategies
  - `core/` — Core runtime (actor_system, scheduler, mailbox, registry)
  - `sched/` — Scheduling subsystem (work_queue, edf_queue, a2ws, timing_wheel, coroutine_frame_pool)
  - `types/` — Type system (types, types_fwd, serialization)
  - `rpc/` — RPC channel (rpc_channel.hpp)
- `src/actor/` — actor_system.cpp, abstract_actor.cpp, actor_context.cpp, scheduler.cpp, event_based_actor.cpp, local_actor.cpp, spawn_receiver.cpp
- `src/net/` — event_loop.cpp, acceptor.cpp, connection.cpp, tcp_transport.cpp, frame.cpp, tls_context.cpp, tls_connection.cpp, connection_pool.cpp, registrar.cpp
- `src/ref/` — actor_proxy.cpp, actor_ref.cpp
- `src/sched/` — scheduler.cpp, worker_thread.cpp, edf_queue.cpp, a2ws.cpp, timing_wheel.cpp, coroutine_frame_pool.cpp
- `src/spawn.cpp` — AsyncActor implementation
- `src/actor_type_registry.cpp` — ActorTypeRegistry implementation
- `src/core/serialization.cpp` — DefaultSerializer implementation
- `src/rpc/rpc_channel.cpp` — RpcChannel implementation
- Tests: `tests/{actor,core,mailbox,net,ref,supervision,spawn,sched,rpc}/`

## Build Commands

```bash
# From project root - using homebrew clang-20 toolchain (Debug build)
cmake -S . -B build -GNinja \
  -DCMAKE_C_COMPILER=/opt/homebrew/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/bin/clang++ \
  -DCMAKE_BUILD_TYPE=Debug
ninja -C build

ctest --output-on-failure

# With sanitizers
cmake -DENABLE_TSAN=ON ..  # ThreadSanitizer
cmake -DENABLE_ASAN=ON ..  # AddressSanitizer (may show false positives in intrusive queue tests)

# Enable/disable examples (default ON)
cmake -DENABLE_EXAMPLES=OFF ..
```

## Known Issues

- ASAN may report false positives in `test_mailbox_awaiter` and `test_priority_scheduler` due to intrusive queue memory patterns. Tests pass cleanly with TSAN or without sanitizers.
- MPSCMailbox `dequeue()` had a cyclic queue bug when returning last element — fixed in commit dc25f18.
