# HPActor

A high-performance distributed Actor framework with million-level concurrency support. Combines work-stealing schedulers, EDF (Earliest Deadline First) real-time scheduling, multi-priority queues, and an application-defined two-tier slab memory allocator for deterministic response times without GC pauses.

## Features

### Actor Model
- **Actor Type Hierarchy**: Event-based, blocking, typed, and stateful actors with unified `ActorRef` references
- **Dynamic Behavior**: Actors change message handlers at runtime via `become()`
- **Coroutine-Powered**: C++20 stackless coroutines — thousands of actors multiplexed onto a small thread pool
- **Unified Message Passing**: `TypedMessage` wraps any protobuf payload with sender address for request/response routing
- **ActorRefCache**: Lock-free LRU cache for resolved `ActorRef` lookups, O(1) amortized
- **Error Reply**: `reply_with_error()` / `reply_with_result()` for structured error handling across the network

### Protobuf-Native Programming Model
- **proto_actor**: Base class with template handler registration — `on<T>()` for fire-and-forget, `on_request<ReqT, ResT>()` for request-response
- **ProtoStatefulActor\<T\>**: Protobuf actor with explicit state access via `state()`
- **ProtoTypeRegistry**: Maps `TypeTag` to protobuf message types with 4-byte BE TypeTag + payload wire format
- **Zero-Copy Potential**: Protobuf messages flow from wire to handler without intermediate variant wrapping

### Scheduling Subsystem
- **HybridScheduler**: Work-stealing scheduler with A2WS adaptive victim selection
- **ChaselevDeque\<T\>**: Lock-free work-stealing deque (LIFO owner pop, FIFO thief steal)
- **MultiPriorityWorkQueue**: Per-priority ChaseLev arrays — starvation-free priority scheduling
- **EDFQueue**: Earliest Deadline First min-heap for real-time work, O(log n) push/pop
- **TimingWheel**: Hierarchical O(1) timer wheel with cascading (4 levels)
- **CoroutineFramePool**: Lock-free stack pool for coroutine frames, O(1) acquire/release

### Mailbox
- **MPSCMailbox\<T\>**: Vyukov lock-free MPSC queue (wait-free enqueue, lock-free dequeue)
- **MPSCActorMailbox\<T\>**: Edge-triggered CAS wakeup — no lost wakeups, no spurious rescheduling
- **Swap-in Interface**: `IMailbox<T>` allows replacing the mailbox implementation without touching actor code

### Memory Management
- **Two-Tier Slab Allocator**: mmap-backed SegmentProvider (Tier 0) → per-thread SlabCache with bump+freelist (Tier 1), 8 size classes (32B–4KB)
- **Typed Memory Regions**: Separate allocation pools for actors, messages, coroutines, network buffers, internal structures, and hibernation
- **Thread-Local Hot Path**: Bump pointer allocation < 25ns, lock-free CAS freelist recycle < 32ns
- **Per-Actor Observability**: 64-byte cache-line-aligned atomic counter array indexed by ActorId, lock-free telemetry ring buffer
- **Debugging**: Memory poisoning (0xAA), canary verification on alloc/free, guard pages with SIGSEGV/SIGBUS handler
- **Hibernation**: Serialize actor state → `madvise(MADV_PAGEOUT)` to ZRAM → 3-4× effective memory capacity for idle actors
- **Compaction**: Generation-based slab tracking with 5% fragmentation budget, relocatable actors by ActorId
- **Zero malloc in hot path**: Custom allocator routes all actor/message/coroutine allocations away from general-purpose `malloc`

### Observability & Metrics
- **Actor-Level Metrics**: Native OpenMetrics exposition — mailbox depth, message processing latency, actor lifecycle counters, scheduler steals, supervision restarts — served via HTTP `/metrics` endpoint for Prometheus/Grafana
- **Out-of-Band Instrumentation**: Lock-free `MpscRingBuffer<T>` with CAS-based event writes — zero hot-path overhead for user actors
- **OpenMetrics Format**: `# HELP`/`# TYPE` metadata, `Counter`/`Gauge`/`Histogram` with exponential bucketing, Prometheus-compatible output
- **Per-Actor Labeling**: `actor_id` and `actor_type` labels for drill-down debugging, configurable cardinality via `per_actor_labels`
- **TOML Configurable**: `[system.metrics]` section — enable/disable, ring buffer capacity, scrape path
- **Compile-Time Disable**: `ENABLE_ACTOR_METRICS=OFF` for zero-overhead deployments

### Actor Lifecycle
- **ActorState**: Atomic state machine (Idle → Ready → Running → IOWaiting → Terminated) with CAS transitions
- **Hierarchical Supervision**: OneForOne (restart failed child) and AllForOne (restart all children) strategies
- **SupervisorActor**: Supervises children via strategy pattern
- **SelfSupervisingActor**: Manages own children with configurable policy (max restarts, restart interval)
- **Remote Child Tracking**: Supervision across process boundaries

### Networking
- **EventLoop**: kqueue (macOS) / epoll (Linux) edge-triggered event loop with timer support
- **Reactor/Proactor Separation**: `IReactorBackend` interface — `EpollBackend`/`KqueueBackend` (reactor) and `IoUringBackend`/`GcdBackend` (proactor)
- **TCP Transport**: 4-byte length-prefixed framing with `ConnectionPtr` abstraction
- **TLS 1.3**: AES-256-CBC encryption, RSA key exchange, `TlsConnection` state machine
- **Connection Pool**: Dynamic pooling per node, round-robin, exponential backoff reconnect
- **UNIX Domain Socket**: Registry-driven UDS path lookup with TCP fallback
- **Async RPC**: `RpcChannel` with at-least-once delivery, retry on timeout, `RpcFuture<bytes>`

### Service Discovery
- **Registrar**: UDP discovery + TCP registration with heartbeat, failover, and protobuf serialization
- **HostResolver**: DNS resolution with caching
- **NodeRegistry**: Registry of known nodes with static routes

### Remote Actor Spawn
- **AsyncActor**: Non-blocking spawn handle with `get()`, `ready()`, `cancel()`
- **ActorTypeRegistry**: Register spawnable actor types by name
- **SpawnReceiver**: System actor for handling spawn requests across the network
- **Well-Known System IDs**: `SpawnReceiverId`, `SystemActorType` — constexpr initialized

### Interactive CLI
- **Hierarchical Command Tree**: Trie-based `CommandNode` registry — `/actor <id> show`, `/system stats`, `/metrics show` with tab-completion-ready traversal
- **Thread-Safe Introspection**: `InspectStateRequest`/`InspectStateReply` message pair — CLI never reads actor memory directly, target actor handles request on its own thread
- **Dedicated I/O Thread**: `CliActor` extends `DaemonActor` with its own OS thread, blocks on stdin without disrupting compute workers
- **Pluggable Output Formats**: `PrettyFormatter` (ANSI box-drawing), `JsonFormatter` (machine-readable), `TabularFormatter` (grep/awk-friendly)
- **Interactive Paging**: Cursor-based `/actor list` with n/p/q/search/goto navigation, 50 actors per page
- **Virtual `to_metadata()` Interface**: Every actor exposes lightweight inspectable summary — no CLI knowledge of specific actor types needed
- **Remote Attach Ready**: Configurable UDS/TCP listener for `hpactor attach` from separate process (future frontend)
- **TOML Configurable**: `[system.cli]` section — enable/disable, listen path, default format, page size
- **Compile-Time Disable**: `ENABLE_CLI=OFF` for zero-overhead deployments

### Declarative Topology Configuration
- **TOML-Based Topology**: Declare actor trees, supervision hierarchies, and dispatcher bindings in TOML — `ActorSystem::load_topology("config.toml")` bootstraps the entire system
- **Template System**: Reusable actor templates with argument merging for DRY topology definitions
- **AOT Compiler**: `tools/toml-compiler/` compiles TOML topology to a compact binary format for production deployment
- **Binary Format**: mmap-friendly binary topology with string interning — zero-parse bootstrap

### Serialization
- **Protobuf Wire Format**: `common.proto` (endpoint types), `frame.proto` (WireFrame transport), `messages.proto` (system messages)
- **DefaultSerializer**: Protobuf-based encode/decode for all system message types
- **CommunicationEndpoint**: `std::variant<Ipv4Endpoint, Ipv6Endpoint>` — network-byte-order storage for zero-copy socket operations

## Architecture

### Actor Type Hierarchy

```
AbstractActor (interface base)
└── LocalActor (has ActorContext access)
        ├── EventBasedActor (cooperative, behavior-based, coroutine-powered)
        │       ├── StatefulActor<T> (explicit state)
        │       └── TypedEventBasedActor<Signatures...> (statically typed)
        ├── BlockingActor (thread-based, blocking receive)
        │       └── ScopedActor (for main/non-actor contexts)
        └── ProtoActor (protobuf-native, on<T>() / on_request<ReqT,ResT>())
                └── ProtoStatefulActor<T> (protobuf + explicit state)
```

### Message Flow

Actors communicate via `TypedMessage` (protobuf payload with sender address):

```cpp
context()->send(addr, msg);        // send message to actor
context()->reply(msg);             // reply to current sender
context()->reply_with_error(code); // reply with error to sender
become(Behavior);                  // change behavior dynamically
co_await mailbox_awaiter;          // suspend until message arrives
```

### Actor References

```
ActorRef (std::variant)
├── Actor        — shared_ptr to local actor (direct dispatch)
└── ActorProxy   — remote actor handle (transport-based send)
```

`ActorRef` unifies local and remote references — callers use `send()` without knowing where the actor lives. Resolution uses `ActorRefCache` for O(1) amortized lookups.

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
| `CoroutineFramePool` | Lock-free stack pool for coroutine frames |

### Memory Management

| Component | Purpose |
|-----------|---------|
| `SegmentProvider` | Tier 0: mmap-based segment acquisition (2MB segments), carves slabs for thread-local caches |
| `SlabCache` | Tier 1: per-size-class slab with bump allocator + lock-free CAS freelist |
| `ThreadLocalAllocator` | Per-thread allocator owning 8 SlabCaches (32B–4KB) |
| `MemoryTracker` | Per-actor shadow counters (64B-aligned atomic array, 1M actor capacity) |
| `TelemetryRingBuffer` | Lock-free MPSC ring buffer for allocation event sampling |
| `HibernationRegistry` | Concurrent map of ActorId → serialized hibernation buffers |
| `CompactionManager` | Generation-based slab tracking with 5% fragmentation budget |
| `ZramManager` | `madvise(MADV_PAGEOUT/COLD/WILLNEED)` hints for ZRAM integration |

### Network Layer

| Component | Purpose |
|-----------|---------|
| `EventLoop` | kqueue (macOS) / epoll (Linux) edge-triggered event loop |
| `IReactorBackend` | Unified backend interface for reactor and proactor modes |
| `EpollBackend` | Linux epoll reactor backend |
| `KqueueBackend` | macOS kqueue reactor backend |
| `IoUringBackend` | Linux io_uring proactor backend |
| `GcdBackend` | macOS GCD proactor backend |
| `TcpTransport` | TCP transport with TLS 1.3 support |
| `PlainConnection` | Raw TCP with 4-byte length-prefixed framing |
| `TlsConnection` | AES-256-CBC encryption, RSA key exchange |
| `ConnectionPool` | Dynamic pooling with exponential backoff |
| `Registrar` | UDP discovery + TCP registration with heartbeat |
| `HostResolver` | DNS resolution with caching |
| `RpcChannel` | Async RPC with at-least-once delivery and retry |

### Protobuf Serialization

| Component | Purpose |
|-----------|---------|
| `common.proto` | Shared endpoint types (ActorEndpoint, ActorAddress, Ipv4Endpoint, Ipv6Endpoint) |
| `frame.proto` | WireFrame transport format |
| `messages.proto` | System message types (Down, Exit, Link, Unlink, SpawnRequest, SpawnResponse) |
| `DefaultSerializer` | Protobuf-based encode/decode for all message types |
| `registrar.proto` | Registrar protocol messages (Register, Accept, Join, Leave, Resolve) |
| `registrar_serialization.hpp` | to_proto/parse helpers for registrar protobuf types |

### Supervision

- `OneForOneSupervisor` — only the failed child restarts
- `AllForOneSupervisor` — all children restart when one fails
- `SupervisorActor` — supervises children via strategy pattern
- `SelfSupervisingActor` — manages own children with policy (max_restarts, restart_interval)

## Build

```bash
# Configure and build
cmake -S . -B build -GNinja
ninja -C build

# Run tests (90 tests)
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
| `-DENABLE_PROACTOR=ON` | Enable proactor backend (OFF=macOS default, ON=Linux default) |
| `-DENABLE_MEMORY_TRACKING=OFF` | Disable per-actor memory tracking (default ON) |
| `-DENABLE_MEMORY_DEBUG=ON` | Enable memory poisoning + canary verification (default OFF) |
| `-DENABLE_ACTOR_METRICS=OFF` | Disable actor-level metrics subsystem (default ON) |
| `-DENABLE_CLI=OFF` | Disable interactive CLI subsystem (default ON) |

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

### Minimal dependencies

System packages: **OpenSSL** (TLS), **Protobuf** (serialization). Vendored in `third_party/`: **llhttp** (HTTP parsing), **toml++ v3.4.0** (TOML config parsing). On Linux, **liburing** is optional for the proactor backend.

### LLVM coding standards
The codebase uses LLVM style (`clang-format`) with strict warnings (`-Wall -Wextra -Wpedantic`). This ensures the code is clean, portable, and compatible with the clang toolchain used for development.

## Project Structure

```
include/hpactor/
├── actor/          — Actor base classes, behaviors, typed actors, proto actors
├── cli/            — CLI subsystem (CliActor, CommandNode, Lexer, OutputFormatter, Pager, commands)
├── config/         — TOML config topology parser, binary format, actor factory registry
├── core/           — ActorSystem, ActorContext, mailbox, registry, config
├── mailbox/        — MPSCMailbox, MPSCActorMailbox (lock-free queues)
├── metrics/        — MpscRingBuffer, MetricRegistry, Aggregator, OpenMetricsFormatter, MetricsActor
├── net/            — EventLoop, TLS, connection pool, registrar, reactor/proactor
├── ref/            — Actor references (address, ref, proxy, cache)
├── rpc/            — Async RPC channel with retry and timeout
├── sched/          — HybridScheduler, work queues, timing wheel, coroutines
├── spawn/          — AsyncActor for non-blocking remote spawn
├── supervision/    — OneForOne, AllForOne supervisors
├── mem/            — Two-tier slab allocator, hibernation, compaction, observability
├── types/          — Type system, protobuf serialization, endpoints
├── behavior.hpp    — Dynamic behavior with message_handler
├── typed_behavior.hpp — Statically typed behavior for typed actors
├── actor_context.hpp  — Actor execution context (send, reply, spawn, link)
└── actor_type_registry.hpp — Spawnable actor type registration

src/
├── actor/          — ActorSystem, EventBasedActor, SpawnReceiver, ProtoActor
├── cli/            — CliActor, lexer, command_node, formatters (pretty/json/tabular), pager
├── config/         — TOML parser, binary serializer/loader, factory registry
├── metrics/        — MetricRegistry, Aggregator, OpenMetricsFormatter, MetricsActor
├── core/           — serialization.cpp (protobuf-based)
├── net/            — EventLoop, TcpTransport, TLS, connection pool, frame
├── ref/            — ActorRef, ActorProxy implementations
├── rpc/            — RpcChannel implementation
├── sched/          — HybridScheduler, timing wheel, EDF queue, coroutine pool
├── mem/            — SegmentProvider, SlabCache, memory tracker, hibernation, guard pages
├── spawn.cpp       — AsyncActor implementation
└── actor_type_registry.cpp — ActorTypeRegistry implementation

protos/hpactor/
├── cli_messages.proto — CLI inspect/kill/list/stats/memory messages
├── common.proto    — Shared endpoint types
├── frame.proto     — WireFrame transport format
├── messages.proto  — System message types
└── registrar.proto — Registrar protocol messages

tools/toml-compiler/ — AOT compiler: TOML topology → binary format
tests/              — 95 unit tests (actor, cli, config, core, mailbox, metrics, mem, net, ref, rpc, sched, spawn, supervision)
examples/           — 9 API usage examples
third_party/        — Vendored dependencies (llhttp, toml++)
cmake/              — CMake modules (protobuf codegen, toml++ interface target)
```

## Status

### Complete (94 tests passing)

- **Actor Core**: spawn, send, reply, behaviors, typed actors, proto actors, stateful actors
- **Unified Message Passing**: TypedMessage with sender address, reply routing, error replies
- **Actor References**: ActorRef (local/proxy variant), ActorRefCache (LRU resolution cache)
- **Supervision**: OneForOne, AllForOne, SupervisorActor, SelfSupervisingActor
- **Scheduling**: HybridScheduler with work-stealing + EDF + timing wheel + coroutine frame pool
- **Coroutines**: CoroutineTask, MailboxAwaiter, TimerAwaiter, YieldAwaiter
- **Mailbox**: MPSCMailbox (Vyukov lock-free), MPSCActorMailbox (edge-triggered CAS)
- **Memory Management**: Two-tier slab allocator (mmap → thread-local slabs), typed regions, hibernation with ZRAM hints, compaction with fragmentation budget, per-actor observability, memory poisoning + canaries + guard pages
- **Network**: TLS 1.3, connection pooling, UDS support, reactor/proactor backends
- **Service Discovery**: UDP registrar + TCP registration with protobuf serialization
- **Remote Spawn**: AsyncActor with spawn_remote(), ActorTypeRegistry
- **RPC**: Async RPC channel with at-least-once delivery, retry, and timeout
- **Serialization**: Protobuf-based for all system messages (WireFrame, Down, Exit, Link, Unlink, Spawn)
- **TOML Config Topology**: Declarative topology bootstrapping with templates, imports, AOT binary compilation
- **Actor Metrics**: Out-of-band ring buffer instrumentation, OpenMetrics `/metrics` endpoint for Prometheus/Grafana
- **CLI Interactive**: Trie-based command tree with InspectState introspection, paged output, pluggable formatters (Pretty/JSON/Tabular)

### Next Steps

- Full two-process integration test with TCP transport
- Argument deserialization for passing constructor args through spawn
- Typed RPC API (`call<Request, Response>` with serialization)
- Tiny-block optimization for 32B size class in slab allocator
- Runtime configuration via environment variables for memory limits
