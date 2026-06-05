# ActorSystem Refactor Design Spec

## Problem Statement

`ActorSystem` is the runtime facade for HPActor, but its implementation has grown into a mixed ownership point for actor identity, spawning, local delivery, remote delivery, topology bootstrap, backpressure, dead letters, metrics, scheduler wiring, network lifecycle, and graceful shutdown. The current shape is hard to reason about because `src/actor/actor_system.cpp` is about 1420 lines and `include/hpactor/core/actor_system.hpp` is about 859 lines, with multiple independent runtime concerns sharing private state and locks.

The complexity is already producing correctness risks. The current implementation contains at least two concrete behavior bugs that should be fixed during the refactor with failing tests first:

- `ActorSystem::resolve_actor()` finds a named actor address but returns a default `Actor` instead of the resolved actor.
- Remote spawn argument bytes are accepted by `spawn_remote()` and `spawn_remote_async()` APIs but are not passed through the remote spawn path.

This refactor keeps `ActorSystem` as the public facade while moving ownership and policies into smaller runtime components with explicit contracts.

## Design Inputs

- `docs/architecture/core/actor-core-concept.md`
- `docs/architecture/core/unified-message-passing.md`
- `docs/architecture/production/production-reliability-plane.md`
- `docs/architecture/production/graceful-shutdown-rolling-upgrade-design.md`
- `include/hpactor/core/actor_system.hpp`
- `src/actor/actor_system.cpp`
- `src/actor_type_registry.cpp`
- Existing actor, spawn, delivery, backpressure, topology, and shutdown tests under `tests/`

## Goals

- Preserve source-compatible public actor APIs and default runtime behavior.
- Keep `ActorSystem` as the single user-facing runtime facade.
- Split `ActorSystem` internals by data plane, control plane, and operations plane responsibilities.
- Make actor identity, mailbox ownership, and actor context lookup explicit in one directory component.
- Keep unified message passing semantics: local and remote ingress must converge on `try_deliver_local()` or the extracted equivalent.
- Fix the named actor resolution and remote spawn argument propagation bugs with tests.
- Shrink `actor_system.cpp` and the private state section of `actor_system.hpp` over staged changes.
- Keep no-exceptions and no-RTTI constraints intact.
- Keep production-facing observability visible through existing metrics, logs, DLQ, health, and CLI surfaces.

## Non-Goals

- No rewrite of the actor runtime.
- No public API break for actor spawning, `ActorContext::send()`, `ActorRef::send()`, `load_topology()`, or shutdown APIs.
- No new production features such as durable delivery, sharding, placement, security, or reliable replay.
- No public `toml++` exposure.
- No `dynamic_cast`, `typeid`, exception-based control flow, or RTTI-dependent ownership model.
- No change to protobuf `TypedMessage` type tag semantics.
- No broad scheduler, mailbox, network transport, or metrics redesign.

## Current Responsibility Map

`ActorSystem` currently owns these concerns directly:

- Runtime identity: node name, endpoint, actor ids, named actor registry, address lookup.
- Local actor storage: actor instances, mailboxes, contexts, scheduler wakeup, lifecycle hooks.
- Spawn: template spawn path, configured topology spawn, actor type registry bridge, remote spawn calls.
- Delivery: local delivery, remote delivery, message dedup, expiry, DLQ capture, sender backpressure.
- Backpressure: local callback dispatch, metric emission, remote control-frame serialization.
- Remote runtime: event loop thread, transport, registrar/discovery, location cache, remote spawn receiver, RPC channel.
- Topology: TOML parse and bootstrap convenience entry point.
- Shutdown: readiness, ingress draining, actor drain loop, discovery leave, telemetry flush, forced stop.

The refactor should make each concern independently testable while preserving the facade methods that existing users call.

## Proposed Component Boundaries

### ActorDirectory

Owns local actor runtime entries:

- Actor id allocation.
- Actor instance lookup by `ActorId`.
- Mailbox lookup by `ActorId`.
- Actor context lookup by `ActorId`.
- Name to address registry.
- Local actor snapshots for metrics, shutdown, and introspection.

Concurrency contract:

- Owns the actor maps and their mutex.
- Never invokes scheduler, actor, mailbox, metrics, or user callbacks while holding the directory lock.
- Provides snapshot APIs when callers need to iterate.
- Returns stable `std::shared_ptr` handles for mailboxes and contexts where lifetime must extend past lookup.

### ActorSpawner

Owns local actor construction orchestration that is currently split across the `ActorSystem` template and non-template helpers:

- Allocates an id through `ActorDirectory`.
- Builds mailbox and context.
- Attaches context to `LocalActor`.
- Registers actor in `ActorDirectory`.
- Starts lifecycle actor hooks.
- Updates actor metrics registration.
- Enqueues initial scheduler work when needed.

The public `ActorSystem::spawn<T>()` template can remain in the header, but it should delegate to a smaller compiled helper after constructing the actor object.

### LocalDeliveryEngine

Owns local delivery admission:

- Actor existence lookup.
- Message deduplication.
- Expiry checks.
- Mailbox enqueue.
- Delivery result mapping.
- DLQ record construction for missing actor, expired message, duplicate, and rejected mailbox delivery.
- Scheduler wakeup on accepted messages.

It should become the implementation behind `ActorSystem::try_deliver_local()` and `ActorSystem::deliver_local()`.

Concurrency contract:

- Does not hold the directory lock while enqueueing into a mailbox.
- Emits backpressure after mailbox admission returns a pressure signal.
- Records DLQ entries through a narrow DLQ sink.

### BackpressureCoordinator

Owns backpressure policy fanout:

- Local sender callback delivery.
- Metric event emission.
- Remote control frame serialization.
- Test wire sink currently exposed by `set_backpressure_signal_wire_sink_for_test()`.

It receives directory lookup functions and metric sinks from `ActorSystem` wiring, but it does not own actor maps.

### RemoteRuntime

Owns remote runtime lifecycle:

- Event loop startup and shutdown.
- Transport ownership.
- Registrar/discovery integration.
- Location cache.
- RPC channel wiring.
- Spawn receiver registration.
- Remote message ingress into local delivery.

`ActorSystem` keeps facade methods such as `deliver_remote()` and `spawn_remote_async()`, but delegates network-specific work to `RemoteRuntime`.

### RemoteSpawnClient

Owns remote spawn request construction and response translation:

- Resolves a node name to an endpoint.
- Serializes actor type, arguments, and argument type metadata.
- Sends the request over the RPC channel.
- Converts the response into `result<ActorRef>` or `AsyncActor`.

The first behavioral fix in this area is to pass the supplied `StreamBuffer args` through all remote spawn call paths.

### TopologyBootstrapper

Owns `load_topology()` orchestration:

- Parses TOML with `config::TomlParser`.
- Builds `config::TopologyModel`.
- Executes `config::BootstrapEngine`.
- Returns a typed `result<void>`.

This component should not expose `toml++` in public headers.

### ShutdownCoordinator

Owns the graceful shutdown phase machine:

- `Running`
- `DrainingIngress`
- `DrainingActors`
- `LeavingCluster`
- `FlushingTelemetry`
- `Stopped`
- `ForcedStop`

It should receive narrow adapters for actor snapshots, actor drain requests, discovery leave, remote runtime stop, telemetry flush, and readiness state updates.

Concurrency contract:

- Phase updates use release/acquire semantics through the facade.
- Actor ids are snapshotted before drain requests.
- Blocking waits do not run while holding directory locks.

## Runtime Flow After Refactor

### Local Spawn Flow

1. `ActorSystem::spawn<T>()` constructs `T`.
2. `ActorSystem` delegates registration to `ActorSpawner`.
3. `ActorSpawner` creates mailbox and context using existing config defaults.
4. `ActorDirectory` stores the actor slot and optional name.
5. Metrics registration and lifecycle start hooks run after the actor is visible.
6. The returned `Actor` and `ActorRef` preserve existing address semantics.

### Local Delivery Flow

1. `ActorContext::send()` or `ActorRef::send()` calls the `ActorSystem` facade.
2. The facade delegates to `LocalDeliveryEngine`.
3. `LocalDeliveryEngine` checks dedup and expiry policy.
4. `ActorDirectory` returns a mailbox handle for the target id.
5. The mailbox admission result stays as `mailbox::EnqueueResult`; callers that need the public delivery surface still use `deliver_with_result()` to map it to `DeliveryResult`.
6. Accepted messages wake the scheduler.
7. Failed delivery records DLQ and emits backpressure or metrics as before.

### Remote Receive Flow

1. `RemoteRuntime` decodes the wire message.
2. The decoded `TypedMessage` enters the same local delivery engine used by local sends.
3. Remote-specific metadata is preserved in delivery options and tracing context.
4. DLQ, metrics, and backpressure behavior stay aligned with local delivery.

### Remote Spawn Flow

1. `ActorSystem::spawn_remote()` forwards actor type and `StreamBuffer args`.
2. `RemoteSpawnClient` serializes actor type, args, and args type.
3. The remote spawn receiver calls `ActorTypeRegistry::spawn()` with the same bytes.
4. The registered factory constructs the actor from the received args.
5. The response maps to `ActorRef` with the remote endpoint preserved.

### Shutdown Flow

1. `ActorSystem::shutdown()` delegates to `ShutdownCoordinator`.
2. Ingress draining stops new external work.
3. Actor ids are snapshotted through `ActorDirectory`.
4. Actor drain requests are issued without holding directory locks.
5. Remote runtime and discovery leave steps run through narrow adapters.
6. Metrics/logging flush and readiness updates preserve current operations behavior.

## Header Dependency Strategy

The refactor should avoid replacing one large `.cpp` with several large public headers. New implementation-heavy types should live in compiled sources, with only narrow headers needed by tests and facade wiring.

Preferred layout:

- `include/hpactor/actor/actor_directory.hpp`
- `src/actor/actor_directory.cpp`
- `include/hpactor/actor/local_delivery_engine.hpp`
- `src/actor/local_delivery_engine.cpp`
- `include/hpactor/actor/backpressure_coordinator.hpp`
- `src/actor/backpressure_coordinator.cpp`
- `include/hpactor/actor/shutdown_coordinator.hpp`
- `src/actor/shutdown_coordinator.cpp`
- `include/hpactor/net/remote_runtime.hpp`
- `src/net/remote_runtime.cpp`
- `include/hpactor/spawn/remote_spawn_client.hpp`
- `src/spawn/remote_spawn_client.cpp`
- `include/hpactor/config/topology_bootstrapper.hpp`
- `src/config/topology_bootstrapper.cpp`

If a type is only needed inside `ActorSystem`, prefer a private header under `src/actor/` instead of adding public API surface.

## Observability Contract

Existing operations-plane signals must remain intact:

- Dead letters still appear in `DeadLetterQueue`.
- Backpressure metrics still use `metrics::MetricEventType::kBackpressureSignal`.
- Actor metrics registration remains source-compatible.
- CLI/admin behavior backed by actor listing, DLQ, inspect, and shutdown state remains stable.
- Remote spawn and delivery failures keep typed `result<T>` failures rather than exceptions.
- Shutdown phase visibility through `shutdown_phase()` and readiness remains stable.

## Testing Strategy

Use TDD for behavior changes and characterization tests for non-behavioral extraction:

- Add failing tests for named actor resolution and remote spawn args propagation before implementation.
- Add focused unit tests for `ActorDirectory`.
- Add focused tests for `LocalDeliveryEngine` admission results and DLQ routing.
- Keep existing integration tests for `ActorSystem` spawn, delivery, backpressure, topology, remote spawn, and shutdown passing.
- Add stress or sanitizer runs only for changes that alter lock-free, scheduler, mailbox, timer, or transport behavior.

## Migration Phases

1. Characterize and fix known correctness bugs.
2. Extract actor registry implementation from `actor_system.cpp` if still colocated.
3. Introduce `ActorDirectory` and migrate map/name/lookup operations.
4. Move local spawn registration into `ActorSpawner` or compiled spawn helpers.
5. Extract `LocalDeliveryEngine`.
6. Extract `BackpressureCoordinator`.
7. Extract `RemoteRuntime` and `RemoteSpawnClient`.
8. Extract `TopologyBootstrapper`.
9. Extract `ShutdownCoordinator`.
10. Clean up `ActorSystem` private state and includes.

Each phase should leave the repository buildable and should commit independently.

## Acceptance Criteria

- `ActorSystem::resolve_actor()` returns the registered actor for a valid name.
- Remote spawn APIs pass `StreamBuffer args` to the remote factory path.
- Public actor APIs remain source-compatible.
- `src/actor/actor_system.cpp` no longer owns all listed concerns directly.
- The private state in `include/hpactor/core/actor_system.hpp` is reduced to facade wiring and stable public API support.
- Existing targeted test binaries pass after each implementation phase.
- Full test suite is run before the refactor branch is proposed for merge.
- Design docs identify concurrency and observability contracts for changed runtime paths.

## Risks And Mitigations

- Risk: moving actor map ownership can introduce lifetime bugs.
  Mitigation: return stable shared handles from `ActorDirectory`, snapshot before iteration, and test actor removal and shutdown paths.

- Risk: local delivery changes can diverge local and remote semantics.
  Mitigation: keep one local delivery engine for both local sends and remote ingress.

- Risk: shutdown extraction can deadlock if callbacks run under locks.
  Mitigation: phase changes and actor snapshots happen before blocking waits or actor callbacks.

- Risk: remote spawn args fixes can expose factory contract gaps.
  Mitigation: test serialization, `ActorTypeRegistry::spawn()`, and integration receiver behavior separately.

- Risk: header refactors can ripple across public includes.
  Mitigation: start with compiled helpers and private implementation headers where possible.
