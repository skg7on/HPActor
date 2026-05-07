# Service Discovery — Architecture Design

## 1. IServiceDiscovery Interface

The single abstraction that all discovery backends implement. Every method is
non-blocking and safe to call from any thread.

```cpp
// include/hpactor/net/service_discovery.hpp

namespace hpactor::net {

struct Member {
    EndPoint endpoint;                     // primary identity
    std::string host;                      // resolved IP
    uint16_t tcp_port = 0;                // transport port (actor messages)
    std::string uds_path;                  // UDS path if local, empty otherwise
    std::vector<AcceptorInfo> acceptors;   // TLS, protocol versions per acceptor
    std::vector<std::string> actor_types;  // e.g. ["WorkerActor", "HTTPGatewayActor"]
    MemberStatus status = MemberStatus::Alive;
    uint64_t incarnation = 0;             // monotonic, breaks ties during conflict
    std::chrono::steady_clock::time_point last_seen;
};

enum class MemberStatus : uint8_t {
    Alive,
    Suspicious,  // gossip-only: being probed indirectly
    Dead,
    Left,        // graceful departure
};

using MemberChangeCallback = std::function<void(const Member&, bool joined)>;

class IServiceDiscovery {
public:
    virtual ~IServiceDiscovery() = default;

    // Start the discovery backend. Called once by ActorSystem after construction.
    // May start background threads, register EventLoop handlers, etc.
    virtual void start() = 0;

    // Stop the backend. Blocks until all threads/handlers are cleaned up.
    virtual void stop() = 0;

    // Return all currently known members (snapshot).
    virtual std::vector<Member> discover_all() const = 0;

    // Look up a specific endpoint. Returns nullptr if unknown.
    virtual const Member* discover(EndPoint endpoint) const = 0;

    // Announce local node state. Called at startup and when local state changes
    // (e.g., new actor types spawned, load changes).
    virtual void announce(Member local_state) = 0;

    // Register a callback for member join/leave events. Must be safe to call
    // from any thread. The callback is invoked on the discovery backend's thread
    // or EventLoop context.
    virtual void on_member_change(MemberChangeCallback cb) = 0;

    // Name for logging/metrics (e.g. "gossip", "etcd", "static").
    virtual std::string backend_name() const = 0;
};

} // namespace hpactor::net
```

## 2. ActorLocationCache

A local TTL cache mapping `ActorId` → `EndPoint`. This is NOT part of
IServiceDiscovery — it's an ActorSystem-level facility that uses discovery to
populate and evict entries.

```cpp
// include/hpactor/net/actor_location_cache.hpp (new, ~80 lines)

class ActorLocationCache {
public:
    // Look up an actor's endpoint. Returns nullopt if not cached.
    std::optional<EndPoint> get(ActorId id) const;

    // Cache an actor's endpoint with TTL.
    void put(ActorId id, EndPoint ep, std::chrono::seconds ttl = std::chrono::seconds(30));

    // Evict a specific entry (called on send failure).
    void evict(ActorId id);

    // Evict all entries for a dead node (called on member_change joined=false).
    void evict_node(EndPoint ep);

    // Periodic cleanup of expired entries.
    void purge_expired();

private:
    struct CacheEntry {
        EndPoint endpoint;
        std::chrono::steady_clock::time_point expires_at;
    };
    std::unordered_map<ActorId, CacheEntry> cache_;
    mutable std::shared_mutex mutex_;  // read-heavy, write-rare
};
```

**Integration with ActorProxy::send():**
```
send(actor_addr, msg):
    endpoint = location_cache_->get(actor_addr.id)
    if !endpoint:
        member = discovery_->discover(actor_addr.endpoint)
        if !member: return error("unreachable")
        endpoint = member->endpoint
        location_cache_->put(actor_addr.id, endpoint)

    result = transport_->send(endpoint, msg)
    if result == connection_refused or timeout:
        location_cache_->evict(actor_addr.id)
        // retry once — will re-resolve via discovery
        return send(actor_addr, msg)
    return result
```

## 3. GossipMembership Design

### 3.1 SWIM Protocol Adaptation

The standard SWIM protocol has three components:

1. **Failure detection**: Each node pings a randomly selected peer every protocol
   period (default 1s). If no ack within timeout (200ms), it requests indirect
   probes via k other peers. If those also fail, the node is marked Suspicious,
   then Dead after a confirmation period.

2. **Membership dissemination**: Piggyback join/leave/suspicion events onto ping
   messages and ack messages. This gives O(log N) convergence without a separate
   gossip channel.

3. **State synchronization**: On join, a new node receives the full membership
   table from its seed contact.

HPActor's adaptation:
- Runs entirely within the existing `EventLoop` — a periodic timer replaces the
  SWIM protocol round, no dedicated thread needed.
- Reuses the existing UDP socket infrastructure (same pattern as
  `handle_udp_read_ready`).
- Piggyback format is a compact binary encoding, not protobuf (low latency,
  1500-byte MTU-friendly).

### 3.2 Configuration

```cpp
struct GossipConfig {
    // UDP port for gossip traffic (separate from registrar UDP)
    uint16_t gossip_port = 5354;

    // Protocol timing
    std::chrono::milliseconds protocol_period{1000};   // ping interval
    std::chrono::milliseconds ping_timeout{200};         // direct ack timeout
    std::chrono::milliseconds suspicion_timeout{3000};   // suspicious → dead
    std::chrono::milliseconds dead_timeout{30000};       // dead → tombstone removal

    // Fanout
    uint32_t fanout = 3;         // peers to ping per round
    uint32_t indirect_probes = 3; // indirect probes for suspicion

    // Bootstrap
    std::vector<EndPoint> seeds;  // initial contact points

    // Local node info
    Member local_state;
};
```

### 3.3 Gossip Message Format

All messages fit in a single UDP datagram (padded to MTU for piggyback space):

```
 0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5  6  7  8  9  0  1
+---------------+---------------+---------------+-----------------------------------------------+
| Magic (4B): "HPGC"                        | Version (1B)  | Type (1B)      | Flags (2B)    |
+---------------+---------------+---------------+-----------------------------------------------+
| Sender Endpoint (variable, binary-encoded)                                                      |
+-----------------------------------------------------------------------------------------------+
| Incarnation (8B, big-endian)                                                                   |
+-----------------------------------------------------------------------------------------------+
| Sequence Number (4B, big-endian)                                                               |
+-----------------------------------------------------------------------------------------------+
| Ping Target Endpoint (variable)  [present for PingReq only]                                    |
+-----------------------------------------------------------------------------------------------+
| Piggyback Count (2B)                                                                           |
+-----------------------------------------------------------------------------------------------+
| Piggyback Entry 1: [Type(1B) | Endpoint(variable) | Incarnation(8B)]                           |
| Piggyback Entry 2: ...                                                                         |
| ...                                                                                            |
+-----------------------------------------------------------------------------------------------+
```

**Message Types:**
| Type | Value | Description |
|------|-------|-------------|
| Ping | 0x01 | Direct liveness probe |
| Ack | 0x02 | Response to Ping (carries piggyback) |
| PingReq | 0x03 | Request indirect probe of target |
| IndirectAck | 0x04 | Response from indirect probe |
| Join | 0x05 | New node announcement (to seed) |
| SyncReq | 0x06 | Request full membership table |
| SyncRsp | 0x07 | Full membership table response |
| Leave | 0x08 | Graceful departure |

**Piggyback Entry Types:**
| Type | Description |
|------|-------------|
| Alive | Node claims it or another node is alive (new incarnation) |
| Suspicious | Node suspects another is dead |
| Dead | Node confirms another is dead |
| Metadata | Actor types, load, or other metadata update |

### 3.4 Protocol Round

```cpp
void GossipMembership::protocol_round() {
    // 1. Pick fanout random peers from alive members
    auto targets = pick_random_peers(config_.fanout);

    // 2. Send Ping to each
    for (auto& target : targets) {
        send_ping(target);
        pending_pings_[target].expires_at = now() + ping_timeout;
    }

    // 3. Check for expired pings
    for (auto& [target, pending] : pending_pings_) {
        if (now() > pending.expires_at && !pending.indirect_requested) {
            // Direct ping failed — request indirect probes
            auto proxies = pick_random_peers(config_.indirect_probes, exclude={target});
            for (auto& proxy : proxies) {
                send_ping_req(proxy, target);
            }
            pending.indirect_requested = true;
            pending.indirect_expires_at = now() + ping_timeout;
        }
        if (now() > pending.indirect_expires_at && pending.indirect_requested) {
            // Indirect probes also failed — mark suspicious
            mark_suspicious(target);
            pending_pings_.erase(target);
        }
    }

    // 4. Expire old suspicious entries → dead
    for (auto& [ep, member] : members_) {
        if (member.status == Suspicious &&
            now() - member.last_seen > config_.suspicion_timeout) {
            mark_dead(ep);
        }
    }

    // 5. Purge dead tombstones
    purge_dead_tombstones();
}
```

### 3.5 State Machine

```
                    ┌─────────┐
                    │  None   │  (never seen this endpoint)
                    └────┬────┘
                         │ Ping arrives or piggyback Alive with higher incarnation
                         ▼
                    ┌─────────┐
              ┌─────│  Alive  │◄────┐
              │     └────┬────┘     │
              │          │          │ higher incarnation from same endpoint
              │  direct  │          │ (resolves split-brain: lower incarnation yields)
              │  ping    │          │
              │  fails   │          │
              │          ▼          │
              │     ┌───────────┐   │
              │     │ Suspicious │───┘ (timeout with no confirmation)
              │     └─────┬─────┘
              │           │ k nodes confirm + suspicion_timeout elapsed
              │           ▼
              │     ┌─────────┐
              │     │  Dead   │
              │     └────┬────┘
              │          │ dead_timeout elapsed
              │          ▼
              │     (tombstone removed)
              │
              └─── node sends Leave message (graceful)
                    ▼
              ┌─────────┐
              │  Left   │
              └─────────┘
```

**Incarnation numbers** prevent split-brain: a node restarts with a higher
incarnation (monotonic, persisted or clock-derived). When other nodes see a
message with a higher incarnation, they accept the new state. When they see a
message with a lower incarnation, they reject it. This means a restarted node
reinforces its Alive status even if stale Suspicious markers exist for its old
incarnation.

### 3.6 Thread Safety

`GossipMembership` uses the EventLoop timer for protocol rounds, `handle_udp_read_ready`
for inbound messages (EventLoop read handler), and shared-mutex protection for the
membership table (read-heavy via `discover_all`/`discover`, write-rare via protocol
rounds). Member change callbacks are invoked from the EventLoop thread.

## 4. StaticDiscovery

Trivial implementation for fixed topologies. Configured via TOML or code.

```cpp
class StaticDiscovery : public IServiceDiscovery {
    // discover_all() → returns pre-configured vector
    // discover() → linear scan, O(N) (fine for N < 10)
    // announce() → no-op
    // start()/stop() → no-op
    // on_member_change() → never fires (topology is static)
};
```

## 5. UdpRegistrar Refactored as IServiceDiscovery

The existing `UdpRegistrar` already provides `discover`, `get_all_endpoints`,
and a `node_callback`. The refactoring is mechanical:

```cpp
class UdpRegistrar : public IServiceDiscovery {
    // discover_all()  → delegates to get_all_endpoints() with Member conversion
    // discover()      → delegates to get_endpoint()
    // announce()      → no-op (registrar server handles membership)
    // on_member_change() → delegates to existing node_callback_ wiring
    // backend_name()  → returns "udp-registrar"
    // start()/stop()  → existing
};
```

This preserves backward compatibility — existing users who construct
`ActorSystem` without setting `service_discovery` get the UdpRegistrar backend
by default.

## 6. ActorSystem Integration

```cpp
// include/hpactor/core/actor_system.hpp — Config changes

struct Config {
    // ... existing fields ...

    // Service discovery backend. If nullptr (default), creates UdpRegistrar
    // internally for backward compatibility. Set to GossipMembership,
    // StaticDiscovery, or a custom IServiceDiscovery.
    std::shared_ptr<net::IServiceDiscovery> service_discovery;

    // Gossip configuration (only used when service_discovery is GossipMembership
    // or nullptr with enable_network + udp_port)
    net::GossipConfig gossip;
};
```

**Constructor logic:**

```
ActorSystem::ActorSystem(config):
    if config.service_discovery:
        discovery_ = config.service_discovery
    elif config.enable_network and config.registrar.udp_port > 0:
        discovery_ = make_shared<UdpRegistrar>(config.registrar, endpoint_, loop)
    else:
        discovery_ = make_shared<StaticDiscovery>(/* empty */)

    discovery_->start()
    discovery_->on_member_change(on_member_change)  // wire death detection

    location_cache_ = make_shared<ActorLocationCache>()
```

## 7. Configuration (TOML)

```toml
[system]
scheduler_threads = 4

# ── Service discovery ──────────────────────────────────────────
[system.discovery]
# Backend: "gossip", "static", or "none" (defaults to embedded registrar
# if enable_network = true and udp_port is set).
backend = "gossip"

# ── Gossip backend ─────────────────────────────────────────────
[system.discovery.gossip]
gossip_port = 5354
protocol_period_ms = 1000
ping_timeout_ms = 200
suspicion_timeout_ms = 3000
dead_timeout_ms = 30000
fanout = 3
indirect_probes = 3

# Seed nodes for bootstrap. At least one must be reachable.
seeds = [
    "10.0.1.1:5354",
    "10.0.1.2:5354",
]

# ── Static backend ─────────────────────────────────────────────
# [[system.discovery.static_nodes]]
# endpoint = "10.0.1.1:8080"
# host = "10.0.1.1"
# tcp_port = 8080
# actor_types = ["WorkerActor"]
```

## 8. Failure Scenarios and Recovery

### Scenario 1: Single node crash (server B dies)

```
Timeline (gossip, 3-node cluster, fanout=3):
  t=0.0s   B crashes (SIGKILL)
  t=1.0s   A pings B → no ack → requests indirect probes via C
  t=1.0s   C pings B (same round) → no ack → requests indirect probes via A
  t=1.2s   A and C receive each other's PingReq for B
           A probes B on behalf of C, C probes B on behalf of A
  t=1.4s   Both indirect probes fail
  t=1.4s   A marks B Suspicious, piggybacks on next Ping to C
           C marks B Suspicious, piggybacks on next Ack to A
  t=1.4s   Both nodes now agree B is Suspicious
  t=4.4s   suspicion_timeout (3s) elapsed since Suspicious
           A and C independently mark B Dead
           on_member_change(B, joined=false) fires
           → ActorSystem sends DownMsg for all actors linked to B's actors
           → ActorLocationCache evicts all B entries
           → ConnectionPool closes connections to B
```

Detection time: **~4.4 seconds** (vs. 15s expiration_timeout in current registrar).

### Scenario 2: Network partition (split-brain)

```
[ A ] --x-- [ B ]    (link between A and B fails, C can see both)
   \         /
    [ C ]

  A pings B → fails → requests indirect via C
  C probes B → succeeds → B is alive
  C piggybacks B's Alive status to A
  A accepts (B's incarnation is current) → B remains Alive
```

No split-brain: indirect probes via C confirm B is alive. The partition is
invisible to membership.

### Scenario 3: Full partition — cluster splits into two halves

```
[ A  B ] --x-- [ C  D ]

  A and B can only see each other. C and D can only see each other.
  Since gossip doesn't require quorum, both halves continue operating.
  Each half converges to {A, B} and {C, D} respectively.

  When the partition heals:
  - Next protocol round: A pings C → succeeds → A sees C is alive with
    higher incarnation → A merges C's membership table → A now sees {C, D}
  - Same from C's perspective → C now sees {A, B}
  - Both halves re-converge to {A, B, C, D}

  Actors that had links across the partition receive DownMsg when their
  halves mark the other as Dead. When the partition heals, they can
  re-establish links (application-level retry).
```

### Scenario 4: Seed node failure at startup

```
  New node E starts with seeds = [A, B].
  E tries A → connection refused (A is down)
  E tries B → succeeds → B sends full membership table
  E bootstraps with the full cluster view
```

## 9. Migration Path

The existing `UdpRegistrar` path is preserved. Migration from embedded registrar
to gossip is a TOML config change — no code changes:

```toml
# Before (embedded registrar, current default):
[system]
enable_network = true
udp_port = 5353

# After (gossip):
[system]
enable_network = true

[system.discovery]
backend = "gossip"

[system.discovery.gossip]
seeds = ["10.0.1.1:5354", "10.0.1.2:5354"]
```

Actors continue to use `context()->send(addr, msg)` unchanged. The location
cache, ActorProxy re-resolution, and ConnectionPool proactive connections all
work transparently regardless of which `IServiceDiscovery` backend is active.

## 10. New Files

| File | Purpose |
|------|---------|
| `include/hpactor/net/service_discovery.hpp` | `IServiceDiscovery`, `Member`, `MemberStatus` |
| `include/hpactor/net/gossip_membership.hpp` | `GossipConfig`, `GossipMembership` |
| `include/hpactor/net/static_discovery.hpp` | `StaticDiscovery` |
| `include/hpactor/net/actor_location_cache.hpp` | `ActorLocationCache` |
| `src/net/gossip_membership.cpp` | SWIM protocol implementation |
| `src/net/static_discovery.cpp` | Trivial static implementation |
| `src/net/actor_location_cache.cpp` | Cache implementation |
| `tests/net/test_gossip_membership.cpp` | SWIM protocol tests |
| `tests/net/test_service_discovery.cpp` | Interface + integration tests |

## 11. Existing Files Modified

| File | Change |
|------|--------|
| `include/hpactor/core/actor_system.hpp` | Add `service_discovery` and `gossip` to Config; add `location_cache_` member |
| `src/actor/actor_system.cpp` | Create/start discovery backend; wire `on_member_change` to death detection |
| `include/hpactor/net/registrar.hpp` | `UdpRegistrar` inherits `IServiceDiscovery`; add interface method stubs |
| `src/net/registrar.cpp` | Implement `IServiceDiscovery` methods on `UdpRegistrar` |
| `src/ref/actor_proxy.cpp` | Integrate `ActorLocationCache` in `send()` |
| `src/net/connection_pool.cpp` | Proactive pool creation on `on_member_change(joined=true)` |
| `docs/architecture/net/service-discovery-core-concept.md` | This document's companion |
