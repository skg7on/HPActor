# Distributed Actor Name Resolution — Design Spec

## 1. Executive Summary

HPActor's `ActorDirectory` maintains a local `name → ActorAddress` map, but there
is no mechanism to resolve an actor name when the actor lives on a remote node.
`ActorSystem::resolve_actor("billing-service")` returns an empty `Actor{}` for
any actor not registered on the local node.

This spec defines a distributed name resolution layer that bridges the existing
node discovery (`IServiceDiscovery`, `GossipMembership`) and the local
`ActorDirectory`, enabling cluster-wide name→address resolution with minimal
overhead on the hot path (every message send after the first uses cached
`ActorId`).

### 1.1 Non-Goals

- Replacing `Receptionist` / `ClusterReceptionistCore` (ServiceKey-based
  pub/sub remains separate).
- Changing the local-only contract of `ActorDirectory::names_`.
- Adding a consensus protocol (no Raft, no Paxos).
- Actor migration or transparent relocation.

## 2. Architecture

### 2.1 Core Concept: Home-Node Indirection

Each actor name has a **home node** determined by
`consistent_hash(name) → node` from the current membership ring. The home node
is a *directory role*, not a *hosting role* — it owns the authoritative
directory entry for that name, but the actor itself can run on any node.

Resolution follows a three-tier cascade:

1. **Local `ActorDirectory::names_`** — unchanged, O(1) lookup for local actors.
2. **Local `NameResolutionCache`** — TTL cache of previously resolved remote
   names (`name → ActorAddress`). One network round-trip on first lookup only.
3. **Home-node query** — hash name → home node → send `NameResolveQuery` →
   receive `(ActorId, EndPoint)` → populate cache → return `ActorProxy`.

Registration pushes to the home node synchronously (ACK before the registration
is considered complete), preventing duplicate-name races without a consensus
layer.

### 2.2 Component Diagram

```
ActorSystem::resolve_actor("name")
         │
         ▼
┌────────────────────┐
│  NameResolver       │  ← cluster glue (new)
│                     │
│  resolve():         │
│   1. ActorDirectory │  ← local check (existing, unchanged)
│   2. NameResolveCache│  ← TTL cache (new)
│   3. ConsistentHash │  ← ring from IServiceDiscovery
│   4. NameDirectory  │  ← home-node store (new)
│   5. Transport      │  ← cross-node query/register
└────────────────────┘
```

### 2.3 Component: `NameDirectory`

A thread-safe, mutex-guarded store owned by the runtime. Holds the
authoritative entries for names whose home is this node. Analogous to
`ActorDirectory` but stores remote-endpoint records rather than live actor
handles.

```
NameDirectory:
  entries_: map<string, NameEntry>

  NameEntry:
    ActorId     actor_id       // the actor's ID
    EndPoint    endpoint       // where the actor actually runs
    uint64_t    generation     // bumped on re-registration; stale-guard
    time_point  registered_at  // for observability and TTL

Methods:
  register(name, entry)     → RegisterResult
  resolve(name)             → optional<NameEntry>
  unregister(name)          → bool
  purge_by_endpoint(ep)     → size_t    // node-departure cleanup
  snapshot()                → vector    // for CLI / metrics
```

Header: `include/hpactor/cluster/name/name_directory.hpp`
Source: `src/cluster/name/name_directory.cpp`

### 2.4 Component: `NameResolver`

The glue layer bridging `ActorDirectory`, `IServiceDiscovery`, and the
home-node protocol. Owned by the runtime, not an actor. All public methods
are thread-safe.

```
NameResolver:
  Dependencies (fixed at construction, no late setters):
    - NameDirectory&              // home-node store (served by this node)
    - IServiceDiscovery&          // for membership → ring
    - NameResolutionConfig        // timeouts, TTL, virtual node count
    - NameResolveCache&           // TTL cache (shared with NameDirectory lookup path)
    - OutboundNameQueryPort       // function-pointer port for cross-node queries
    - NameRegistrationPort        // function-pointer port, installed on ActorDirectory

Internal state:
  - ConsistentHashRing ring_      // built from membership
  - mutex                         // protects ring_

Key methods:
  resolve(name)           → optional<ActorAddress>
  on_local_register(name, ActorAddress, generation)
  on_local_unregister(name)
  on_membership_change(added, removed)
  on_name_register_request(from_node, name, entry)  → NameRegisterResult
  on_name_resolve_query(from_node, name)             → NameResolveResult
  on_name_unregister_request(from_node, name, generation)
```

Header: `include/hpactor/cluster/name/name_resolver.hpp`
Source: `src/cluster/name/name_resolver.cpp`

### 2.5 Component: `NameResolveCache`

A TTL cache similar to `ActorLocationCache` but keyed by `std::string` (name)
rather than `ActorId`.

```
NameResolveCache:
  entries_: map<string, CacheEntry>
  CacheEntry: { ActorAddress, time_point expires_at }

Methods:
  get(name)                → optional<ActorAddress>
  put(name, ActorAddress, ttl)
  evict(name)
  evict_node(EndPoint)     // purge all entries pointing to a node
  purge_expired()
```

Header: `include/hpactor/cluster/name/name_resolve_cache.hpp`
Source: `src/cluster/name/name_resolve_cache.cpp`

### 2.6 Component: `ConsistentHashRing`

A deterministic hash ring built from the live node set reported by
`IServiceDiscovery`. Every node computes the same ring from the same membership
view, so every node agrees on the home node for any name without coordination.

```
ConsistentHashRing:
  nodes_: sorted map<HashToken, EndPoint>  // token → node
  replicas_: int = 100                     // virtual nodes per physical node

Methods:
  build(live_members: set<EndPoint>)   // rebuild from membership
  lookup(name: string_view)            → EndPoint   // hash → owning node
  empty()                              → bool
  size()                               → size_t
```

Hash function: FNV-1a (already used elsewhere in HPActor for fingerprints).
Virtual nodes: 100 per physical node (provides ±1% imbalance).

Header: `include/hpactor/cluster/name/consistent_hash_ring.hpp`
Source: `src/cluster/name/consistent_hash_ring.cpp`

## 3. Wire Protocol

### 3.1 Protobuf Messages

New file: `proto/name_directory.proto`

```protobuf
message PbNameRegisterRequest {
  string name = 1;
  uint64 actor_id = 2;
  string endpoint = 3;      // endpoint_ops::to_string format
  uint64 generation = 4;
}

message PbNameRegisterResponse {
  enum Result {
    OK = 0;
    DUPLICATE_NAME = 1;
    INVALID_REQUEST = 2;
  }
  Result result = 1;
}

message PbNameResolveQuery {
  string name = 1;
}

message PbNameResolveResponse {
  bool found = 1;
  uint64 actor_id = 2;
  string endpoint = 3;
  uint64 generation = 4;
}

message PbNameUnregisterRequest {
  string name = 1;
  uint64 generation = 2;   // home node rejects if gen < existing.gen
}
```

### 3.2 TypeTags

| Tag | Name | Purpose |
|-----|------|---------|
| `0x60` | `NameRegisterRequestTag` | System tag for name registration requests |
| `0x61` | `NameRegisterResponseTag` | System tag for name registration responses |
| `0x62` | `NameResolveQueryTag` | System tag for name resolution queries |
| `0x63` | `NameResolveResponseTag` | System tag for name resolution responses |
| `0x64` | `NameUnregisterRequestTag` | System tag for name unregistration requests |

Tags are in the system message range (`0x50–0x6F`), alongside existing CLI and
inspection tags.

### 3.3 Message Routing

All four messages are system messages delivered via the existing transport
layer. They bypass the `DeliveryPipeline` (no TTL, no dedup — they are
idempotent request/response pairs) and are routed directly to the
`NameResolver` on the receiving node via a dedicated handler installed on the
`InboundFrameRouter`.

## 4. Flows

### 4.1 Registration

```
Node-1: register_actor("billing-service", actor)
  │
  ├─ ActorDirectory::publish(entry, name="billing-service")   // local commit
  │
  └─ NameResolver::on_local_register("billing-service", address, gen=1)
       │
       ├─ hash("billing-service") → Node-3
       │
       ├─ if home_node == self:
       │     NameDirectory::register("billing-service", entry) → OK, return
       │
       └─ else (home_node == Node-3):
            send NameRegisterRequest{name, actor_id, endpoint, gen=1} → Node-3
            await NameRegisterResponse (timeout: 5s)
              ├─ OK → register_actor() succeeds
              └─ DUPLICATE → register_actor() returns error
```

### 4.2 Resolution

```
Node-2: resolve_actor("billing-service")
  │
  ├─ ActorDirectory::resolve_actor("billing-service")  // local check
  │   └─ hit → return Actor (no remote path)
  │   └─ miss ↓
  │
  ├─ NameResolveCache::get("billing-service")           // cache check
  │   └─ hit → return ActorProxy(endpoint, actor_id)
  │   └─ miss ↓
  │
  ├─ hash("billing-service") → Node-3
  │
  ├─ if home_node == self:
  │     NameDirectory::resolve("billing-service")
  │       ├─ found → cache.put() → return ActorProxy
  │       └─ not found → return nullopt
  │
  └─ else (home_node == Node-3):
       send NameResolveQuery{name:"billing-service"} → Node-3
       await NameResolveResponse (timeout: configurable, default 2s)
         ├─ found → cache.put() → return ActorProxy(endpoint, actor_id)
         └─ not found → return nullopt
```

### 4.3 Unregistration

```
Node-1: actor "billing-service" terminates
  │
  ├─ ActorDirectory::erase(actor_id)    // local cleanup
  │   └─ names_ entry removed
  │
  └─ NameResolver::on_local_unregister("billing-service")
       │
       ├─ hash("billing-service") → Node-3
       │
       └─ if home_node != self:
            send NameUnregisterRequest{name:"billing-service"} → Node-3
            (fire-and-forget — no ACK needed; home node purges on
             generation mismatch or explicit unregister)
```

### 4.4 Node Departure

Triggered by `IServiceDiscovery::on_member_change()` callback.

```
NameResolver::on_membership_change(added=[], removed=[Node-3])
  │
  ├─ ring_.rebuild(live_nodes)
  │    // Names that hashed to Node-3 now hash to their successor
  │
  ├─ name_directory_.purge_by_endpoint(Node-3)
  │    // Remove entries for actors hosted on the departed node
  │
  ├─ cache_.evict_node(Node-3)
  │    // Purge cached resolution results pointing to the departed node
  │
  └─ // Entries where this node was the HOME for names hosted ON Node-3
     // are purged. Entries where Node-3 was the HOME for names hosted
     // elsewhere are lost until re-registration (acceptable — the hosting
     // node detects transport failure and re-registers on reconnect).
```

## 5. Consistency Model

**Eventual consistency with synchronous registration.**

| Operation | Consistency | Rationale |
|-----------|-------------|-----------|
| Register | Synchronous (ACK from home node) | Prevents duplicate names; infrequent operation |
| Resolve | Read-through cache with TTL (30s default) | 1st lookup: 1 RTT, subsequent: cache hit |
| Unregister | Fire-and-forget + generation guard | Home node rejects stale re-registrations |
| Node departure | Ring rebuild + cache eviction | Membership view converges via gossip |

**Duplicate name detection:** The home node serializes `NameRegisterRequest`
processing under its mutex. Two concurrent registrations for the same name
to the same home node: first one wins, second gets `DUPLICATE_NAME`.

**Generation guard:** Each registration carries a monotonic generation counter.
The home node rejects any registration with `gen <= existing.gen`, preventing
a stale registration from a slow/delayed message from overwriting a newer one.

**Partition tolerance:** During a network partition, two nodes may have
different membership views and compute different home nodes for the same name.
Registration goes to the old home; the new home has no entry. When the
partition heals, the ring converges. Any resolution during the partition
that reaches the "wrong" home returns `not found`. No permanent inconsistency.

## 6. Error Handling

| Scenario | Behavior |
|----------|----------|
| Name already registered (duplicate on home node) | `RegisterResult::DuplicateName` → caller receives error |
| Home node unreachable during registration | 5s timeout → `RegisterResult::Timeout` → caller retries or picks new name |
| Home node unreachable during resolution | Configurable timeout (default 2s) → `resolve_actor()` returns `Actor{}` |
| Actor dies on hosting node | `ActorDirectory::erase()` triggers `on_local_unregister()` → home node notified |
| Stale cache entry (actor moved/died) | `ActorProxy::send()` fails → `ActorLocationCache` evicts → next resolution re-queries home node |
| Registration to wrong home (ring disagreement) | Old home rejects future registrations for re-hashed names (generation guard). Hosting node retries with new hash after membership convergence. |
| Ring empty (single-node cluster) | `ring_.empty()` → all names are local → home node is always self → no network messages |

## 7. Integration Points

### 7.1 Changes to `ActorDirectory`

- `publish(entry, name)` — after successful local commit, call
  `NameResolver::on_local_register(name, address, generation)`.
- `erase(id)` — after removing the entry, for each removed name, call
  `NameResolver::on_local_unregister(name)`.

These callbacks are installed via a function-pointer port (`NameRegistrationPort`)
to avoid a direct `NameResolver*` dependency in `ActorDirectory`. If no
`NameResolver` is configured (cluster mode disabled), the port is null and
the callbacks are no-ops.

### 7.2 Changes to `ActorSystem::resolve_actor()`

After the existing `directory.resolve_actor(name)` check (which covers local
actors), add a fallback:

```cpp
Actor ActorSystem::resolve_actor(const std::string& name) {
    // 1. Local directory (unchanged)
    auto actor_opt = impl_->actors.directory.resolve_actor(name);
    if (actor_opt.has_value()) {
        return actor_opt.value();
    }
    // 2. Cluster name resolution (new)
    if (impl_->name_resolver) {
        auto addr_opt = impl_->name_resolver->resolve(name);
        if (addr_opt.has_value()) {
            // Resolve transport for the remote endpoint
            auto* transport = impl_->network.get_transport_for(addr_opt->endpoint);
            return ActorProxy{*addr_opt, transport};
        }
    }
    return Actor{};
}
```

### 7.3 Runtime Lifecycle

- `NameResolver` is owned by `ActorSystem::Impl` (or `MessagingRuntime`).
- Constructed during `RuntimeBuilder` build phase (after `IServiceDiscovery`
  is available).
- `NameResolver::on_membership_change()` is wired into the existing
  `IServiceDiscovery::on_member_change()` callback chain (adds a callback,
  does not replace any existing one).
- `NameDirectory` is created alongside `NameResolver` — same lifetime.

### 7.4 NameRegistrationPort

```cpp
// In include/hpactor/cluster/name/name_registration_port.hpp
struct NameRegistrationPort {
    using RegisterFn = void (*)(void* context, std::string_view name,
                                 ActorAddress address, uint64_t generation);
    using UnregisterFn = void (*)(void* context, std::string_view name);

    void* context = nullptr;
    RegisterFn on_register = nullptr;
    UnregisterFn on_unregister = nullptr;
};
```

`ActorDirectory` gains a `set_name_registration_port(NameRegistrationPort)` method.
The port is set by the `RuntimeBuilder` during construction.

### 7.5 InboundNamePort

For inbound name protocol messages, the `NameResolver` exposes fixed
function-pointer ports that the `InboundFrameRouter` dispatches to:

```cpp
// In include/hpactor/cluster/name/inbound_name_port.hpp
struct InboundNamePort {
    using RegisterFn = void (*)(void* context, EndPoint from,
                                std::string_view name, ActorAddress address,
                                uint64_t generation);
    using ResolveFn = void (*)(void* context, EndPoint from,
                               std::string_view name);
    using UnregisterFn = void (*)(void* context, EndPoint from,
                                  std::string_view name, uint64_t generation);

    void* context = nullptr;
    RegisterFn on_register_request = nullptr;
    ResolveFn on_resolve_query = nullptr;
    UnregisterFn on_unregister_request = nullptr;
};
```

The `InboundFrameRouter` classifies incoming frames with
`NameRegisterRequestTag`/`NameResolveQueryTag`/`NameUnregisterRequestTag` and
dispatches to the corresponding port function. Responses
(`NameRegisterResponse`/`NameResolveResponse`) are normal TypedMessage sends
back to the requesting node's `NameResolver`.

### 7.6 OutboundNameQueryPort

For outbound queries, the `NameResolver` sends messages through the transport
via a port, avoiding a direct dependency on `Transport`:

```cpp
// In include/hpactor/cluster/name/outbound_name_query_port.hpp
struct OutboundNameQueryPort {
    using SendFn = void (*)(void* context, EndPoint target,
                            TypedMessage msg);

    void* context = nullptr;
    SendFn send = nullptr;
};
```

## 8. Configuration

TOML `[system.name_resolution]` section:

```toml
[system.name_resolution]
enabled = true                      # default: true when cluster mode is active
resolve_timeout_ms = 2000           # timeout for remote resolve queries
register_timeout_ms = 5000          # timeout for remote register requests
cache_ttl_seconds = 30              # TTL for local NameResolveCache entries
virtual_nodes = 100                 # virtual nodes per physical node in hash ring
```

Self-registering parser at `src/config/parsers/name_resolution_config_parser.cpp`.

Config struct: `config::NameResolutionConfig` in
`include/hpactor/config/name_resolution_config.hpp`.

## 9. Observability

### 9.1 Metrics

| Name | Type | Labels | Description |
|------|------|--------|-------------|
| `hpactor_name_registrations_total` | Counter | `result` (ok/duplicate/timeout) | Total name registration attempts |
| `hpactor_name_resolutions_total` | Counter | `source` (local/cache/remote_hit/remote_miss) | Total name resolutions |
| `hpactor_name_resolution_latency_ms` | Histogram | — | Remote resolution latency |
| `hpactor_name_home_entries` | Gauge | — | Names homed on this node |

### 9.2 CLI

| Command | Description |
|---------|-------------|
| `/cluster names` | List all names homed on this node |
| `/cluster name <name> show` | Resolve a name, show entry details |
| `/cluster name <name> resolve` | Full resolution walk (local → cache → remote) |
| `/cluster ring` | Show hash ring token assignments |

### 9.3 Logging

- `kNameRegistered` — name registered on home node (DEBUG)
- `kNameResolveRemote` — remote resolution performed (DEBUG)
- `kNameResolveTimeout` — remote resolution timed out (WARNING)
- `kNameRegisterDuplicate` — duplicate name rejected (INFO)
- `kNameRingRebuilt` — ring rebuilt after membership change (INFO)
- `kNameNodePurged` — entries purged for departed node (INFO)

## 10. Testing Strategy

### 10.1 Unit Tests

| Test file | Coverage |
|-----------|----------|
| `test_name_directory` | Register, resolve, unregister, duplicate rejection, generation guard, purge_by_endpoint, snapshot, empty-state |
| `test_name_resolve_cache` | Put/get, TTL expiry, evict, evict_node, purge_expired, empty-state |
| `test_consistent_hash_ring` | Build from membership, lookup determinism, same-ring-across-nodes, virtual node distribution (±1% imbalance), empty ring, single node, add/remove nodes |
| `test_name_resolver` | Local register (home==self), remote register path, local resolve (all three tiers), timeout handling, membership change callback, port-not-set no-op |

### 10.2 Integration Tests

| Test file | Coverage |
|-----------|----------|
| `test_name_resolution_two_node` | Two-node cluster: register on node-1, resolve on node-2, verify ActorProxy correctness, verify cache hit on second resolve |
| `test_name_resolution_node_departure` | Register on node-1, resolve on node-2, kill node-1, verify cache eviction and resolution returns nullopt |
| `test_name_register_duplicate` | Register same name on two nodes, verify second registration fails |
| `test_name_resolution_ring_rebuild` | Add/remove nodes, verify ring converges and resolutions succeed on new home |

### 10.3 Architecture Tests

- No RTTI/exceptions in new `src/cluster/name/` files.
- No `std::function` in `NameResolver`, `NameDirectory`, `NameResolveCache`,
  or `ConsistentHashRing`.
- No `ActorSystem*` capture in cluster name resolution code.
- `NameRegistrationPort` is fixed-size (function pointer + void* context).

## 11. File Manifest

### New Files

```
include/hpactor/cluster/name/name_directory.hpp
include/hpactor/cluster/name/name_resolver.hpp
include/hpactor/cluster/name/name_resolve_cache.hpp
include/hpactor/cluster/name/consistent_hash_ring.hpp
include/hpactor/cluster/name/name_registration_port.hpp
include/hpactor/cluster/name/inbound_name_port.hpp
include/hpactor/cluster/name/outbound_name_query_port.hpp
include/hpactor/config/name_resolution_config.hpp

src/cluster/name/name_directory.cpp
src/cluster/name/name_resolver.cpp
src/cluster/name/name_resolve_cache.cpp
src/cluster/name/consistent_hash_ring.cpp
src/config/parsers/name_resolution_config_parser.cpp

proto/name_directory.proto

tests/unit/cluster/name/test_name_directory.cpp
tests/unit/cluster/name/test_name_resolve_cache.cpp
tests/unit/cluster/name/test_consistent_hash_ring.cpp
tests/unit/cluster/name/test_name_resolver.cpp

tests/integration/cluster/name/test_name_resolution_two_node.cpp
tests/integration/cluster/name/test_name_resolution_node_departure.cpp
tests/integration/cluster/name/test_name_register_duplicate.cpp
tests/integration/cluster/name/test_name_resolution_ring_rebuild.cpp
```

### Modified Files

```
include/hpactor/actor/system/actor_directory.hpp  — add NameRegistrationPort + setter
src/actor/system/actor_directory.cpp              — call port on publish/unregister
src/actor/system/actor_system.cpp                  — resolve_actor fallback to NameResolver
include/hpactor/actor/system/actor_system.hpp     — (no public header change needed)
src/runtime/messaging_runtime.hpp                  — (if NameResolver owned here)
CMakeLists.txt                                     — add new sources + test targets
proto/CMakeLists.txt                               — compile name_directory.proto
```

## 12. References

- Issue: [#452](https://github.com/skg7on/HPActor/issues/452)
- [Cluster Receptionist Design](../production/cluster-receptionist-design.md)
- [Cluster Sharding and Placement Design](../production/cluster-sharding-placement-design.md)
- [Production Reliability Plane](../production/production-reliability-plane.md)
- `include/hpactor/actor/system/actor_directory.hpp` — local name registry
- `include/hpactor/net/actor_location_cache.hpp` — ActorId→EndPoint TTL cache
- `src/net/registrar.cpp` — node discovery (Registrar, NodeRegistry, UdpRegistrar)
