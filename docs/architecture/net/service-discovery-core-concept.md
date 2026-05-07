# Service Discovery — Core Concept and Design Philosophy

## 1. The Problem: From Single-Server to Multi-Server

HPActor's embedded `UdpRegistrar` works well for a single server or two-server setup:
one node becomes the authoritative registrar, others connect as clients, heartbeats
flow, and everyone knows about everyone else. The architecture is simple and the
code is self-contained.

The cracks appear when you go to production with multiple servers, each with
different roles:

```
Server A: HTTPGatewayActor, CliActor         (ingress + ops)
Server B: WorkerActor × 100                  (compute pool)
Server C: AggregatorActor, MetricsActor      (analytics)
Server D: WorkerActor × 100                  (compute pool)
```

In this topology, Server A's `RegistrarServer` holds the only copy of the
membership table. If Server A is restarted, killed by the OOM killer, or its NIC
fails — **the entire cluster loses its service directory**. Servers B, C, and D
still have TCP connections to each other (actor messages can flow), but they
cannot discover new peers, cannot detect that A is dead, and cannot onboard a
new Server E.

### The specific failures:

| Failure | Effect on current design |
|---------|------------------------|
| Registrar server crashes | All membership state lost. New nodes can't join. |
| Network partition | Split-brain: two nodes both try to become server. No quorum. |
| Transient DNS/network blip | `RegistrarClient` reconnects blindly to the same dead server for 5 retries, then `failover()` races with other clients. |
| Silent NIC failure | Server appears alive (heartbeats) but can't receive actor messages. Registrar doesn't detect this. |
| Rolling restart | Each restart triggers a registrar election. Registry state is rebuilt from scratch. |

These are not edge cases — they're what every distributed system faces on a
long enough timeline.

## 2. The Three Layers of Discovery

Service discovery in an actor framework is really three separate problems:

```
Layer 1: MEMBERSHIP
  "Who is alive right now?"
  → Failure detection, liveness, join/leave events
  → SWIM gossip, heartbeats, phi-accrual detectors

Layer 2: METADATA
  "What does each live node do?"
  → Actor types hosted, load, capabilities, ports
  → Piggybacked on membership messages or separate metadata channel

Layer 3: LOCATION
  "Where is Actor X right now?"
  → ActorId → EndPoint resolution
  → Local cache + membership table lookup
```

These layers are often collapsed into a single "registry," but they have
different consistency requirements:

- **Membership** needs to be **fast** (seconds, not minutes). A dead node should
  be detected quickly so actors don't queue messages into a black hole.
- **Metadata** can be **eventually consistent** (10-30 seconds). Actor types
  deployed on a node change at configuration time, not at message rate.
- **Location** needs to be **correct** (an ActorId either exists and has a known
  location, or it doesn't). Stale location cache entries must be invalidated on
  first send failure — the cache is a hint, not a guarantee.

## 3. Why Hybrid: One Size Doesn't Fit All

There is no single "best" service discovery for every deployment:

| Backend | Best for | Trade-off |
|---------|----------|-----------|
| **Gossip (SWIM)** | 3-50 nodes, zero-dependency, self-contained | O(N) background traffic, eventual consistency only |
| **etcd / Consul** | 50+ nodes, existing infra, strong consistency | External dependency, operational complexity |
| **Static routes** | Fixed topologies, firewalled networks, edge | No dynamism, manual reconfiguration |
| **Embedded registrar** | Dev, single-server, testing | Single point of failure, no failover |

A framework should not force one choice. An actor framework used for both a
developer's laptop and a 200-node production cluster needs to span this range
without changing application code.

## 4. Design Philosophy

**Zero-dependency by default, pluggable by design.** The embedded gossip
implementation is the default — it works out of the box on a laptop with no
external services. When the cluster grows beyond gossip's sweet spot, the
operator changes one line of config to point at etcd. Actor code is unchanged.

**Eventual consistency is the right default for membership.** Strongly
consistent membership (via Raft) requires a quorum for every join/leave
decision. In a 3-node cluster, losing one node means no new members can join.
Gossip-based membership has no such restriction — it degrades gracefully with
partial failure.

**The framework is honest about the trade-off.** A SWIM-based system converges
in 2-3 seconds under normal conditions but can take 15-30 seconds during a
network partition. This is acceptable for actor-to-actor messaging (messages are
buffered in mailboxes) but not for critical path decisions. The design
documents these numbers explicitly.

**Separation of discovery from transport.** Discovery populates a cache. The
transport layer uses that cache to establish connections. If the cache is stale,
the first send fails, the cache is invalidated, and a fresh resolve occurs. This
means discovery can be slow without breaking correctness.

## 5. The `IServiceDiscovery` Abstraction

The core of the hybrid approach is a single interface that every backend
implements:

```
┌──────────────────────────────────────────────────┐
│                  ActorSystem                       │
│                                                    │
│   owns IServiceDiscovery* (set at construction)    │
│                                                    │
│   ┌──────────────────────────────────────┐        │
│   │       IServiceDiscovery              │        │
│   │                                      │        │
│   │  start()                             │        │
│   │  discover_all() → vector<Member>     │        │
│   │  discover(endpoint) → Member*        │        │
│   │  announce(local_state)               │        │
│   │  on_member_change(callback)          │        │
│   └──────┬───────┬───────┬──────────────┘        │
│          │       │       │                         │
│     ┌────┴──┐ ┌──┴───┐ ┌┴──────────┐             │
│     │ Gossip│ │Static│ │UdpRegistrar│  (existing) │
│     │(new)  │ │(new) │ │(refactored)│             │
│     └───────┘ └──────┘ └───────────┘             │
└──────────────────────────────────────────────────┘

   Plug backends in the future without touching ActorSystem:
     ┌──────────┐ ┌───────────┐ ┌──────────┐
     │   etcd   │ │  Consul   │ │   K8s    │
     │ (future) │ │ (future)  │ │ (future) │
     └──────────┘ └───────────┘ └──────────┘
```

`ActorSystem::Config` gains one field:

```cpp
// Service discovery backend. If nullptr (default), uses embedded
// UdpRegistrar for backward compatibility.
std::shared_ptr<IServiceDiscovery> service_discovery;
```

This is a shared pointer so the same discovery instance can serve multiple
ActorSystems in the same process (testing, multi-tenant).

## 6. Relationship to Existing Components

### UdpRegistrar → IServiceDiscovery adapter

The existing `UdpRegistrar` already does discovery. Refactoring it to implement
`IServiceDiscovery` is a mechanical change: extract the interface, add the four
methods, delegate to existing code. This preserves backward compatibility —
existing users who don't set `service_discovery` get the same behavior.

### ActorProxy (remote send path)

```
ActorProxy::send(msg)
  1. Check local location cache: actor_id → endpoint
  2. Cache hit? → TcpTransport::send(endpoint, msg)
     Cache miss? → discovery_->discover(actor_endpoint) → cache → send
  3. Send fails (connection refused / timeout)?
     → evict cache entry
     → discovery_->discover(actor_endpoint)  // re-resolve
     → retry send
```

The cache is a `shared_ptr<ActorLocationCache>` on `ActorSystem`, keyed by
`ActorId`, backed by TTL (30s default, refresh on gossip update). It's a hint —
if it's wrong, we re-resolve.

### Link / Monitor (cross-node death detection)

When gossip marks a node Dead, the `IServiceDiscovery` fires `on_member_change`
with `joined=false`. `ActorSystem` translates this into `DownMsg` for every
actor linked to or monitoring an actor on that node. This is exactly the same
path as local actor death — the abstraction holds.

### ConnectionPool (proactive connections)

Today, connections are established lazily on first `send()`. With a membership
table, nodes can establish pools proactively:

```
on_member_change(endpoint, joined=true):
    for each acceptor in member.acceptors:
        transport_->get_or_create_pool(endpoint, acceptor)
```

This reduces first-message latency from "DNS + TCP handshake + TLS" to just
message serialization.

## 7. What Changes and What Stays the Same

| Component | Change |
|-----------|--------|
| Actor code (send, reply, link, monitor) | **Unchanged** — same API |
| ActorSystem::Config | One new field: `service_discovery` |
| UdpRegistrar | Refactored to implement `IServiceDiscovery` |
| ActorProxy | Gains location cache + re-resolve on failure |
| RegistrarServer/RegistrarClient | Unchanged (still used by UdpRegistrar backend) |
| TcpTransport / ConnectionPool | Gains proactive pool creation |
| New code | `IServiceDiscovery` interface, `GossipMembership`, `StaticDiscovery`, `ActorLocationCache` |

## 8. Naming Conventions

The term "registrar" is retained for the existing embedded server/client
protocol (UDP `ResolveQuery` + TCP `Register`/`Heartbeat`). New components use
"discovery" to describe the broader abstraction. This avoids renaming existing
code while establishing a clear boundary between the old implementation and the
new interface.
