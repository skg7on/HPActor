# HPActor Project Memory

This project has a persistent memory system in `.claude/projects/-Users-skg7on-Workspace-Projects-HPActor/memory/`.

## Quick Reference

| Topic | File | Description |
|-------|------|-------------|
| Architectural Decisions | `architectural_decisions.md` | Actor model, type system, supervision strategy |
| Implemented Features | `implemented_features.md` | Complete implementation - what's built |
| Project Status | `project_status.md` | Current phase, next steps, build commands |

## Current State

**TOML Config Topology:** ✅ Complete (2026-05-04, 14 commits, ~2700 lines)
- Declarative actor topology bootstrapping — describe the full actor tree in TOML
- `TopologyModel` — ActorDef, DispatcherDef, SystemDef, ResourceSpec data structures
- `ActorFactoryRegistry` — singleton mapping behavior name strings to factory functions
- `HPACTOR_REGISTER_ACTOR` macro — static registration before main()
- `TomlParser::parse()` — import resolution (glob), template inheritance (deep merge), validation, topological sort (Kahn's algorithm)
- `BootstrapEngine` — `spawn_configured()`, dispatcher creation, behavior validation, batch spawn in DAG order
- `SystemInitTag` (TypeTag 12) — broadcast after full topology spawn to gate external traffic
- `ConfigurableActor` concept — per-actor `configure_from_args()` interface
- AOT compiler — C++ executable (`hpactor_toml_compiler`) linking hpactor_lib, shares parsing logic
- Binary format — custom mmap-friendly format with string table for zero-copy loading
- `ActorSystem::load_topology()` — end-to-end convenience: parse TOML → spawn → SystemInit
- 4 new test suites: factory registry (6), parser (13), bootstrap engine (7), binary roundtrip (3)
- `toml++` v3.4.0 as header-only FetchContent dependency

**Actor Core Framework:** ✅ Complete (Phase A-G, 65 tests passing)

**Link/Monitor:** ✅ Complete (2026-04-29)
- `link_to()`/`unlink_from()` — bidirectional death sharing via LinkMsg/UnlinkMsg
- `monitor()`/`demonitor()` — one-way death watching
- Death propagation via `on_exit()` with `DownMsg` to linked + monitored actors
- System message dispatch in `EventBasedActor::receive()` intercepts LinkMsg/UnlinkMsg/DownMsg

**Memory Management:** ✅ Complete (2026-05-03, 18 commits, 83 tests passing)
- Two-tier slab allocator — Tier 0: mmap-backed SegmentProvider (2MB segments), Tier 1: per-thread SlabCache with bump+freelist (32B–4KB size classes)
- AllocHeader (32B) + CanaryFooter (8B) on every block — owner ActorId, incarnation counter, magic canary, generation
- Lock-free CAS freelist for block recycling, CAS-based MPSC TelemetryRingBuffer for allocation events
- MemoryTracker — per-actor shadow counters (64B-aligned, lock-free), 1M actor capacity
- Typed memory regions: kActor, kMessage, kCoroutine, kNetwork, kInternal, kHibernate
- ThreadLocalAllocator per WorkerThread, global mem::allocate()/mem::deallocate() API
- Memory poisoning (0xAA) + canary verification (debug mode), guard pages with SIGSEGV/SIGBUS handler
- Hibernatable interface, HibernationRegistry (ActorId → serialized buffer), ActorState::kHibernating
- CompactionManager with generation tracking and 5% fragmentation budget
- ZramManager — MADV_PAGEOUT/COLD/WILLNEED hints for ZRAM integration
- MPSCActorMailbox refactored to use custom allocator (placement new + mem::deallocate)
- Build flags: ENABLE_MEMORY_TRACKING (ON by default), ENABLE_MEMORY_DEBUG (OFF)

**HTTP Protocol:** ✅ Complete (2026-04-30)
- HTTP parser, serializer, server (test_http_parser, test_http_serializer, test_http_server)
- Fundamental types: ActorId, error, Clock, AlarmHandle, TraceContext, MessageId, result<T>
- Actor base classes: abstract_actor, local_actor, event_based_actor
- ActorContext, ActorSystem, actor_registry
- Blocking actors: blocking_actor, scoped_actor
- Stateful actor: stateful_actor<T>
- Typed actors: typed_event_based_actor, typed_behavior
- ActorMailbox integration
- Supervision: OneForOne, AllForOne, supervisor_actor, self_supervising_actor

**Network Layer:** ✅ Complete (Phase 4-5, optional TLS 2026-04-22, comm-endpoint refactor 2026-04-23, registrar protobuf 2026-04-25)
- TlsContext — certificate loading, RSA signing, pre-master secret decryption
- Connection — abstract base, owns fd, local_endpoint, remote_endpoint, EventLoop*; handle_read() pure virtual (no args), no framing assumptions
- TlsConnection — TLS state machine, AES-256-CBC encryption, inherits from Connection
- PlainConnection — raw TCP socket, 4-byte length-prefixed framing, inherits from Connection
- ConnectionPool — standalone class (not a Connection), dynamic pooling per node, round-robin, exponential backoff
- TcpTransport — uses ConnectionPtr, creates PlainConnection or TlsConnection based on pool_config_.use_tls; connect() returns individual Connection (not pool)
- PoolConfig::use_tls defaults to false (plain text is default)
- EventLoop timer support — EVFILT_TIMER/timerfd for reconnect backoff
- EventLoop backend fallback — EpollBackend (Linux), KqueueBackend (macOS) with explicit run()/stop()
- UdpRegistrar — UDP-based node discovery with server/client dual mode
- HostResolver — DNS resolution with caching
- NodeRegistry — registry of known nodes with static routes
- RegistrarServer — TCP server for node registration, heartbeat, broadcasts
- RegistrarClient — TCP client with failover, local IP detection, AcceptorInfo
- **UNIX Domain Socket Support (2026-04-25)** — `listen_unix_domain()` in Acceptor, `connect_unix_domain()` in TcpTransport, registry-driven UDS path lookup with TCP fallback, UDS path derivation utility with `/tmp/hpactor/<node_id>.sock` convention
- **Registrar Protobuf Serialization (2026-04-25)** — `registrar.proto` with PbRegisterPayload, PbAcceptPayload, PbNodeJoinPayload, PbNodeLeavePayload, PbErrorPayload, PbResolveQueryPayload, PbResolveResponsePayload; `registrar_serialization.hpp` with to_proto/parse helpers; RegistrarServer/RegistrarClient updated to use protobuf instead of manual byte serialization
- **Async UDP (2026-04-25)** — OpCompletion extended with src_addr/src_addr_len for UDP recvfrom; UdpRegistrar async UDP via EventLoop edge-triggered polling and async_sendto
- **RegistrarServer refactor (2026-04-26)** — removed background polling thread; RegistrarServer now uses EventLoop's completion callback for send routing; added SO_REUSEADDR and error handling for UDP bind
- **ActorRef remote send (2026-04-26)** — ActorRef::send() now calls ActorProxy::send() for remote actors instead of placeholder comment
- **liburing optional (2026-04-26)** — on Linux, liburing is now optional; if not found, build uses epoll backend only without external dependencies

**CommunicationEndpoint Refactor** ✅ Complete (2026-04-23, 50 tests passing)
- NodeId (string "host:port") replaced with CommunicationEndpoint (std::variant<Ipv4Endpoint, Ipv6Endpoint>)
- Ipv4Endpoint stores uint32_t addr, uint16_t port in **network byte order** for efficient socket operations
- Ipv6Endpoint stores std::array<uint8_t, 16> addr, uint16_t port in network byte order
- ActorAddress now holds CommunicationEndpoint directly (replaced NodeId node_id field)
- endpoint_ops::parse_endpoint(NodeId) converts string to endpoint, endpoint_ops::to_string() converts back
- Binary serialization: 0x04 prefix + 7 bytes for IPv4, 0x06 prefix + 19 bytes for IPv6
- is_local() uses loopback detection (127.0.0.1 in network byte order = 0x7F000001)
- Fixed ARM Mac bug: inet_pton returns host byte order, required htonl() conversion
- ActorAddress{} default initializes to loopback (127.0.0.1:0) to match parse_endpoint("")

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

**Tests:** ✅ 87 tests passing
- Memory: test_size_class, test_alloc_header, test_freelist, test_segment_provider, test_slab_cache, test_thread_local_allocator, test_memory_stress (1M ops), test_memory_tracker, test_telemetry_ring_buffer, test_memory_poisoning, test_guard_page, test_hibernation, test_compaction, test_allocator_benchmark
- Scheduling: test_chaselev_deque, test_multi_priority_work_queue, test_hybrid_scheduler, test_edf_queue, test_a2ws, test_mailbox_awaiter, test_coroutine_scheduling, test_priority_scheduler
- UDS: test_unix_domain_socket (path derivation, acceptor, fallback), test_uds_integration (connect and data flow)

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
- Spec: `docs/superpowers/specs/2026-04-22-optional-tls-plaintext-design.md` (optional TLS, plain text default)
- Plan: `docs/superpowers/plans/2026-04-22-optional-tls-plaintext-connection.md`
- Spec: `docs/superpowers/specs/2026-04-25-unix-domain-socket-support-design.md`
- Plan: `docs/superpowers/plans/2026-04-25-unix-domain-socket-support-impl.md`
- Spec: `docs/superpowers/specs/2026-04-25-registrar-protobuf-async-udp-design.md` (registrar protobuf + async UDP)
- Plan: `docs/superpowers/plans/2026-04-25-registrar-protobuf-async-udp-plan.md`
- Spec: `docs/architecture/memory/memory-management-architecture-design.md` (memory management)
- Spec: `docs/architecture/core/actor-toml-config-core-concept.md` (TOML config philosophy)
- Spec: `docs/architecture/core/actor-toml-config-architecture.md` (TOML config detailed spec)
- Plan: `docs/superpowers/plans/2026-05-03-toml-config-topology-impl.md` (TOML config implementation)
- Plan: `docs/superpowers/plans/2026-05-03-memory-management-impl.md`

## Key Decisions

- Event-based actors (caf-style) with cooperative scheduling
- Explicit lifecycle with optional hibernation
- Both statically and dynamically typed actors
- Hierarchical supervision (OneForOne, AllForOne)
- Swap-in mailbox interface (earned lock-free through testing)
- Application-defined memory management: two-tier slab allocator (mmap → thread-local slabs), no malloc in hot path
- Typed memory regions with per-region back-pressure and observability
- Hibernation via serialization + madvise(MADV_PAGEOUT) to ZRAM for cold storage
- Actors are relocatable by ActorId, enabling slab compaction without dangling pointers
- Header-only library, C++20, no external dependencies (except OpenSSL for TLS)
- No exceptions (-fno-exceptions), no RTTI (-fno-rtti)
- constexpr ActorId constructor for constant initialization

## Current Progress

**Phase 0-10 + Memory Management + TOML Config Complete** (87 tests passing)
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

**Phase 9-10 Complete** (Unified Message Passing + Zero-Copy Net)
- Unified message passing: proto_actor.hpp deleted, replaced by TypedMessage<T> + TypedEventBasedActor
- ErrorMsg TypeTag, reply_with_error(), current_sender_ capture for reply routing
- Zero-copy read path in reactor backends via std::span (5 copies → 2)
- Unified ReadStrategy abstraction for EpollBackend/KqueueBackend
- service_read_handler gated on pending op, accumulated reads
- MSG_PEEK infinite loop fix in EpollBackend recvfrom
- Reactor/Proactor separation: IReactorBackend, ReactorDispatcher, ProactorDispatcher

**Memory Management Phase ✅ Complete (2026-05-03)**
- 8 phases (M1-M8), 18 commits, 14 new memory tests
- Performance: bump alloc 25 ns/op, freelist recycle 32 ns/op, 1M ops in 650ms

**Next Steps (Phase 11 - remaining items)**
- Proactor backend production hardening (GcdBackend, IoUringBackend)
- Full two-process integration test with TCP transport
- Per-actor argument deserialization via `configure_from_args()` (concept defined, integration pending)
- Typed RPC API (template call<Request, Response> with serialization)
- Tiny-block optimization for 32B size class (packed out-of-band metadata)
- Runtime configuration knobs (environment variables / config file)
- Multi-node TOML topology (remote actor placement via dispatcher name)

**Source Reorganization**
- `include/hpactor/` — header-only library, organized by architectural group:
  - `actor/` — Actor base classes, behaviors, typed actors, spawn
  - `config/` — TOML topology config (topology_model, actor_factory, actor_factory_registry, toml_parser, binary_format, binary_serializer, binary_loader, actor_args)
  - `ref/` — Actor references (address, ref, proxy)
  - `net/` — Networking (event loop, TLS, connection pool, transports)
  - `supervision/` — Supervision strategies
  - `core/` — Core runtime (actor_system, mailbox, registry)
  - `sched/` — Scheduling subsystem (work_queue, edf_queue, a2ws, timing_wheel, coroutine_frame_pool)
  - `types/` — Type system (types, types_fwd, serialization)
  - `rpc/` — RPC channel (rpc_channel.hpp)
  - `mem/` — Memory management (alloc_header, size_class, freelist, segment_provider, slab_cache, thread_local_allocator, memory_region, memory_config, memory_tracker, telemetry_ring_buffer, hibernation_registry, hibernatable, guard_page, compaction, zram)
- `src/actor/` — actor_system.cpp, abstract_actor.cpp, actor_context.cpp, event_based_actor.cpp, local_actor.cpp, spawn_receiver.cpp
- `src/config/` — actor_factory_registry.cpp, toml_parser.cpp, binary_serializer.cpp, binary_loader.cpp
- `src/net/` — event_loop.cpp, acceptor.cpp, connection.cpp, tcp_transport.cpp, frame.cpp, tls_context.cpp, tls_connection.cpp, connection_pool.cpp, registrar.cpp
- `src/ref/` — actor_proxy.cpp, actor_ref.cpp
- `src/sched/` — scheduler.cpp, worker_thread.cpp, edf_queue.cpp, a2ws.cpp, timing_wheel.cpp, coroutine_frame_pool.cpp
- `src/spawn.cpp` — AsyncActor implementation
- `src/actor_type_registry.cpp` — ActorTypeRegistry implementation
- `src/core/serialization.cpp` — DefaultSerializer implementation
- `src/rpc/rpc_channel.cpp` — RpcChannel implementation
- `src/mem/` — segment_provider.cpp, slab_cache.cpp, thread_local_allocator.cpp, memory_config.cpp, memory_tracker.cpp, hibernation_manager.cpp, guard_page.cpp, compaction.cpp, zram.cpp
- `tools/toml-compiler/` — AOT compiler executable (compiler.cpp)
- Tests: `tests/{actor,config,core,mailbox,net,ref,supervision,spawn,sched,rpc,mem}/`

## Build Commands

```bash
# Configure and build
cmake -S . -B build -GNinja
ninja -C build

# Run tests
ctest --output-on-failure

# With sanitizers
cmake -DENABLE_TSAN=ON ..  # ThreadSanitizer
cmake -DENABLE_ASAN=ON ..  # AddressSanitizer (may show false positives in intrusive queue tests)

# Enable/disable examples (default ON)
cmake -DENABLE_EXAMPLES=OFF ..

# Memory management options
cmake -DENABLE_MEMORY_TRACKING=OFF ..  # Disable per-actor tracking (default ON)
cmake -DENABLE_MEMORY_DEBUG=ON ..     # Enable poisoning + canaries (default OFF)
```

## Known Issues

- ASAN may report false positives in `test_mailbox_awaiter` and `test_priority_scheduler` due to intrusive queue memory patterns. Tests pass cleanly with TSAN or without sanitizers.
- MPSCMailbox `dequeue()` had a cyclic queue bug when returning last element — fixed in commit dc25f18.
