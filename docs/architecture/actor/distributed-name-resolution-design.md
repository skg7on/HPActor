# Distributed Actor Name Resolution — Core Concept & Architecture

## 1. Executive Summary

HPActor's `ActorDirectory` resolves actor **names** to **addresses** — but only
locally. `ActorSystem::resolve_actor("billing-service")` returns an empty handle
when the named actor lives on another node. The system can discover nodes,
route messages to remote `ActorId`s, and publish/subscribe by `ServiceKey`, yet
there is no mechanism to answer the fundamental question: **"Which node hosts
the actor named X?"**

The distributed name resolution subsystem bridges this gap. It introduces a
consistent-hash-based **home-node indirection** model: each actor name has a
deterministic **home node** (computed from the membership ring) that stores the
authoritative `name → (ActorId, EndPoint)` mapping. The home node is a
*directory role*, not a *hosting role* — the actor itself runs wherever it was
spawned. Resolution follows a three-tier cascade: local directory → TTL cache
→ home-node query. Registration pushes synchronously to the home node
(preventing duplicate-name races), while unregistration and node-departure
cleanup are fire-and-forget with generation guards.

**Key Design Decisions:**

- **Consistent-hash ring from membership.** Every node derives the same ring
  from the same `IServiceDiscovery` membership view — no coordination protocol
  needed. A name's home node is `hash(name) → ring successor`.
- **Home node ≠ host node.** The home node owns the *directory entry*; the
  actor runs wherever it was spawned. This preserves location transparency
  and keeps naming independent of placement.
- **Synchronous registration, cached resolution.** Registration waits for a
  home-node ACK (infrequent, must be correct). Resolution reads through a
  local TTL cache after the first lookup (frequent, latency-sensitive).
- **No new consensus layer.** The hash ring is purely deterministic — no Raft,
  no Paxos. Duplicate-name detection is serialized under the home node's mutex.
- **Function-pointer ports, no std::function.** All cross-component callbacks
  use fixed-size `void*` + function-pointer pairs. No `ActorSystem*` capture
  in cluster name-resolution code.

---

## 2. Problem Context

### 2.1 What Exists Today

| Component | Scope | What it resolves |
|-----------|-------|-----------------|
| `ActorDirectory::names_` | Local node | `name → ActorAddress` (local actors only) |
| `ActorLocationCache` | Local node | `ActorId → EndPoint` (cached, after you know the ID) |
| `Registrar` / `NodeRegistry` | Cluster | `EndPoint → NodeEndpoint` (node-level discovery) |
| `GossipMembership` | Cluster | Node membership + SWIM failure detection |
| `Receptionist` | Local node | `ServiceKey → ActorAddress[]` (pub/sub, not names) |
| `ClusterReceptionistCore` | Cluster (design) | G gossip of ServiceKey registrations (not implemented) |

### 2.2 The Gap

```
ActorSystem::resolve_actor("billing-service")
  → ActorDirectory::resolve_actor("billing-service")
      → names_.find("billing-service")  →  nullopt  (actor is on another node)
          → return Actor{}   ❌
```

No fallback. No "ask the cluster." No cache of remote name mappings. The system
knows which nodes exist, and it knows which actors are on this node, but it has
**no bridge** between these two facts.

### 2.3 Why Not Extend the Receptionist?

The Receptionist uses `ServiceKey` (a role/function label like
`"worker-pool"`), not actor names. Multiple actors can register under the
same key. Names are unique identifiers — one actor, one name. These are
different primitives with different consistency requirements (ServiceKeys
are best-effort groupings; names need duplicate detection). The
`ClusterReceptionistCore` design sketches gossip-based ServiceKey propagation,
which could *share infrastructure* with name propagation, but the semantics
differ enough to warrant a separate subsystem.

### 2.4 Why Not DHT-Only (No Indirection)?

A pure DHT approach (`hash(name) → owning node → actor MUST be there`) would
couple naming to placement. You couldn't name an actor "gpu-inference" and
spawn it on the GPU node unless the hash happened to point there. Home-node
indirection separates the *directory role* from the *hosting role* — the hash
determines who answers queries, not where the actor runs.

---

## 3. Architecture

### 3.1 Component Diagram

```
                         ActorSystem::resolve_actor("name")
                                    │
                    ┌───────────────▼────────────────┐
                    │  ActorDirectory::resolve_actor()│
                    │  (local names_ check — unchanged)│
                    └───────────────┬────────────────┘
                                    │ miss
                    ┌───────────────▼────────────────┐
                    │  NameResolver                   │  ← cluster glue (new)
                    │                                 │
                    │  Three-tier cascade:            │
                    │   1. ActorDirectory (local)     │
                    │   2. NameResolveCache (TTL)     │
                    │   3. Home-node query            │
                    └───┬────────────┬────────────────┘
                        │            │
        ┌───────────────▼──┐   ┌────▼──────────────────┐
        │ NameDirectory    │   │ ConsistentHashRing     │
        │ (home-node store)│   │ (from IServiceDiscovery)│
        │ name→(id,endpoint)│   │ hash(name)→home node   │
        └──────────────────┘   └────────────────────────┘
```

### 3.2 Home-Node Indirection Model

```
 Cluster Ring:  [Node-1] ── [Node-2] ── [Node-3] ── [Node-4] ── (wrap)

 hash("billing-service") = 0x8F3A... → ring successor = Node-3

                      ┌─────────────────────────────┐
                      │ Node-3 (HOME for "billing") │
                      │ NameDirectory:               │
                      │   "billing" → {              │
                      │     actor_id: 42,            │
                      │     endpoint: Node-1:9000    │  ← actor runs HERE
                      │   }                          │
                      └─────────────────────────────┘
                                      ▲
                                      │ query/register
                                      │
  ┌────────────────────┐    ┌────────────────────────┐
  │ Node-1 (HOST)      │    │ Node-2 (CALLER)         │
  │ spawns "billing"   │    │ resolve_actor("billing")│
  │ → registers with   │    │ → queries Node-3        │
  │   home Node-3      │    │ → caches result         │
  └────────────────────┘    │ → returns ActorProxy    │
                            └────────────────────────┘
```

### 3.3 Three-Tier Resolution Cascade

```
resolve_actor("billing-service")
  │
  ├─ Tier 1: ActorDirectory::names_["billing-service"]
  │    └─ hit → return Actor (local)           ← O(1) hashmap, no lock contention
  │
  ├─ Tier 2: NameResolveCache::get("billing-service")
  │    └─ hit → return ActorProxy(endpoint, id) ← shared_mutex, read-lock
  │
  └─ Tier 3: hash("billing-service") → home_node
       ├─ if home_node == self:
       │    NameDirectory::resolve("billing-service")
       │      ├─ found → cache.put() → return ActorProxy
       │      └─ not found → return nullopt
       └─ else:
            send NameResolveQuery → home_node
            await NameResolveResponse (timeout: 2s)
              ├─ found → cache.put() → return ActorProxy
              └─ not found → return nullopt
```

Tier 1 is the existing fast path — zero overhead for local actors. Tier 2
makes the second-and-later remote lookups as cheap as a `shared_mutex` read.
Tier 3 incurs one network round-trip on first lookup per cache TTL window
(30s default).

### 3.4 Registration Protocol

Registration is **synchronous** — the caller blocks until the home node ACKs.
This prevents two nodes from simultaneously registering the same name without
a distributed lock.

```
Node-1: register_actor("billing-service", actor)
  │
  ├─ ActorDirectory::publish(entry, name="billing-service")  // local commit
  │
  └─ NameResolver::on_local_register("billing-service", address, gen=1)
       │
       ├─ hash("billing-service") → Node-3
       │
       ├─ if home_node == self:
       │     NameDirectory::register("billing-service", entry)  // mutex-guarded
       │       → Ok (no duplicate)
       │
       └─ else:
            send NameRegisterRequest{name, actor_id, endpoint, gen=1} → Node-3
            await NameRegisterResponse (timeout: 5s)
              ├─ Ok → success
              ├─ DuplicateName → error returned to caller
              └─ Timeout → error, caller may retry
```

### 3.5 Node Departure

When a node fails or leaves the cluster:

1. `IServiceDiscovery::on_member_change()` fires on every surviving node.
2. `NameResolver` rebuilds the hash ring — names previously homed on the departed
   node now map to its ring successor.
3. `NameResolveCache::evict_node(departed_ep)` purges cached resolution results
   pointing to the departed node.
4. `NameDirectory::purge_by_endpoint(departed_ep)` removes entries for actors
   that were *hosted* on the departed node (those actors are gone).
5. Entries where the departed node was the *home* but the actor was *hosted
   elsewhere* are lost until the hosting node detects the failure and
   re-registers. This is acceptable — resolution fails safely (returns
   `nullopt`) rather than returning a dead actor.

---

## 4. Key Components

### 4.1 NameDirectory

A thread-safe, mutex-guarded store. Holds the authoritative entries for names
whose consistent-hash home is **this node**. Analogous to `ActorDirectory` but
stores remote-endpoint records rather than live actor handles.

```
NameEntry: { ActorId, EndPoint, generation, registered_at }

Operations:
  register(name, entry)  → Ok | DuplicateName | StaleGeneration
  resolve(name)          → optional<NameEntry>
  unregister(name)       → bool
  purge_by_endpoint(ep)  → count   // node-departure cleanup
  snapshot()             → [(name, entry), ...]
```

**Generation guard:** Each `NameEntry` carries a monotonic `generation` counter.
The home node rejects registrations with `gen <= existing.gen`, preventing a
stale retransmission from overwriting a newer registration.

### 4.2 NameResolveCache

A TTL cache following the same pattern as `ActorLocationCache` (which caches
`ActorId → EndPoint`), but keyed by `std::string` (actor name).

- `shared_mutex` with read-lock for `get()`, write-lock for `put()`/`evict()`
- Entries expire after a configurable TTL (default 30s)
- `evict_node(EndPoint)` purges all entries for a departed node in one
  write-lock pass

### 4.3 ConsistentHashRing

A deterministic hash ring built from the live node set. Every node computes
the same ring from the same membership view — no coordination.

- **Hash function:** FNV-1a 64-bit (reused from HPActor's fingerprint code)
- **Virtual nodes:** 100 per physical node (provides ~±1% imbalance)
- **Rebuild:** Called on every `IServiceDiscovery::on_member_change()` event
- **Empty ring:** When `ring_.empty()` (single-node or no discovery), all
  names are treated as local

### 4.4 NameResolver

The glue layer. Fixed dependencies at construction, no late setters, no
`std::function`. All public methods are thread-safe.

| Dependency | Role |
|-----------|------|
| `NameDirectory&` | Home-node store served by this node |
| `IServiceDiscovery&` | Membership for ring construction |
| `NameResolveCache&` | TTL cache shared across all resolutions |
| `NameResolutionConfig` | Timeouts, TTL, virtual node count |
| `OutboundNameQueryPort` | Sends messages to remote home nodes |
| `InboundNamePort` | Receives name protocol messages from peers |

### 4.5 Port Types

Three function-pointer port structs (fixed-size, no `std::function`):

| Port | Installed on | Purpose |
|------|-------------|---------|
| `NameRegistrationPort` | `ActorDirectory` | `on_register`/`on_unregister` callbacks |
| `InboundNamePort` | `InboundFrameRouter` | Dispatch incoming name-protocol frames |
| `OutboundNameQueryPort` | `NameResolver` | Send name-protocol messages to peers |

---

## 5. Wire Protocol

Five protobuf messages on five subsystem TypeTags (`0x80`–`0x84`):

| Message | Tag | Direction | Sync/Async |
|---------|-----|-----------|------------|
| `PbNameRegisterRequest` | `0x80` | Host→Home | Sync (await ACK) |
| `PbNameRegisterResponse` | `0x81` | Home→Host | Response |
| `PbNameResolveQuery` | `0x82` | Caller→Home | Sync (await response) |
| `PbNameResolveResponse` | `0x83` | Home→Caller | Response |
| `PbNameUnregisterRequest` | `0x84` | Host→Home | Fire-and-forget |

Messages bypass the `DeliveryPipeline` (no TTL, no dedup — they carry their
own generation-based idempotency). Registration and resolution requests are
routed via `InboundFrameRouter` to `InboundNamePort`; responses flow through
normal `TypedMessage` delivery back to the waiting `NameResolver`.

---

## 6. Consistency & Failure Model

| Operation | Consistency | Recovery |
|-----------|-------------|----------|
| Register | Synchronous ACK | Timeout → error, caller retries |
| Resolve | Read-through cache (30s TTL) | Timeout → nullopt, caller handles |
| Unregister | Fire-and-forget + gen guard | Home node rejects stale re-registrations |
| Node departure | Ring rebuild + cache eviction | Membership converges via gossip |

**Duplicate name detection:** Serialized under the home node's mutex. Two
concurrent `NameRegisterRequest`s for the same name → first wins, second
gets `DUPLICATE_NAME`.

**Partition tolerance:** During a partition, nodes may have different
membership views and compute different home nodes. Registration goes to the
old home; the new home has no entry. Resolution returns `not found` rather
than wrong data. When the partition heals, ring converges — no permanent
inconsistency.

---

## 7. Integration Points

### 7.1 ActorDirectory

`ActorDirectory` gains a `NameRegistrationPort` — a function-pointer pair
installed by `RuntimeBuilder`. After `publish(entry, name)` commits a name
locally, it calls `port.on_register(name, address, generation)`. During
`erase(id)`, for each removed name, it calls `port.on_unregister(name)`.
When the port is null (no `NameResolver` configured, single-node mode),
these are zero-overhead no-ops.

### 7.2 ActorSystem::resolve_actor()

After the existing `directory.resolve_actor(name)` (Tier 1), a new fallback
queries `NameResolver::resolve(name)` (Tiers 2–3). On success, constructs an
`ActorProxy` from the returned `ActorAddress`. On failure (or when
`NameResolver` is not configured), returns `Actor{}` as before.

### 7.3 InboundFrameRouter

The router checks incoming data frames: if the TypeTag is in the name-protocol
range (`0x80`–`0x84`) and the `InboundNamePort` is installed, dispatch
short-circuits to the port instead of flowing through `DeliveryPipeline`.
Response tags (`0x81`, `0x83`) pass through normal delivery to reach the
waiting `NameResolver`.

### 7.4 Runtime Lifecycle

`NameResolver` (and its owned `NameDirectory`, `NameResolveCache`,
`ConsistentHashRing`) is constructed during `RuntimeBuilder` build phase,
after `IServiceDiscovery` and transport are available. Membership change
callbacks are wired via `IServiceDiscovery::on_member_change()` — additive,
not replacing any existing callback.

---

## 8. Observability

| Signal | Type | Description |
|--------|------|-------------|
| `hpactor_name_registrations_total` | Counter | Labeled by result (ok/duplicate/timeout) |
| `hpactor_name_resolutions_total` | Counter | Labeled by source (local/cache/remote_hit/remote_miss) |
| `hpactor_name_resolution_latency_ms` | Histogram | Remote resolution latency |
| `hpactor_name_home_entries` | Gauge | Names homed on this node |
| `/cluster names` | CLI | List names homed on this node |
| `/cluster name <name> show` | CLI | Resolve a name, show entry details |
| `/cluster ring` | CLI | Show hash ring token assignments |

---

## 9. Non-Goals

- **Replacing Receptionist.** ServiceKey-based pub/sub remains separate.
  Names and ServiceKeys are different primitives.
- **Changing ActorDirectory's local contract.** `ActorDirectory::names_` stays
  local-only. Remote resolution is a higher layer.
- **Adding consensus.** No Raft, no Paxos. The hash ring is deterministic from
  the gossip membership view. Duplicate detection is per-home-node serialized.
- **Actor migration.** Names resolve to actors wherever they were spawned.
  If an actor moves (future shard rebalancing), the home node's entry updates
  — no ring rebuild needed.
- **Security/auth.** Name registration and resolution are intra-cluster
  operations between trusted nodes. Authorization is deferred to the broader
  security architecture.

---

## 10. Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Consistent-hash home nodes (not gossip propagation) | No new gossip channel; deterministic from existing membership; no conflict resolution for name ownership |
| Home node ≠ host node (indirection) | Preserves location transparency; names and placement vary independently |
| Synchronous registration, cached resolution | Registration is infrequent — correctness > latency. Resolution is frequent — cache makes it fast after first lookup |
| Generation guard for stale detection | Prevents slow/delayed messages from corrupting state without distributed locking |
| Function-pointer ports (no std::function) | Fixed size, no allocation, no exceptions, consistent with HPActor runtime port conventions |
| Subsystem TypeTags (0x80–0x84) via `make_subsystem_tag()` | Follows the established pattern for subsystem extensions; core `TypeTag` enum stays closed |
| No RTTI, no exceptions | Conforms to HPActor-wide implementation constraints |

---

## 11. References

- Design spec: `docs/superpowers/specs/2026-07-13-distributed-name-resolution-design.md`
- Implementation plan: `docs/superpowers/plans/2026-07-13-distributed-name-resolution.md`
- Issue: [#452](https://github.com/skg7on/HPActor/issues/452)
- `include/hpactor/actor/system/actor_directory.hpp` — local name registry
- `include/hpactor/net/actor_location_cache.hpp` — ActorId→EndPoint TTL cache
- `src/net/registrar.cpp` — node discovery (Registrar, NodeRegistry, UdpRegistrar)
- `include/hpactor/cluster/receptionist/cluster_receptionist_core.hpp` — ServiceKey gossip design
- [Production Reliability Plane](../production/production-reliability-plane.md) — control plane responsibilities
- [Cluster Sharding and Placement Design](../production/cluster-sharding-placement-design.md) — shard-based placement
