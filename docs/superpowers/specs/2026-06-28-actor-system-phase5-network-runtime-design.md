# ActorSystem Phase 5 NetworkRuntime Design

**Date:** 2026-06-28

**Status:** Proposed phase design

**Parent design:**
`docs/superpowers/specs/2026-06-27-actor-system-component-refactor-design.md`

**Prerequisites:** Phase 0 correctness stabilization and Phases 1–4 are
merged. In particular, Phase 4 provides a stable `InboundFrameSink`, one
`InboundFrameRouter`, and a `StreamRuntime` whose lifetime can be placed behind
network ingress.

**Scope:** Move event-loop/thread, transport, discovery, registrar, location
cache, network maintenance timers, RPC channel, HTTP client, and remote-spawn
network integration into one independently testable `NetworkRuntime`. Preserve
public `ActorSystem` network APIs as compatibility forwards. Do not redesign
global startup configuration or telemetry ownership; those are Phases 6 and 7.

## 1. Summary

Phase 5 establishes one owner for every resource that can create, receive, or
schedule network work:

```text
ActorSystem facade
  -> ActorSystem::Impl
       -> NetworkRuntime (optional)
            +-- TcpTransport and its authoritative EventLoop
            +-- network thread
            +-- IServiceDiscovery / UdpRegistrar
            +-- ActorLocationCache
            +-- cache and retry timers
            +-- RpcChannel
            +-- HttpClient
            +-- remote-spawn network endpoint
            +-- fixed InboundFrameSink -> InboundFrameRouter
            +-- fixed NodeEventSink ----> cluster/actor lifecycle ports
```

`NetworkRuntime` is a component owner, not a generic networking service
locator. It receives concrete dependencies and fixed function-pointer ports at
construction, performs no externally visible side effect until `start()`, and
uses one idempotent `stop()` path for normal shutdown, partial-start rollback,
and destruction.

There is one authoritative event loop: the loop owned by `TcpTransport`.
Discovery, HTTP, maintenance timers, connection I/O, and callback teardown are
bound to that loop. The current separate `ActorSystem::network_loop_` is
removed rather than migrated.

Networking-disabled systems contain no event loop, thread, transport,
discovery object, location cache, RPC channel, HTTP client, or dummy service.
The `NetworkRuntime` pointer is absent and compatibility accessors return
`nullptr` exactly as documented.

## 2. Current-State Evidence

The current constructor performs network policy and side effects directly:

- allocates `network_loop_` and stores `ActorSystem*` in it;
- starts service discovery and installs a callback capturing `this`;
- schedules location-cache and reliable-retry timers capturing facade fields;
- constructs a `TcpTransport`, whose `loop()` is a second event loop;
- constructs RPC and HTTP clients against different loop/owner combinations;
- installs transport handlers that capture `ActorSystem` and `RpcChannel`;
- starts listening before launching the thread that drives `network_loop_`;
- manually inserts the remote `SpawnReceiver` into the actor directory; and
- gives shutdown a callback that stops only `network_loop_`.

The destructor stops `network_loop_`, joins its thread, and only then calls
`transport_->stop_listening()` and `discovery_->stop()`. `TcpTransport` I/O is
not driven by the thread shown in the constructor, because the thread waits on
the separate `network_loop_` rather than `transport_->loop()`.

These facts make Phase 5 a correctness and ownership change, not a file move.

## 3. Important Correctness Findings

Every finding below requires a characterization or regression test before the
affected resource moves.

### 3.1 Two event loops create ambiguous execution and shutdown ownership

`ActorSystem` constructs `network_loop_`, while `TcpTransport` owns and exposes
its own `EventLoop`. Timers and `HttpClient` use the former; transport
connections use the latter. The network thread drives only the former.

Required contract:

- `TcpTransport::loop()` is the single Phase 5 loop;
- discovery, HTTP client, cache purge, reliable retry, and connection I/O bind
  to that loop;
- no second loop remains in the facade or component;
- all loop-bound objects are constructed and destroyed on documented threads;
  and
- loop progress is proven by real timer, RPC, and socket tests.

### 3.2 Shutdown joins the wrong loop before stopping ingress

The current order stops/joins `network_loop_` before transport listening is
disabled, and does not prove the transport loop has stopped dispatching. A
callback can therefore race handler destruction or enqueue work during actor
drain.

Required stop order:

1. atomically reject new `start()` and new outbound requests;
2. mark the inbound sink closed so late frames receive `RuntimeStopping`;
3. stop listening and disable discovery membership callbacks;
4. cancel loop timers and pending RPC/HTTP operations;
5. request transport-loop stop;
6. join the network thread exactly once;
7. clear transport/discovery handlers;
8. destroy loop-bound clients, discovery, transport, and cache; and
9. report a stable stopped snapshot.

The router, messaging runtime, stream runtime, node event sink, and telemetry
sinks outlive steps 1–8.

### 3.3 Constructor side effects cannot roll back partial network startup

Discovery starts before transport construction/listening/thread launch. A
failure after membership publication can leave discovery active or callbacks
installed. Several APIs return no error to the caller.

Required contract:

- construction only validates and stores dependencies;
- `start()` returns `result<void>` and records completed stages;
- a failure rolls completed stages back in strict reverse order;
- `stop()` succeeds from constructed, starting, running, failed, and already
  stopped states; and
- deterministic fault injection covers every stage boundary.

### 3.4 Callbacks capture the facade and depend on destruction order by accident

Discovery, timers, transport handlers, tracing, RPC, and spawn integration use
capturing lambdas whose target is the whole `ActorSystem`.

Required contract: callbacks installed by `NetworkRuntime` capture only
`NetworkRuntime*` where the component owns and joins the callback source, or
copy fixed non-owning ports whose targets have a documented longer lifetime.
No network callback captures `ActorSystem`, `ActorSystem::Impl`, or a temporary
adapter.

### 3.5 Discovery callbacks can cross stop without a quiescence guarantee

`on_member_change()` has no visible unregistration/quiescence contract. Merely
calling `stop()` does not prove an in-flight callback has returned.

Required contract:

- discovery exposes either a detachable subscription token plus `stop()`
  quiescence, or an equivalent explicit drain operation;
- `NetworkRuntime::stop()` waits for the final membership callback before
  destroying the node event sink target; and
- callbacks after the ingress gate closes are ignored and counted, never
  forwarded to actor or cluster state.

### 3.6 Timers retain raw component/facade dependencies

Cache purge and reliable-retry timers capture `this` and read pointers that
are independently reset. Timer cancellation without a completion barrier can
still race an executing callback.

Required contract: timer cancellation returns only after the callback cannot
start again; an already-running callback is included in loop-thread join
quiescence. Retry processing uses a narrow `OutboundRetryPort`, not direct
access to `ActorSystem` or internal messaging fields.

### 3.7 Remote spawn bypasses the canonical actor adoption pipeline

The current constructor manually builds `SpawnReceiver`, mailbox, context, and
directory entry. This duplicates the Phase 2 adoption invariants and ties the
network owner to actor internals.

Required contract: `NetworkRuntime` requests the remote-spawn endpoint through
a narrow Phase 2 `SystemActorPort`. `ActorRuntime` owns the actor, mailbox,
context, and directory entry. `NetworkRuntime` owns only the transport-facing
spawn protocol client/handler and its callback registration.

### 3.8 Disabled networking still leaks assumptions into callers

Several accessors return raw pointers and callers may assume an event loop or
transport exists even when `enable_network` is false.

Required contract:

- no dummy network services are constructed;
- compatibility pointer accessors return `nullptr`;
- result-returning component operations return `NetworkDisabled`;
- startup/readiness does not wait for a network stage when disabled; and
- network-disabled unit tests link without needing discovery or transport
  test doubles.

### 3.9 Stop can be invoked from the network thread

Node-death, error, or administrative callbacks can eventually request system
shutdown. Joining the current thread would deadlock.

Required contract: a network-thread stop request only closes ingress and posts
the stop transition to the lifecycle owner. `NetworkRuntime::stop()` detects
the loop thread and returns `StopDeferred`; the Phase 6 coordinator performs
the final join from a non-network thread. Phase 5 supplies and tests this hook
without becoming the global coordinator.

## 4. Goals and Non-Goals

### 4.1 Goals

- One explicit owner for all network resources and callbacks.
- One event loop and one joinable thread.
- Side-effect-free construction and result-returning, reversible startup.
- Idempotent, callback-safe shutdown.
- Stable concrete hot paths and fixed narrow ports.
- Source-compatible facade accessors and remote operations.
- Independent network tests with fake ports and real loop/socket coverage.

### 4.2 Non-goals

- Global startup blueprint or topology-before-construction factory (Phase 6).
- Moving metrics/log/tracing ownership (Phase 7).
- Replacing cluster type erasure (Phase 7).
- Removing public raw network accessors or introducing facade PImpl (Phase 8).
- Changing HPAC/protobuf formats, delivery semantics, discovery protocols, TLS
  defaults, or retry policy.
- Adding a generic DI container, service locator, or virtual call per frame.

## 5. Target Architecture

### 5.1 Component boundary

Public component header (`include/hpactor/runtime/network_runtime.hpp`):

```cpp
class NetworkRuntime final {
public:
    enum class State : uint8_t {
        Constructed,
        Starting,
        Running,
        Stopping,
        Stopped,
        Failed,
    };

    NetworkRuntime(NetworkRuntimeConfig,
                   InboundFrameSink,
                   NodeEventSink,
                   OutboundRetryPort,
                   RemoteSpawnPort,
                   NetworkTelemetryPort) noexcept;
    ~NetworkRuntime();

    result<void> start() noexcept;
    result<void> stop(StopMode mode = StopMode::Drain) noexcept;

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] NetworkSnapshot snapshot() const noexcept;

    net::EventLoop* event_loop() noexcept;
    net::TcpTransport* transport() noexcept;
    net::IServiceDiscovery* discovery() noexcept;
    RpcChannel* rpc_channel() noexcept;
    net::HttpClient* http_client() noexcept;
};
```

The component header lives under `include/hpactor/runtime/`. Public compatibility
methods forward through `ActorSystem::Impl`; applications do not construct or
depend on `NetworkRuntime` directly.

### 5.2 Effective configuration

`NetworkRuntimeConfig` is an already-effective value object. It contains only
network concerns: enabled state, endpoint, TCP/TLS/pool limits, discovery
configuration or injected discovery factory, HTTP-client enablement, RPC ask
retry limit, frame bounds, cache purge period, and retry poll period.

It contains no TOML parser objects, no `ActorSystem::Config&`, and no mutable
back-reference. Phase 5 builds it from current config in the shell. Phase 6
builds the same value from an immutable `RuntimeBlueprint`.

### 5.3 Narrow ports

Ports are fixed-size non-owning values with a context pointer and function
pointer. They do not allocate or perform lookup on the frame path.

```cpp
struct NodeEventSink {
    void* context{};
    void (*member_changed)(void*, const net::Member&, bool) noexcept{};
};

struct OutboundRetryPort {
    void* context{};
    void (*process_due)(void*, uint64_t now_ns) noexcept{};
};

struct RemoteSpawnPort {
    void* context{};
    result<ActorAddress> (*install_receiver)(void*, net::Transport&) noexcept{};
    void (*remove_receiver)(void*) noexcept{};
};
```

`InboundFrameSink` remains the Phase 4 contract. Telemetry is a stable bounded
sink and may be null. Port targets are created before `NetworkRuntime` and
destroyed after it is stopped and destroyed.

### 5.4 One loop model

`TcpTransport` remains the owner of the authoritative `EventLoop` in Phase 5.
`NetworkRuntime` obtains it through `transport.loop()` and passes that same
address to registrar/discovery integrations, HTTP client, and timers. The
network thread drives it until a stop request.

This avoids a broad transport constructor/API migration. A later transport
refactor may invert loop ownership, but only after proving an operational need.

### 5.5 Ownership table

| Resource | Phase 5 owner | Notes |
|---|---|---|
| `TcpTransport` | `NetworkRuntime` | Owns authoritative loop |
| network thread | `NetworkRuntime` | Only driver of transport loop |
| discovery/registrar | `NetworkRuntime` | Injected or configured factory |
| location cache | `NetworkRuntime` | Created only when enabled |
| cache/retry timers | `NetworkRuntime` | Registered on transport loop |
| `RpcChannel` | `NetworkRuntime` | Handler installed before listen |
| `HttpClient` | `NetworkRuntime` | Uses transport loop |
| remote-spawn protocol adapter | `NetworkRuntime` | Actor instance stays in `ActorRuntime` |
| frame router/stream state | existing runtimes | Outlive network owner |
| reliable tracker/policy | `MessagingRuntime` | Exposed through retry port |

### 5.6 Lifecycle state machine

Legal transitions:

```text
Constructed -> Starting -> Running -> Stopping -> Stopped
                    \-> Failed -----> Stopping -> Stopped
Constructed ------------------------> Stopped
Stopped --start()-------------------> Stopped + AlreadyStopped error
```

Concurrent calls are serialized by a lifecycle mutex/condition variable; the
mutex is never held while joining, invoking a port, or running transport code.
Only the first stop caller performs teardown. Other callers wait for `Stopped`
and receive the same terminal result.

`start()` is idempotent while running. Starting a stopped instance is rejected;
restart requires construction of a new component so stale callbacks, timers,
or connection state cannot be reused accidentally.

### 5.7 Startup transaction

The exact startup stages are:

1. construct transport and obtain its loop;
2. construct location cache, RPC, optional HTTP, and discovery;
3. install the fixed frame, RPC, spawn, and node event callbacks;
4. install the actor-owned remote-spawn receiver through `RemoteSpawnPort`;
5. register maintenance timers;
6. start discovery without publishing ready state;
7. start listening;
8. launch the loop thread and pass a progress barrier;
9. publish `Running` and the local discovery-ready state.

Each successful stage pushes a fixed rollback action. Failure executes actions
in reverse and leaves no thread, listener, timer, subscription, actor endpoint,
or callback installed.

### 5.8 Shutdown and callback lifetime

The Phase 5 shell invokes `network.stop()` before actor/stream/router
destruction. The component ingress gate is an atomic state checked before any
port call. Clearing callbacks happens only after the loop thread joins, so a
callback can never observe a destroyed target.

`NetworkRuntime`'s destructor calls `stop(Abort)` as a defensive invariant. It
does not implement a second teardown sequence. A debug assertion records any
deferred self-stop that was not completed by the owner.

### 5.9 Compatibility surface

Existing methods remain source compatible:

- `event_loop()`, `transport()`, `get_transport_for()`, `registrar()`,
  `rpc_channel()`, and `http_client()` forward to the optional component;
- disabled/not-running pointer accessors return `nullptr`;
- remote send/ask/spawn methods preserve signatures and map component errors to
  current results or documented no-op behavior; and
- direct transport handler APIs remain available, but the Phase 4 unified sink
  stays authoritative.

New internal code receives a `NetworkRuntime&` or a narrow port. It must not
add a new `ActorSystem` dependency because compatibility methods exist.

## 6. Concurrency Contract

- The network thread is the single consumer of event-loop callbacks.
- Public outbound APIs may be called from any actor/scheduler thread and post
  bounded work through existing transport mechanisms.
- Lifecycle state is synchronized independently from transport data locks.
- No lifecycle mutex is held across loop wait, join, discovery, transport,
  router, actor, telemetry, or user callback code.
- Discovery subscription teardown and loop join are quiescence barriers.
- The ingress gate is acquire/release; after `stop()` publishes `Stopped`, no
  port callback can begin.
- Snapshot counters are atomic or copied on the owning thread; snapshotting
  never blocks the loop on actor or CLI work.
- Existing mailbox MPSC and actor single-consumer rules remain unchanged.

## 7. Error and Failure Semantics

Network startup and operations use explicit error codes including:

- `NetworkDisabled`;
- `InvalidNetworkConfig`;
- `DiscoveryStartFailed`;
- `ListenFailed`;
- `LoopThreadStartFailed`;
- `RemoteSpawnInstallFailed`;
- `RuntimeStopping`;
- `StopDeferred`; and
- `NetworkAlreadyStopped`.

Errors carry a bounded stage and subsystem identifier, not unbounded exception
text. Start failure records the primary error and any rollback failure
counters. A rollback failure is observable but does not prevent later cleanup
attempts.

## 8. Observability and Operations

`NetworkSnapshot` contains bounded values only:

- state and enabled flag;
- endpoint/listen status;
- discovery status and member count;
- active/idle connection counts;
- pending RPC/HTTP counts;
- cache entry count;
- callback rejection count after ingress close;
- last startup/stop stage and error code; and
- loop-thread running/joined flags.

Metrics/logging use the existing injected sinks. Phase 5 does not own or
replace them. CLI/admin code may consume the snapshot but must not traverse
transport internals.

## 9. Security and Resource Bounds

- Phase 4 frame limits remain enforced before dispatch.
- Timer periods, connection-pool limits, pending RPC bounds, and discovery
  member data use effective validated configuration.
- No work is accepted after the stop ingress gate closes.
- Discovery callbacks and remote spawn frames are never allowed to retain the
  component or facade beyond stop.
- TLS material and behavior remain unchanged; secret values are not included
  in snapshots or logs.

## 10. Migration Sequence

1. Characterize the two-loop behavior and callback/stop order.
2. Add private config, ports, snapshot, state machine, and fake-backed tests.
3. Move transport and authoritative loop/thread ownership.
4. Move discovery/registrar and location cache.
5. Move timers and reliable-retry port wiring.
6. Move RPC/HTTP ownership and callbacks.
7. Replace manual remote-spawn insertion with the Phase 2 system-actor port.
8. Forward compatibility methods through `Impl`.
9. Delete facade/shell network fields and old callback setters.
10. Add architecture, integration, ASan, and TSAN evidence.

Every step keeps the tree buildable. There is never a period with two active
network runtimes or two loop threads.

## 11. Testing Strategy

### 11.1 Unit tests

- state transitions and concurrent idempotent stop;
- reverse rollback at every startup stage;
- no object creation when disabled;
- one loop address supplied to transport-adjacent clients;
- ingress rejection after stop begins;
- discovery subscription quiescence;
- timer cancellation and retry-port routing;
- remote-spawn install/remove pairing; and
- bounded snapshot/error results.

### 11.2 Integration tests

- plain/TLS frame routing on the authoritative loop;
- discovery join/leave and node-death forwarding;
- RPC response, timeout, shutdown cancellation, and late response;
- HTTP client progress on the same loop;
- remote spawn success/failure/stop;
- graceful and abort stop with active connections; and
- network-disabled actor-only startup.

### 11.3 Concurrency and sanitizer evidence

- TSAN concurrent send/snapshot/stop;
- stop requested from the loop thread followed by owner-thread completion;
- callback race at discovery/transport teardown;
- repeated start-fail-destroy and start-stop-destroy under ASan; and
- no timing-only assertions: use loop barriers and explicit latches.

## 12. Acceptance Criteria

1. No event-loop, network-thread, transport, discovery, registrar, network
   timer, location-cache, RPC-channel, HTTP-client, or remote-spawn network
   owner field remains in `ActorSystem` or the generic Phase 1 shell.
2. There is exactly one network event loop and one loop-driving thread.
3. `NetworkRuntime` construction has no network side effect.
4. Start failure rolls back every completed stage in reverse order.
5. Stop is idempotent and joins the loop before callback targets are destroyed.
6. No network callback captures `ActorSystem` or `Impl`.
7. Remote-spawn actor adoption goes through the Phase 2 actor owner.
8. Networking-disabled construction creates no dummy network services.
9. Existing public network APIs compile without source changes.
10. Focused normal, ASan, TSAN, integration, and architecture checks pass.

## 13. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Changing loop ownership breaks transport progress | First add real socket/timer characterization; assert one loop identity |
| Discovery implementation cannot quiesce callbacks | Add subscription token/drain contract before moving ownership |
| Self-stop deadlocks on join | Return `StopDeferred`; complete join on owner thread |
| RPC/HTTP callbacks outlive component | Close ingress, cancel, join, then clear and destroy |
| Remote spawn move changes system actor ordering | Use Phase 2 `SystemActorPort` and characterize address/adoption order |
| Compatibility raw pointers escape past stop | Preserve current API but return null when unavailable; deprecate in Phase 8 |
| Component becomes a new God Class | Restrict it to network resource ownership/lifecycle; protocol policy stays in router/messaging/stream |

## 14. Decision Summary

- Use component composition, not mixins or inheritance, for network ownership.
- Make `TcpTransport::loop()` the single Phase 5 event loop.
- Keep protocol classification out of `NetworkRuntime`.
- Use concrete dependencies and fixed ports, not a DI container.
- Construct without side effects; start and stop explicitly with typed results.
- Quiesce callback sources before destroying targets.
- Keep remote-spawn actor ownership in `ActorRuntime`.
- Preserve public APIs through thin forwards and remove them only through a
  later compatibility policy.
