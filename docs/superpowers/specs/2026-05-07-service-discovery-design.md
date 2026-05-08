# Service Discovery — Detailed Design Specification

## 1. Overview

Replace HPActor's single-mode embedded `UdpRegistrar` with a pluggable service
discovery system supporting three deployment scenarios through a common
`IServiceDiscovery` interface:

| Scenario | Backend | Same-host discovery | Cross-host discovery |
|----------|---------|---------------------|----------------------|
| Single server, multi-process | UdpRegistrar (default) | TCP/UDP registrar protocol | — |
| Multi-server, one process each | GossipMembership | — | SWIM gossip |
| Multi-server, multi-process | HybridDiscovery (UdpRegistrar + Gossip) | TCP/UDP registrar | SWIM gossip |

Plus `StaticDiscovery` for fixed topologies and `ActorLocationCache` for
`ActorId` → `EndPoint` resolution caching.

## 2. IServiceDiscovery Interface

**File:** `include/hpactor/net/service_discovery.hpp` (new, ~60 lines)

```cpp
namespace hpactor::net {

struct Member {
    EndPoint endpoint;
    std::string host;
    uint16_t tcp_port = 0;
    std::string uds_path;
    std::vector<AcceptorInfo> acceptors;
    std::vector<std::string> actor_types;
    MemberStatus status = MemberStatus::Alive;
    uint64_t incarnation = 0;
    std::chrono::steady_clock::time_point last_seen;
};

enum class MemberStatus : uint8_t { Alive, Suspicious, Dead, Left };

using MemberChangeCallback = std::function<void(const Member&, bool joined)>;

class IServiceDiscovery {
public:
    virtual ~IServiceDiscovery() = default;

    virtual void start() = 0;
    virtual void stop() = 0;  // blocks until cleanup completes

    virtual std::vector<Member> discover_all() const = 0;  // snapshot copy
    virtual const Member* discover(EndPoint) const = 0;    // ptr into internal table
    virtual void announce(Member local_state) = 0;          // idempotent
    virtual void on_member_change(MemberChangeCallback) = 0; // replaces previous
    virtual std::string backend_name() const = 0;

    // HybridDiscovery support — returns raw member map for composition.
    // Returns nullptr if backend does not expose members directly.
    virtual const std::unordered_map<EndPoint, Member>* raw_members() const {
        return nullptr;
    }
};

} // namespace hpactor::net
```

**Method semantics:**

- `discover_all()` — snapshot copy. O(N). Safe to call from any thread. The
  vector is independently owned by the caller.
- `discover()` — pointer into the backend's internal map. O(1) or O(log N).
  Valid until the next mutation (`announce`, member join/leave, protocol round).
  Must not be held across await points. For `StaticDiscovery`, valid for the
  process lifetime.
- `announce()` — updates local node metadata. Idempotent. Calling with
  unchanged data is a no-op. Calling with changed `actor_types` or `acceptors`
  triggers re-dissemination.
- `on_member_change()` — registers ONE callback. Replaces any previous. The
  callback is invoked on the backend's EventLoop thread. Callback must not call
  back into the discovery backend (documented constraint).
- `start()` — called once by ActorSystem constructor after the backend is
  created. May start timers, open sockets, initiate connections.
- `stop()` — called once by ActorSystem destructor. Blocks until all threads,
  timers, and socket handlers are cleaned up.

## 3. UdpRegistrar Refactored as IServiceDiscovery

**Files:** `include/hpactor/net/registrar.hpp`, `src/net/registrar.cpp`

In-place refactoring. Add `IServiceDiscovery` as a base class. Existing public
API unchanged. Constructor unchanged.

```cpp
class UdpRegistrar : public IServiceDiscovery {  // base added
public:
    // Existing API unchanged: start(), stop(), get_endpoint(),
    // get_all_endpoints(), set_node_callback(), handle_udp_packet(), failover()

    // IServiceDiscovery overrides
    std::vector<Member> discover_all() const override;
    const Member* discover(EndPoint) const override;
    void announce(Member) override;
    void on_member_change(MemberChangeCallback) override;
    std::string backend_name() const override { return "udp-registrar"; }

    const std::unordered_map<EndPoint, Member>* raw_members() const override {
        return &endpoint_to_member_;
    }

private:
    // Converts NodeEndpoint → Member (free function in registrar.cpp)
    static Member to_member(const NodeEndpoint& ep);

    // Adapter map: kept in sync with the server's NodeRegistry or
    // client's client_registry_ on each join/leave event.
    std::unordered_map<EndPoint, Member> endpoint_to_member_;
};
```

**Method implementations** (~40 lines in `registrar.cpp`):

- `discover_all()` — calls `get_all_endpoints()`, converts each `NodeEndpoint`
  via `to_member()`, returns the vector.
- `discover()` — looks up `endpoint_to_member_`. Returns `&it->second` or
  `nullptr`.
- `announce()` — no-op. The registrar server already handles membership
  announcements via `Register`/`Heartbeat` messages from clients.
- `on_member_change(cb)` — wraps the existing `node_callback_` wiring. When the
  server's `node_callback_` fires `(Endpoint, bool online)`, we call
  `cb(member, online)`.

**Member map maintenance:** The `endpoint_to_member_` map is updated in the
existing registrar callback paths:
- Server mode: in `RegistrarServer::handle_tcp_message(Register)` → upsert;
  in `handle_disconnect()` → remove.
- Client mode: in `RegistrarClient::handle_server_message(NodeJoin)` → upsert;
  in `handle_server_message(NodeLeave)` → remove.

## 4. StaticDiscovery

**File:** `include/hpactor/net/static_discovery.hpp` (new, ~40 lines)
**Source:** `src/net/static_discovery.cpp` (new, ~20 lines)

```cpp
class StaticDiscovery : public IServiceDiscovery {
public:
    explicit StaticDiscovery(std::vector<Member> members);
    void start() override {}
    void stop() override {}
    std::vector<Member> discover_all() const override;
    const Member* discover(EndPoint) const override;
    void announce(Member) override {}
    void on_member_change(MemberChangeCallback) override {}
    std::string backend_name() const override { return "static"; }

private:
    std::vector<Member> members_;
    std::unordered_map<EndPoint, size_t> index_;  // endpoint → vector index
};
```

Thread-safe by immutability. No locks needed. `discover()` O(1) via index
lookup. `discover_all()` returns a copy.

## 5. ActorLocationCache

**File:** `include/hpactor/net/actor_location_cache.hpp` (new, ~50 lines)
**Source:** `src/net/actor_location_cache.cpp` (new, ~40 lines)

```cpp
class ActorLocationCache {
public:
    std::optional<EndPoint> get(ActorId id) const;
    void put(ActorId id, EndPoint ep, std::chrono::seconds ttl = 30s);
    void evict(ActorId id);
    void evict_node(EndPoint ep);
    void purge_expired();

private:
    struct Entry {
        EndPoint endpoint;
        std::chrono::steady_clock::time_point expires_at;
    };
    std::unordered_map<ActorId, Entry> cache_;
    mutable std::shared_mutex mutex_;
};
```

- `get()` — shared lock. Returns `nullopt` if not cached or expired. Purges
  expired entries on read (amortized cleanup).
- `put()` — exclusive lock. Overwrites existing entry.
- `evict()` — exclusive lock. Removes specific entry.
- `evict_node()` — exclusive lock. O(N) scan and remove all entries where
  `entry.endpoint == ep`. N bounded by cache size (typically dozens, not
  thousands).
- `purge_expired()` — exclusive lock. Removes all expired entries. Called
  on every `discover_all()` (amortized cleanup). For periodic cleanup,
  `ActorSystem` creates a 60s EventLoop timer that calls `purge_expired()`.
  The timer handle is stored on `ActorSystem` and cancelled in `~ActorSystem()`.
  `ActorLocationCache` itself has no timer — it is a passive data structure.

**How `discovery_` and `location_cache_` reach `ActorProxy`:**

`ActorProxy` currently holds `address_` (ActorAddress) and `transport_`
(TcpTransport*). Two new pointer members are added:

```cpp
// actor_proxy.hpp — changes
class ActorProxy {
    // ... existing members ...
    IServiceDiscovery* discovery_ = nullptr;   // set by ActorSystem on construction
    ActorLocationCache* location_cache_ = nullptr;
};
```

These are set by `ActorSystem` when it creates the proxy. ActorProxy does not
own them — it holds raw pointers to ActorSystem-owned objects. This avoids
ownership cycles: ActorSystem owns discovery_ and location_cache_ (shared_ptr),
ActorProxy references them (raw ptr, valid for process lifetime).

Alternatively (lower-overhead): store the pointers on `TcpTransport` and pass
them in `send()` as additional arguments to avoid touching ActorProxy's
constructor signature. The planner should pick the approach that minimizes
diff to the existing `actor_proxy.hpp` header.

**Integration with `ActorProxy::send()`** (`src/ref/actor_proxy.cpp`, ~15 lines):

```
send(actor_addr, msg):
    // 1. Resolve
    endpoint = location_cache_->get(actor_addr.id)
    if !endpoint:
        member = discovery_->discover(actor_addr.endpoint)
        if !member: return error("unreachable")
        endpoint = member->endpoint
        location_cache_->put(actor_addr.id, endpoint)

    // 2. Send
    result = transport_->send(endpoint, msg)

    // 3. Retry on failure
    if result == connection_refused or result == timeout:
        location_cache_->evict(actor_addr.id)
        member = discovery_->discover(actor_addr.endpoint)  // re-resolve
        if !member: return error("unreachable")
        return transport_->send(member->endpoint, msg)

    return result
```

## 6. GossipMembership

**Files:**
- `include/hpactor/net/gossip_membership.hpp` (new, ~100 lines)
- `src/net/gossip_membership.cpp` (new, ~500 lines)

### 6.1 Configuration

```cpp
struct GossipConfig {
    uint16_t gossip_port = 5354;
    std::chrono::milliseconds protocol_period{1000};
    std::chrono::milliseconds ping_timeout{200};
    std::chrono::milliseconds suspicion_timeout{3000};
    std::chrono::milliseconds dead_timeout{30000};
    uint32_t fanout = 3;
    uint32_t indirect_probes = 3;
    std::vector<EndPoint> seeds;
    Member local_state;
};
```

### 6.2 Class Layout

```cpp
class GossipMembership : public IServiceDiscovery {
public:
    GossipMembership(const GossipConfig& cfg, EventLoop* loop);
    ~GossipMembership() override;

    void start() override;
    void stop() override;
    std::vector<Member> discover_all() const override;
    const Member* discover(EndPoint) const override;
    void announce(Member) override;
    void on_member_change(MemberChangeCallback) override;
    std::string backend_name() const override { return "gossip"; }

    const std::unordered_map<EndPoint, Member>* raw_members() const override {
        return &members_;
    }

private:
    // ── SWIM protocol ──────────────────────────────────────
    void protocol_round();
    void handle_packet(const StreamBuffer& data, const std::string& from_host,
                       uint16_t from_port);

    // Message handlers (dispatched from handle_packet)
    void handle_ping(EndPoint sender, uint64_t incarnation, uint32_t seq_no,
                     std::vector<PiggybackEntry> piggyback,
                     const std::string& from_host, uint16_t from_port);
    void handle_ack(EndPoint sender, uint64_t incarnation,
                    std::vector<PiggybackEntry> piggyback);
    void handle_ping_req(EndPoint sender, EndPoint target);
    void handle_indirect_ack(EndPoint sender, EndPoint target);
    void handle_join(EndPoint sender, uint64_t incarnation,
                     std::vector<PiggybackEntry> piggyback);
    void handle_sync_rsp(std::vector<Member> members);
    void handle_leave(EndPoint sender, uint64_t incarnation);

    // ── Message sending ────────────────────────────────────
    void send_ping(EndPoint target);
    void send_ack(EndPoint target, std::vector<PiggybackEntry> piggyback);
    void send_ping_req(EndPoint proxy, EndPoint target);
    void send_indirect_ack(EndPoint target, EndPoint original_target);
    void send_join(EndPoint seed);
    void send_sync_rsp(EndPoint target);
    void send_leave(EndPoint target);

    // ── State mutations ────────────────────────────────────
    void mark_suspicious(EndPoint ep);
    void mark_dead(EndPoint ep);
    void merge_member(const Member& remote);
    void apply_piggyback(const std::vector<PiggybackEntry>& entries);
    void purge_dead_tombstones();
    std::vector<EndPoint> pick_random_peers(
        size_t count, std::unordered_set<EndPoint> exclude = {});

    // ── UDP socket ─────────────────────────────────────────
    void setup_udp_socket();
    void teardown_udp_socket();

    // ── Wire protocol ──────────────────────────────────────
    StreamBuffer encode_message(MessageType type, uint64_t incarnation,
        uint32_t seq_no, EndPoint ping_target,  // PingReq only
        const std::vector<PiggybackEntry>& piggyback) const;
    bool decode_message(const StreamBuffer& data,
        MessageType& type, EndPoint& sender, uint64_t& incarnation,
        uint32_t& seq_no, EndPoint& ping_target,
        std::vector<PiggybackEntry>& piggyback) const;
    StreamBuffer encode_sync_rsp(const std::vector<Member>& members) const;
    bool decode_sync_rsp(const StreamBuffer& data,
        std::vector<Member>& members) const;

    // ── Members ────────────────────────────────────────────
    GossipConfig config_;
    EventLoop* loop_ = nullptr;
    int udp_socket_ = -1;
    uint64_t incarnation_ = 0;
    uint32_t seq_no_ = 0;

    std::unordered_map<EndPoint, Member> members_;
    std::unordered_map<EndPoint, PendingPing> pending_pings_;
    MemberChangeCallback member_change_cb_;
    uint64_t protocol_timer_ = 0;
    std::vector<uint8_t> recv_buffer_;  // 64KB

    mutable std::shared_mutex members_mutex_;
};

struct PendingPing {
    std::chrono::steady_clock::time_point expires_at;
    bool indirect_requested = false;
    std::chrono::steady_clock::time_point indirect_expires_at;
    // Note: indirect_expires_at = expires_at + ping_timeout (same timeout).
    // Indirect probes reuse ping_timeout — there is no separate config field.
};
```

### 6.3 Wire Format

All messages fit in a single UDP datagram (max 1400 bytes to stay within
Ethernet MTU after IP/UDP headers).

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+---------------+---------------+---------------+---------------+
| Magic (4B): "HPGC"                                            |
+---------------+---------------+---------------+---------------+
| Version (1B)  | Type (1B)      | Flags (2B)                    |
+---------------+---------------+---------------+---------------+
| Sender Endpoint length (1B) | Sender Endpoint (variable)      |
+---------------+---------------+---------------+---------------+
| Incarnation (8B, big-endian)                                   |
+---------------+---------------+---------------+---------------+
| Sequence Number (4B, big-endian)                               |
+---------------+---------------+---------------+---------------+
| Ping Target Endpoint (variable) [PingReq only]                 |
+---------------+---------------+---------------+---------------+
| Piggyback Count (2B, big-endian)                               |
+---------------+---------------+---------------+---------------+
| Piggyback Entry [0..N]:                                        |
|   Type (1B) | Flags (1B) | Endpoint (variable)                 |
|   Incarnation (8B) | Metadata Length (2B) | Metadata (...)     |
+---------------+---------------+---------------+---------------+
```

**Endpoint encoding** reuses `endpoint_ops` binary format from
`include/hpactor/ref/actor_address.hpp`: 0x04 prefix + 7 bytes for IPv4,
0x06 prefix + 19 bytes for IPv6.

**Message types:**
| Type | Value | Description |
|------|-------|-------------|
| Ping | 0x01 | Direct liveness probe |
| Ack | 0x02 | Response to Ping |
| PingReq | 0x03 | Request indirect probe |
| IndirectAck | 0x04 | Indirect probe response |
| Join | 0x05 | Bootstrap: request membership table from seed |
| SyncRsp | 0x06 | Response to Join — full membership table |
| Leave | 0x07 | Graceful departure |

**Piggyback entry types:**
| Type | Value | Description |
|------|-------|-------------|
| Alive | 0x01 | Node is alive (new incarnation) |
| Suspicious | 0x02 | Node is suspected dead |
| Dead | 0x03 | Node is confirmed dead |
| Metadata | 0x04 | Actor types, load, acceptors |

### 6.4 Bootstrap Sequence (`start()`)

1. Initialize `incarnation_`: compute `Clock::now().time_since_epoch().count()`
   using `std::chrono::system_clock` (wall-clock nanoseconds). On restart, time
   has advanced, so the new incarnation is higher than any previous incarnation
   from this node. Acceptable risk: NTP adjustments can jump backwards by small
   amounts (typically milliseconds). If incarnation regresses, the node loses
   conflict resolution for one protocol round — peers reject its messages as
   stale, mark it Suspicious, then the node self-corrects by issuing a new
   `announce()` with `incarnation_ = max(previous, previously_seen + 1)`. This
   is a documented edge case in section 10.
2. `setup_udp_socket()` — create, `SO_REUSEADDR`, bind to `config_.gossip_port`,
   `add_fd` + `set_read_handler` on EventLoop.
3. Add self to `members_` with status `Alive`.
4. If `config_.seeds` is non-empty: send `Join` to first seed. Seed responds
   with `SyncRsp` containing its full membership table. Call `merge_member()`
   for each. If no response within 1s, try next seed.
5. If `config_.seeds` is empty: start as solo cluster. `members_` contains only
   self.
6. Schedule `protocol_timer_` via `loop_->run_every(protocol_round,
   config_.protocol_period)`.

### 6.5 Protocol Round (`protocol_round()`)

1. Pick `config_.fanout` random alive peers (excluding self) via
   `pick_random_peers()`.
2. Send `Ping` to each. Record `PendingPing` with `expires_at`.
3. Check all pending pings:
   - Expired + no indirect requested → pick `config_.indirect_probes` other
     peers, send `PingReq(target)` to each. Set `indirect_requested = true`.
   - Expired + indirect was requested → `mark_suspicious(target)`.
4. Check all suspicious members: if `now - last_seen > suspicion_timeout`,
   `mark_dead(target)`. No confirmation counter is needed — the
   suspicion_timeout alone gates the transition (standard SWIM design: other
   nodes independently confirm via their own ping failures).
5. `purge_dead_tombstones()` — remove dead entries where
   `now - last_seen > dead_timeout`.

### 6.6 Member State Machine

```
     ┌─────────┐
     │  None   │  (never seen this endpoint)
     └────┬────┘
          │ Join/Ping/Ack with any incarnation
          │ (first contact — accept unconditionally)
          ▼
     ┌─────────┐
┌────│  Alive  │◄──────────────────┐
│    └────┬────┘                   │
│         │                        │ higher-incarnation message
│   direct│                        │ arrives from this endpoint
│   ping  │                        │ (Alive, Ack, or piggyback Alive entry)
│   fails │                        │
│         ▼                        │
│    ┌───────────┐                 │
│    │ Suspicious │────────────────┘
│    └─────┬─────┘
│          │ suspicion_timeout elapsed
│          ▼
│    ┌─────────┐
│    │  Dead   │
│    └────┬────┘
│         │ dead_timeout elapsed → tombstone removed
│
└── Leave message (graceful)
     ▼
┌─────────┐
│  Left   │
└─────────┘
```

Transitions:
- **None → Alive**: First contact — accept any incarnation.
- **Alive → Suspicious**: Direct ping fails (no Ack within `ping_timeout`) AND
  indirect probes via `indirect_probes` peers also fail.
- **Suspicious → Alive**: A higher-incarnation message arrives from this
  endpoint. The node recovered or the suspicion was wrong.
- **Suspicious → Dead**: `suspicion_timeout` elapsed without a
  higher-incarnation message. No confirmation counter needed (standard SWIM).
- **Alive → Alive**: Higher incarnation message — accept new state.
- **Alive → Left**: Graceful `Leave` message received.
- **Dead → tombstone**: Purged after `dead_timeout`.

Incarnation numbers resolve conflicts: when a restarted node announces with a
higher incarnation, peers accept the new Alive state even if stale Suspicious
markers exist for the old incarnation. If NTP causes a brief incarnation
regression (rare), the node self-corrects on the next protocol round.```

### 6.7 Thread Safety

- `protocol_round()` fires on the EventLoop thread via timer.
- UDP read handler fires on the EventLoop thread.
- These are **serialized** — EventLoop runs timers and fd callbacks on a single
  thread. No concurrent mutation of `members_` or `pending_pings_`.
- `discover()` / `discover_all()` / `raw_members()` can be called from any
  thread (actor threads, main thread). Protected by `shared_mutex` — shared
  lock for reads.
- `announce()` can be called from any thread. Acquires exclusive lock on
  `members_mutex_`, updates self entry.
- `on_member_change()` callback is invoked under the exclusive lock. Callback
  must not call back into `GossipMembership`.

### 6.8 Graceful Shutdown (`stop()`)

1. Cancel `protocol_timer_` via `loop_->cancel_timer()`.
2. Send `Leave` to all alive members (fire-and-forget, don't wait for acks).
3. `teardown_udp_socket()` — `clear_read_handler`, `remove_fd`, close.
4. Clear `members_`.

## 7. HybridDiscovery

**Files:**
- `include/hpactor/net/hybrid_discovery.hpp` (new, ~50 lines)
- `src/net/hybrid_discovery.cpp` (new, ~80 lines)

```cpp
class HybridDiscovery : public IServiceDiscovery {
public:
    HybridDiscovery(const RegistrarConfig& reg_cfg,
                    const GossipConfig& gossip_cfg,
                    EndPoint local_ep, EventLoop* loop);
    ~HybridDiscovery() override;

    void start() override;
    void stop() override;
    std::vector<Member> discover_all() const override;
    const Member* discover(EndPoint) const override;
    void announce(Member) override;
    void on_member_change(MemberChangeCallback) override;
    std::string backend_name() const override { return "hybrid"; }

private:
    void on_local_member_change(const Member& m, bool joined);
    static std::vector<Member> merge_lists(
        const std::vector<Member>& local,
        const std::vector<Member>& remote);

    UdpRegistrar registrar_;
    GossipMembership gossip_;
    EndPoint local_ep_;
    MemberChangeCallback user_callback_;
};
```

**Method semantics:**

- `discover(ep)` — checks `registrar_.discover(ep)` first (same-host). Falls
  back to `gossip_.discover(ep)` (cross-host). Returns nullptr if neither knows
  the endpoint.
- `discover_all()` — merges `registrar_.discover_all()` +
  `gossip_.discover_all()`. Same-host entries take precedence on EndPoint
  collision (same EndPoint in both).
- `on_member_change(cb)` — stores `user_callback_`. Forwards
  `gossip_.on_member_change(cb)` for cross-host events. Wires
  `registrar_.on_member_change()` to `on_local_member_change`.
- `on_local_member_change(m, joined)` — calls `gossip_.announce(m)` to
  propagate local changes into the gossip layer immediately, then fires
  `user_callback_(m, joined)`.
- `announce(m)` — if `m.endpoint` is local (loopback or same `EndPoint` as
  `local_ep_`), delegates to `registrar_.announce()`. Always calls
  `gossip_.announce(m)` for cross-host propagation.
- `start()` — starts `registrar_` first (binds TCP/UDP, determines
  server/client), then `gossip_`. Order matters: the registrar may enter client
  mode (someone else on this host is the server), but gossip always starts.
- `stop()` — stops `gossip_` first (graceful Leave to peers), then `registrar_`.

## 8. ActorSystem Integration

**File:** `include/hpactor/core/actor_system.hpp` (~15 lines changed)
**File:** `src/actor/actor_system.cpp` (~40 lines changed)

### 8.1 Config Changes

```cpp
struct Config {
    // ... existing fields unchanged ...

    // Service discovery backend. If nullptr (default), creates UdpRegistrar
    // internally for backward compatibility.
    std::shared_ptr<net::IServiceDiscovery> service_discovery;

    // Gossip configuration (used when creating GossipMembership internally,
    // or when service_discovery is nullptr and enable_network=true).
    net::GossipConfig gossip;
};
```

### 8.2 Constructor Logic

```
ActorSystem::ActorSystem(config):
    // ... existing initialization ...
    if config.enable_network:
        // ... existing EventLoop, TcpTransport setup ...

        // ── Service discovery ────────────────────────────
        if config.service_discovery:
            discovery_ = config.service_discovery
        elif config.registrar.udp_port > 0:
            discovery_ = make_shared<UdpRegistrar>(
                config.registrar, endpoint_, network_loop_.get())
        else:
            discovery_ = make_shared<StaticDiscovery>({})

        discovery_->start()
        discovery_->on_member_change([this](const Member& m, bool joined) {
            if !joined:
                on_node_dead(m.endpoint)
            else if config_.enable_network:
                transport_->prewarm_pool(m.endpoint, m.acceptors)
        })

    // ... rest of existing initialization ...
    location_cache_ = make_shared<ActorLocationCache>()
```

### 8.3 New Members

```cpp
std::shared_ptr<net::IServiceDiscovery> discovery_;
std::shared_ptr<net::ActorLocationCache> location_cache_;
```

### 8.4 `on_node_dead(EndPoint)`

Called from `on_member_change(joined=false)`. Iterates all actors via
`for_each_actor()`. For each actor with links or monitors to actors on the dead
endpoint, sends `DownMsg`. Evicts the dead endpoint from `location_cache_`.

## 9. TOML Configuration

**File:** `src/config/toml_parser.cpp` (~60 lines added)

```toml
[system]
enable_network = true
scheduler_threads = 4

# ── Service discovery ──────────────────────────────────────────
[system.discovery]
backend = "gossip"     # "gossip" | "static" | "hybrid"
                       # omitted → embedded UdpRegistrar (backward compat)

[system.discovery.gossip]
gossip_port = 5354
protocol_period_ms = 1000
ping_timeout_ms = 200
suspicion_timeout_ms = 3000
dead_timeout_ms = 30000
fanout = 3
indirect_probes = 3
seeds = ["10.0.1.1:5354", "10.0.1.2:5354"]

[system.discovery.static]
members = [
    { endpoint = "10.0.1.1:8080", host = "10.0.1.1", tcp_port = 8080,
      actor_types = ["WorkerActor"] },
]

[system.registrar]
tcp_port = 5353
udp_port = 5353
```

**Parsing logic:**
1. Read `[system.discovery].backend` string.
2. `"gossip"` → parse `[system.discovery.gossip]` sub-table → construct
   `GossipConfig`.
3. `"static"` → parse `[[system.discovery.static.members]]` array → construct
   `vector<Member>`.
4. `"hybrid"` → parse both `[system.registrar]` (for UdpRegistrar) and
   `[system.discovery.gossip]` (for GossipMembership).
5. Any unrecognized backend string → parse error.
6. If `backend` key is absent → don't set `service_discovery` in Config
   (defaults to UdpRegistrar via constructor logic).

## 10. Edge Cases

| Case | Behavior |
|------|----------|
| Gossip with zero seeds | Solo cluster. `members_` = {self}. Protocol round no-ops (no peers). When another node with this node as seed joins, cluster grows. |
| UDP port conflict | `setup_udp_socket()` bind fails → `udp_socket_ = -1` → log warning → protocol round no-ops. Node operates solo. |
| Incarnation regression (NTP) | `system_clock` can jump backwards. If incarnation regresses, peers reject this node's messages for one protocol round (stale incarnation). Node detects the rejections, self-corrects with `announce(incarnation = max(previous, peers_seen + 1))`. Recovers within 2 protocol rounds. |
| Empty `announce()` | Valid. Updates self member's `last_seen` and incarnation. Can be used as explicit heartbeat from application code. |
| `discover()` stale pointer | The `const Member*` points into internal map. Valid until next mutation on that backend. Documented constraint. |
| `on_member_change` callback throws | Undefined behavior. Callback must not throw (HPActor is `-fno-exceptions`). |
| Seed list includes self | Ignored. Self is filtered from `pick_random_peers()`. |
| Hybrid mode: registrar enters client mode | Normal — another process on this host is the server. Gossip still starts and provides cross-host discovery. |
| `ActorLocationCache` filled beyond bound | No hard limit. Cache grows with unique `ActorId` lookups. TTL eviction provides back-pressure. For systems with millions of actors, set shorter TTL or call `purge_expired()` aggressively. |
| Network partition heals | Both halves re-converge via next protocol round. Higher incarnation wins conflicts. Actors linked across the partition receive `DownMsg` during partition; re-link is application-level. |

## 11. File Manifest

### New Files (9)

| File | Lines | Description |
|------|-------|-------------|
| `include/hpactor/net/service_discovery.hpp` | ~60 | `IServiceDiscovery`, `Member`, `MemberStatus` |
| `include/hpactor/net/static_discovery.hpp` | ~40 | `StaticDiscovery` |
| `include/hpactor/net/actor_location_cache.hpp` | ~50 | `ActorLocationCache` |
| `include/hpactor/net/gossip_membership.hpp` | ~100 | `GossipConfig`, `GossipMembership` |
| `include/hpactor/net/hybrid_discovery.hpp` | ~50 | `HybridDiscovery` |
| `src/net/gossip_membership.cpp` | ~500 | SWIM protocol, UDP I/O, wire encode/decode |
| `src/net/hybrid_discovery.cpp` | ~80 | Composition logic |
| `src/net/actor_location_cache.cpp` | ~40 | Cache implementation |
| *(toml_parser additions in existing file)* | ~60 | Parse `[system.discovery]` sections |

### Modified Files (7)

| File | Lines Δ | Change |
|------|---------|--------|
| `include/hpactor/core/actor_system.hpp` | +15 | `discovery_`, `location_cache_` members; Config fields |
| `src/actor/actor_system.cpp` | +40 | Backend selection, `on_node_dead`, proactive pool |
| `include/hpactor/net/registrar.hpp` | +10 | `IServiceDiscovery` base, adapter members |
| `src/net/registrar.cpp` | +40 | IServiceDiscovery method implementations |
| `src/ref/actor_proxy.cpp` | +15 | Location cache integration in `send()` |
| `src/net/connection_pool.cpp` | +20 | `prewarm_pool()` method |
| `CMakeLists.txt` | +5 | New source files |

### New Test Files (2)

| File | Lines | Description |
|------|-------|-------------|
| `tests/net/test_gossip_membership.cpp` | ~300 | SWIM protocol unit tests |
| `tests/net/test_service_discovery.cpp` | ~200 | Interface contract + integration tests |

**Totals:** ~1,350 new lines, ~145 modified lines across 7 existing files.
