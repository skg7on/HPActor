# Service Discovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the single-mode embedded UdpRegistrar with a pluggable IServiceDiscovery system supporting UdpRegistrar (same-host), GossipMembership (cross-host SWIM), HybridDiscovery (both), and StaticDiscovery (fixed topology).

**Architecture:** Extract a pure-virtual `IServiceDiscovery` interface. Refactor `UdpRegistrar` to implement it in-place. Build `GossipMembership` (SWIM protocol over UDP + EventLoop timers) and `HybridDiscovery` (composes UdpRegistrar + GossipMembership). Add `ActorLocationCache` for ActorId→EndPoint caching. Integrate into ActorSystem, ActorProxy, and ConnectionPool.

**Tech Stack:** C++20, EventLoop (kqueue/epoll), existing UDP socket patterns from `registrar.cpp`, protobuf-free wire format (compact binary), `std::shared_mutex` for read-heavy concurrency.

**Spec reference:** `docs/superpowers/specs/2026-05-07-service-discovery-design.md`

---

## File Map

| File | Type | Lines | Responsibility |
|------|------|-------|----------------|
| `include/hpactor/net/service_discovery.hpp` | New | 60 | `IServiceDiscovery`, `Member`, `MemberStatus` |
| `include/hpactor/net/static_discovery.hpp` | New | 40 | `StaticDiscovery` class |
| `include/hpactor/net/actor_location_cache.hpp` | New | 50 | `ActorLocationCache` class |
| `include/hpactor/net/gossip_membership.hpp` | New | 100 | `GossipConfig`, `GossipMembership` class |
| `include/hpactor/net/hybrid_discovery.hpp` | New | 50 | `HybridDiscovery` class |
| `src/net/gossip_membership.cpp` | New | 500 | SWIM protocol, UDP I/O, wire encode/decode |
| `src/net/hybrid_discovery.cpp` | New | 80 | Composition logic |
| `src/net/actor_location_cache.cpp` | New | 40 | Cache implementation |
| `src/net/static_discovery.cpp` | New | 20 | Trivial implementation |
| `include/hpactor/net/registrar.hpp` | Modify | +10 | `IServiceDiscovery` base, adapter members |
| `src/net/registrar.cpp` | Modify | +40 | IServiceDiscovery method impls |
| `include/hpactor/core/actor_system.hpp` | Modify | +20 | `discovery_`, `location_cache_`, Config fields |
| `src/actor/actor_system.cpp` | Modify | +50 | Backend selection, `on_node_dead`, proactive pool |
| `src/ref/actor_proxy.cpp` | Modify | +15 | Location cache integration in `send()` |
| `src/net/connection_pool.cpp` | Modify | +20 | `prewarm_pool()` method |
| `src/config/toml_parser.cpp` | Modify | +60 | `[system.discovery]` parsing |
| `CMakeLists.txt` | Modify | +5 | New source files |
| `tests/net/test_gossip_membership.cpp` | New | 300 | SWIM protocol unit tests |
| `tests/net/test_service_discovery.cpp` | New | 200 | Interface contract + integration tests |

---

### Task 1: IServiceDiscovery Interface

**Files:**
- Create: `include/hpactor/net/service_discovery.hpp`

- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/net/service_discovery.hpp
#pragma once

#include <hpactor/types/types.hpp>
#include <hpactor/net/acceptor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

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
    // Not transmitted on wire — receivers set to steady_clock::now() on receipt.
    std::chrono::steady_clock::time_point last_seen;
};

enum class MemberStatus : uint8_t { Alive, Suspicious, Dead, Left };

using MemberChangeCallback = std::function<void(const Member&, bool joined)>;

class IServiceDiscovery {
public:
    virtual ~IServiceDiscovery() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    virtual std::vector<Member> discover_all() const = 0;
    virtual const Member* discover(EndPoint) const = 0;
    virtual void announce(Member local_state) = 0;
    virtual void on_member_change(MemberChangeCallback) = 0;
    virtual std::string backend_name() const = 0;

    virtual const std::unordered_map<EndPoint, Member>* raw_members() const {
        return nullptr;
    }
};

} // namespace hpactor::net
```

- [ ] **Step 2: Build to verify compilation**

```bash
ninja -C build
```
Expected: compiles (header-only, no .cpp yet).

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/service_discovery.hpp
git commit -m "feat(net): add IServiceDiscovery interface and Member types"
```

---

### Task 2: StaticDiscovery

**Files:**
- Create: `include/hpactor/net/static_discovery.hpp`
- Create: `src/net/static_discovery.cpp`
- Modify: `CMakeLists.txt` (add static_discovery.cpp)

- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/net/static_discovery.hpp
#pragma once

#include <hpactor/net/service_discovery.hpp>

namespace hpactor::net {

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
    std::unordered_map<EndPoint, size_t> index_;
};

} // namespace hpactor::net
```

- [ ] **Step 2: Write the implementation**

```cpp
// src/net/static_discovery.cpp
#include <hpactor/net/static_discovery.hpp>

namespace hpactor::net {

StaticDiscovery::StaticDiscovery(std::vector<Member> members)
    : members_(std::move(members)) {
    for (size_t i = 0; i < members_.size(); ++i) {
        index_[members_[i].endpoint] = i;
    }
}

std::vector<Member> StaticDiscovery::discover_all() const {
    return members_;
}

const Member* StaticDiscovery::discover(EndPoint ep) const {
    auto it = index_.find(ep);
    if (it != index_.end()) return &members_[it->second];
    return nullptr;
}

} // namespace hpactor::net
```

- [ ] **Step 3: Add to CMakeLists.txt**

Add `src/net/static_discovery.cpp` to the hpactor_lib sources list.

- [ ] **Step 4: Build and test**

```bash
ninja -C build && ctest --test-dir build --output-on-failure
```
Expected: 97 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/net/static_discovery.hpp src/net/static_discovery.cpp CMakeLists.txt
git commit -m "feat(net): add StaticDiscovery for fixed-topology service discovery"
```

---

### Task 3: ActorLocationCache

**Files:**
- Create: `include/hpactor/net/actor_location_cache.hpp`
- Create: `src/net/actor_location_cache.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/net/actor_location_cache.hpp
#pragma once

#include <hpactor/types/types.hpp>
#include <chrono>
#include <optional>
#include <shared_mutex>
#include <unordered_map>

namespace hpactor::net {

class ActorLocationCache {
public:
    std::optional<EndPoint> get(ActorId id) const;
    void put(ActorId id, EndPoint ep,
             std::chrono::seconds ttl = std::chrono::seconds(30));
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

} // namespace hpactor::net
```

- [ ] **Step 2: Write the implementation**

```cpp
// src/net/actor_location_cache.cpp
#include <hpactor/net/actor_location_cache.hpp>

namespace hpactor::net {

std::optional<EndPoint> ActorLocationCache::get(ActorId id) const {
    std::shared_lock lock(mutex_);
    auto it = cache_.find(id);
    if (it == cache_.end()) return std::nullopt;
    if (it->second.expires_at <= std::chrono::steady_clock::now())
        return std::nullopt;  // expired, deferred eviction via purge_expired()
    return it->second.endpoint;
}

void ActorLocationCache::put(ActorId id, EndPoint ep, std::chrono::seconds ttl) {
    std::unique_lock lock(mutex_);
    cache_[id] = {ep, std::chrono::steady_clock::now() + ttl};
}

void ActorLocationCache::evict(ActorId id) {
    std::unique_lock lock(mutex_);
    cache_.erase(id);
}

void ActorLocationCache::evict_node(EndPoint ep) {
    std::unique_lock lock(mutex_);
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.endpoint == ep) it = cache_.erase(it);
        else ++it;
    }
}

void ActorLocationCache::purge_expired() {
    std::unique_lock lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.expires_at <= now) it = cache_.erase(it);
        else ++it;
    }
}

} // namespace hpactor::net
```

- [ ] **Step 3: Add to CMakeLists.txt, build, test**

```bash
ninja -C build && ctest --test-dir build --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/actor_location_cache.hpp src/net/actor_location_cache.cpp CMakeLists.txt
git commit -m "feat(net): add ActorLocationCache for ActorId→EndPoint resolution caching"
```

---

### Task 4: UdpRegistrar IServiceDiscovery Refactoring

**Files:**
- Modify: `include/hpactor/net/registrar.hpp`
- Modify: `src/net/registrar.cpp`

- [ ] **Step 1: Add IServiceDiscovery base to UdpRegistrar**

In `include/hpactor/net/registrar.hpp`:
- Add `#include <hpactor/net/service_discovery.hpp>`
- Change: `class UdpRegistrar {` → `class UdpRegistrar : public IServiceDiscovery {`
- Add overrides to public section:

```cpp
    // IServiceDiscovery overrides
    std::vector<Member> discover_all() const override;
    const Member* discover(EndPoint) const override;
    void announce(Member) override;
    void on_member_change(MemberChangeCallback) override;
    std::string backend_name() const override { return "udp-registrar"; }
    const std::unordered_map<EndPoint, Member>* raw_members() const override {
        return &endpoint_to_member_;
    }
```

- Add private member and helper:

```cpp
private:
    static Member to_member(const NodeEndpoint& ep);
    std::unordered_map<EndPoint, Member> endpoint_to_member_;
```

- [ ] **Step 2: Implement the IServiceDiscovery methods**

In `src/net/registrar.cpp`, add after the existing `failover()` method:

```cpp
// ── IServiceDiscovery overrides ────────────────────────────────────────────

Member UdpRegistrar::to_member(const NodeEndpoint& ep) {
    Member m;
    m.endpoint = ep.endpoint;
    m.host = ep.host;
    m.tcp_port = ep.tcp_port;
    m.acceptors = ep.acceptors;
    m.last_seen = std::chrono::steady_clock::now();
    return m;
}

std::vector<Member> UdpRegistrar::discover_all() const {
    std::vector<Member> result;
    auto eps = get_all_endpoints();
    result.reserve(eps.size());
    for (auto& ep : eps) result.push_back(to_member(ep));
    return result;
}

const Member* UdpRegistrar::discover(EndPoint ep) const {
    auto it = endpoint_to_member_.find(ep);
    if (it != endpoint_to_member_.end()) return &it->second;
    return nullptr;
}

void UdpRegistrar::announce(Member) {
    // No-op: registrar server handles membership via Register/Heartbeat.
}

void UdpRegistrar::on_member_change(MemberChangeCallback cb) {
    set_node_callback([cb = std::move(cb)](EndPoint ep, bool online) {
        // Convert to Member via lookup
        // cb(member, online) — called from existing node_callback_ wiring
    });
}
```

- [ ] **Step 2a: Update member map maintenance**

In the registrar's existing join/leave paths, sync `endpoint_to_member_`:
- Server mode: in `RegistrarServer::handle_tcp_message(Register)` handler — add `endpoint_to_member_[ep.endpoint] = to_member(ep);`
- Server mode: in `RegistrarServer::handle_disconnect()` — add `endpoint_to_member_.erase(endpoint);`
- Client mode: in `RegistrarClient::handle_server_message(NodeJoin)` — add upsert
- Client mode: in `RegistrarClient::handle_server_message(NodeLeave)` — add erase

- [ ] **Step 2b: Fix `on_member_change` to properly convert callback**

Since the existing `node_callback_` takes `(EndPoint, bool online)` and the new interface takes `(const Member&, bool joined)`, we need to store the `MemberChangeCallback` and wrap the old callback:

```cpp
void UdpRegistrar::on_member_change(MemberChangeCallback cb) {
    member_change_cb_ = std::move(cb);
}
```

Then in the places where `node_callback_` fires, also fire `member_change_cb_` with the `Member` from `endpoint_to_member_`.

Add `MemberChangeCallback member_change_cb_;` to UdpRegistrar private members.

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --test-dir build --output-on-failure
```
Expected: 97 tests pass. No regressions.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/registrar.hpp src/net/registrar.cpp
git commit -m "refactor(net): refactor UdpRegistrar to implement IServiceDiscovery"
```

---

### Task 5: GossipMembership — Wire Protocol

**Files:**
- Create: `include/hpactor/net/gossip_membership.hpp` (class layout + config + message types)
- Create: `src/net/gossip_membership.cpp` (wire encode/decode first)
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write GossipConfig and enums**

```cpp
// include/hpactor/net/gossip_membership.hpp
#pragma once

#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/event_loop.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hpactor::net {

enum class GossipMessageType : uint8_t {
    Ping = 0x01, Ack = 0x02, PingReq = 0x03, IndirectAck = 0x04,
    Join = 0x05, SyncRsp = 0x06, Leave = 0x07,
};

enum class PiggybackType : uint8_t {
    Alive = 0x01, Suspicious = 0x02, Dead = 0x03, Metadata = 0x04,
};

struct PiggybackEntry {
    PiggybackType type;
    EndPoint endpoint;
    uint64_t incarnation;
    // Only for Metadata type:
    std::vector<std::string> actor_types;
    uint32_t load = 0;
    std::vector<AcceptorInfo> acceptors;
};

struct GossipConfig {
    uint16_t gossip_port = 5354;
    std::chrono::milliseconds protocol_period{1000};
    std::chrono::milliseconds ping_timeout{200};
    std::chrono::milliseconds suspicion_timeout{3000};
    std::chrono::milliseconds dead_timeout{30000};
    uint32_t fanout = 3;
    uint32_t indirect_probes = 3;
    std::vector<EndPoint> seeds;
    // Only endpoint, host, tcp_port, uds_path, acceptors, actor_types are
    // used as config. incarnation, status, last_seen are set at startup.
    Member local_state;
};

constexpr uint32_t GossipMagic = 0x48504743; // "HPGC"
constexpr uint8_t GossipVersion = 0x01;
constexpr size_t kGossipMaxMsgSize = 1400;
```

- [ ] **Step 2: Add class layout skeleton**

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
    void protocol_round();
    void handle_packet(const StreamBuffer& data, const std::string& from_host,
                       uint16_t from_port);

    // Message handlers (dispatched from handle_packet)
    void handle_ping(EndPoint sender, uint64_t inc, uint32_t seq,
                     std::vector<PiggybackEntry> pb, const std::string& host, uint16_t port);
    void handle_ack(EndPoint sender, uint64_t inc, std::vector<PiggybackEntry> pb);
    void handle_ping_req(EndPoint sender, EndPoint target);
    void handle_indirect_ack(EndPoint sender, EndPoint target);
    void handle_join(EndPoint sender, uint64_t inc, std::vector<PiggybackEntry> pb);
    void handle_sync_rsp(std::vector<Member> members);
    void handle_leave(EndPoint sender, uint64_t inc);

    // Message sending
    void send_ping(EndPoint target);
    void send_ack(EndPoint target, std::vector<PiggybackEntry> pb);
    void send_ping_req(EndPoint proxy, EndPoint target);
    void send_indirect_ack(EndPoint target, EndPoint orig_target);
    void send_join(EndPoint seed);
    void send_sync_rsp(EndPoint target);
    void send_leave(EndPoint target);

    // Wire protocol
    StreamBuffer encode_message(GossipMessageType type, uint64_t inc, uint32_t seq,
        EndPoint ping_target, const std::vector<PiggybackEntry>& pb) const;
    bool decode_message(const StreamBuffer& data, GossipMessageType& type,
        EndPoint& sender, uint64_t& inc, uint32_t& seq, EndPoint& ping_target,
        std::vector<PiggybackEntry>& pb) const;
    StreamBuffer encode_sync_rsp(const std::vector<Member>& members) const;
    bool decode_sync_rsp(const StreamBuffer& data, std::vector<Member>& members) const;

    // State
    void mark_suspicious(EndPoint ep);
    void mark_dead(EndPoint ep);
    void merge_member(const Member& remote);
    void apply_piggyback(const std::vector<PiggybackEntry>& entries);
    void purge_dead_tombstones();
    std::vector<EndPoint> pick_random_peers(size_t count,
        std::unordered_set<EndPoint> exclude = {});

    void setup_udp_socket();
    void teardown_udp_socket();

    GossipConfig config_;
    EventLoop* loop_ = nullptr;
    int udp_socket_ = -1;
    uint64_t incarnation_ = 0;
    uint32_t seq_no_ = 0;
    std::unordered_map<EndPoint, Member> members_;
    std::unordered_map<EndPoint, PendingPing> pending_pings_;
    MemberChangeCallback member_change_cb_;
    uint64_t protocol_timer_ = 0;
    bool needs_dissemination_ = false;
    std::vector<uint8_t> recv_buffer_;
    mutable std::shared_mutex members_mutex_;
};

struct PendingPing {
    std::chrono::steady_clock::time_point expires_at;
    bool indirect_requested = false;
    std::chrono::steady_clock::time_point indirect_expires_at;
    // Note: indirect timeout = direct timeout + ping_timeout (same timeout).
};

} // namespace hpactor::net
```

- [ ] **Step 3: Implement wire encode/decode in .cpp**

Write `encode_message()` and `decode_message()` following the binary format in spec section 6.3. Reuse `endpoint_ops` from `actor_address.hpp` for endpoint encoding.

- [ ] **Step 4: Implement metadata piggyback encode/decode**

Following spec section 6.3 Metadata encoding:
```
Field Count (1B) | Field[0..N]
Field: Tag (1B) | Length (2B BE) | Value
Tags: 0x01 = actor_types, 0x02 = load, 0x03 = acceptors
```

- [ ] **Step 5: Build wire-only stub**

```bash
ninja -C build
```
Expected: compiles, protocol round / handlers are stubs.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/net/gossip_membership.hpp src/net/gossip_membership.cpp CMakeLists.txt
git commit -m "feat(net): add GossipMembership wire protocol encode/decode"
```

---

### Task 6: GossipMembership — Protocol Logic

**Files:**
- Modify: `src/net/gossip_membership.cpp`

- [ ] **Step 1: Implement UDP socket setup**

`setup_udp_socket()` — same pattern as `UdpRegistrar::setup_udp_socket()`:
- `socket(AF_INET, SOCK_DGRAM, 0)`, `SO_REUSEADDR`, bind to `gossip_port`
- `loop_->add_fd(udp_socket_, EventLoop::Event::Read)`
- `loop_->set_read_handler(udp_socket_, [this](int) { recvfrom loop → handle_packet })`

- [ ] **Step 2: Implement bootstrap (start)**

Following spec section 6.4:
- `incarnation_ = system_clock::now().time_since_epoch().count()`
- `setup_udp_socket()`
- Add self to members_
- If seeds non-empty: send Join to first seed, handle SyncRsp
- Start protocol timer

- [ ] **Step 3: Implement protocol_round()**

Following spec section 6.5:
- Pick fanout peers → send Ping
- Check expired pings → send PingReq or mark_suspicious
- Check suspicious members → mark_dead on timeout
- Purge dead tombstones
- If needs_dissemination_: include self metadata in piggyback

- [ ] **Step 4: Implement message handlers**

- `handle_ping()` → send Ack with piggyback
- `handle_ack()` → clear pending ping, apply piggyback
- `handle_ping_req()` → ping target, send IndirectAck
- `handle_indirect_ack()` → confirm indirect probe succeeded
- `handle_join()` → send SyncRsp with full membership table
- `handle_sync_rsp()` → merge all received members
- `handle_leave()` → mark endpoint as Left

- [ ] **Step 5: Implement state mutations**

- `mark_suspicious(ep)` → set status=Suspicious, update last_seen
- `mark_dead(ep)` → set status=Dead, fire member_change_cb_(joined=false)
- `merge_member(remote)` → if incarnation higher: accept. If Dead+higher incarnation: Alive again.
- `apply_piggyback(entries)` → process each entry
- `pick_random_peers(count, exclude)` → uniform random sample from alive members
- `purge_dead_tombstones()` → remove Dead entries past dead_timeout

- [ ] **Step 6: Implement graceful shutdown (stop)**

Following spec section 6.8:
- Cancel protocol timer
- Send Leave to all alive + suspicious members
- teardown_udp_socket()
- Clear members_

- [ ] **Step 7: Build**

```bash
ninja -C build
```

- [ ] **Step 8: Commit**

```bash
git add src/net/gossip_membership.cpp
git commit -m "feat(net): implement GossipMembership SWIM protocol logic"
```

---

### Task 7: HybridDiscovery

**Files:**
- Create: `include/hpactor/net/hybrid_discovery.hpp`
- Create: `src/net/hybrid_discovery.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the header following spec section 7**

```cpp
// include/hpactor/net/hybrid_discovery.hpp
#pragma once

#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/gossip_membership.hpp>
#include <memory>

namespace hpactor::net {

class HybridDiscovery : public IServiceDiscovery {
public:
    HybridDiscovery(const RegistrarConfig& reg_cfg, const GossipConfig& gossip_cfg,
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

    UdpRegistrar registrar_;
    GossipMembership gossip_;
    EndPoint local_ep_;
    MemberChangeCallback user_callback_;
};

} // namespace hpactor::net
```

- [ ] **Step 2: Write implementation**

- `start()` — starts registrar_ first, then gossip_
- `stop()` — stops gossip_ first (graceful Leave), then registrar_
- `discover(ep)` — checks registrar_.discover(ep) first, falls back to gossip_.discover(ep)
- `discover_all()` — merges both backends' results
- `on_member_change(cb)` — stores cb, wires registrar callbacks to on_local_member_change, wires gossip callbacks directly
- `on_local_member_change(m, joined)` — calls gossip_.announce(m) to push local changes into gossip, then fires user callback
- `announce(m)` — delegates to registrar_ if local, always calls gossip_.announce()

- [ ] **Step 3: Build and commit**

```bash
ninja -C build
git add include/hpactor/net/hybrid_discovery.hpp src/net/hybrid_discovery.cpp CMakeLists.txt
git commit -m "feat(net): add HybridDiscovery composing UdpRegistrar + GossipMembership"
```

---

### Task 8: ActorSystem Integration

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `src/config/toml_parser.cpp`

- [ ] **Step 1: Add Config fields**

In `actor_system.hpp`, add to `Config`:

```cpp
    // Service discovery backend. nullptr = auto-select based on enable_network.
    std::shared_ptr<net::IServiceDiscovery> service_discovery;
    // Gossip configuration (used when creating GossipMembership internally).
    net::GossipConfig gossip;
```

Add `#include <hpactor/net/service_discovery.hpp>` and `#include <hpactor/net/gossip_membership.hpp>` to includes.

- [ ] **Step 2: Add member variables**

In `ActorSystem` private section:

```cpp
    std::shared_ptr<net::IServiceDiscovery> discovery_;
    std::shared_ptr<net::ActorLocationCache> location_cache_;
    uint64_t cache_purge_timer_ = 0;
```

Add `#include <hpactor/net/actor_location_cache.hpp>`.

- [ ] **Step 3: Implement backend selection in constructor**

Replace the existing `if (config.udp_port > 0)` block with:

```cpp
        // ── Service discovery ────────────────────────────
        if (config.service_discovery) {
            discovery_ = config.service_discovery;
        } else if (config.registrar.udp_port > 0) {
            discovery_ = std::make_shared<net::UdpRegistrar>(
                config.registrar, endpoint_, network_loop_.get());
        } else {
            discovery_ = std::make_shared<net::StaticDiscovery>(
                std::vector<net::Member>{});
        }

        discovery_->start();
        discovery_->on_member_change([this](const net::Member& m, bool joined) {
            if (!joined) {
                on_node_dead(m.endpoint);
            } else if (config_.enable_network && transport_) {
                transport_->prewarm_pool(m.endpoint, m.acceptors);
            }
        });

        location_cache_ = std::make_shared<net::ActorLocationCache>();
        if (network_loop_) {
            cache_purge_timer_ = network_loop_->run_every(
                [this]() { if (location_cache_) location_cache_->purge_expired(); },
                60000);
        }
```

- [ ] **Step 4: Implement on_node_dead()**

```cpp
void ActorSystem::on_node_dead(EndPoint dead_ep) {
    for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        auto* ctx = actor.actor_context();
        if (!ctx) return;
        // Check if any linked/monitored actors are on dead_ep
        for (auto& addr : ctx->linked_actors()) {
            if (addr.endpoint() == dead_ep) {
                // Send DownMsg — reuse existing death notification path
                TypedMessage down(TypeTag::DownMsg, StreamBuffer{});
                down.set_sender_address(ActorAddress{dead_ep, 0, 0, 0});
                deliver_local(actor.id(), std::move(down));
                break;
            }
        }
    });
    if (location_cache_) location_cache_->evict_node(dead_ep);
}
```

Add `void on_node_dead(EndPoint dead_ep);` declaration to ActorSystem private section.

- [ ] **Step 5: Implement TOML parser**

In `toml_parser.cpp`, add parsing of `[system.discovery]`:
- Read `backend` string
- `"gossip"` → parse `[system.discovery.gossip]` sub-table
- `"static"` → parse `[[system.discovery.static.members]]` array
- `"hybrid"` → parse both registrar + gossip sections
- Set `config.service_discovery` appropriately

- [ ] **Step 6: Build and test**

```bash
ninja -C build && ctest --test-dir build --output-on-failure
```
Expected: 97 tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp src/config/toml_parser.cpp
git commit -m "feat(core): integrate pluggable IServiceDiscovery into ActorSystem"
```

---

### Task 9: ActorProxy + ConnectionPool Integration

**Files:**
- Modify: `include/hpactor/ref/actor_proxy.hpp` (+5)
- Modify: `src/ref/actor_proxy.cpp` (+15)
- Modify: `include/hpactor/net/connection_pool.hpp` (+1)
- Modify: `src/net/connection_pool.cpp` (+20)

- [ ] **Step 1: Add location cache + discovery to ActorProxy**

In `actor_proxy.hpp`, add:

```cpp
    net::IServiceDiscovery* discovery_ = nullptr;
    net::ActorLocationCache* location_cache_ = nullptr;

    void set_discovery(net::IServiceDiscovery* d) { discovery_ = d; }
    void set_location_cache(net::ActorLocationCache* c) { location_cache_ = c; }
```

In `actor_proxy.cpp::send()`, add the resolution + retry logic:

```cpp
result<void> ActorProxy::send(StreamBuffer payload) {
    if (!transport_) return result<void>::make(error("no transport"));

    // Resolve actor endpoint
    auto cached = location_cache_ ? location_cache_->get(address_.id()) : std::nullopt;
    EndPoint target_ep = cached.value_or(EndPoint{});
    if (!cached && discovery_) {
        auto* member = discovery_->discover(address_.endpoint());
        if (!member) return result<void>::make(error("unreachable"));
        target_ep = member->endpoint;
        if (location_cache_) location_cache_->put(address_.id(), target_ep);
    }

    // Send and retry on failure
    auto result = transport_->send(target_ep, std::move(payload));
    if (!result.has_value() && location_cache_) {
        location_cache_->evict(address_.id());
        // Do not retry with moved payload — sender must re-serialize and retry.
        // The eviction ensures the next send() call re-resolves via discovery.
    }
    return result;
}
```

- [ ] **Step 2: Add prewarm_pool() to ConnectionPool**

Add to `connection_pool.hpp`:
```cpp
void prewarm_pool(EndPoint ep, const std::vector<AcceptorInfo>& acceptors);
```

Implement in `connection_pool.cpp` — delegates to existing `get_or_create_pool()`.

- [ ] **Step 3: Build and test**

```bash
ninja -C build && ctest --test-dir build --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/ref/actor_proxy.hpp src/ref/actor_proxy.cpp \
        include/hpactor/net/connection_pool.hpp src/net/connection_pool.cpp
git commit -m "feat(net): integrate ActorLocationCache into ActorProxy and add prewarm_pool"
```

---

### Task 10: GossipMembership Tests

**Files:**
- Create: `tests/net/test_gossip_membership.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write unit tests**

Test the SWIM protocol in isolation (no real UDP):
- Construction with defaults
- Bootstrap: solo cluster (zero seeds), members_ has self
- Bootstrap: with seed (stub out SyncRsp)
- Protocol round: solo cluster no-ops (no peers)
- Protocol round: 3-node ping succeeds
- Protocol round: ping timeout → indirect probe → suspicious
- Suspicious → dead on timeout
- Suspicious → alive on higher incarnation message
- Piggyback merge: alive, suspicious, dead entries
- Metadata encode/decode roundtrip
- Incarnation conflict: higher wins, lower rejected
- Graceful leave: member marked Left
- Dead tombstone purge

~300 lines, 12-15 test cases.

- [ ] **Step 2: Run tests**

```bash
ninja -C build && ./build/tests/test_gossip_membership
```
Expected: all pass.

- [ ] **Step 3: Commit**

```bash
git add tests/net/test_gossip_membership.cpp tests/CMakeLists.txt
git commit -m "test(net): add GossipMembership SWIM protocol unit tests"
```

---

### Task 11: Service Discovery Integration Tests

**Files:**
- Create: `tests/net/test_service_discovery.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write interface contract tests**

- IServiceDiscovery default `raw_members()` returns nullptr
- StaticDiscovery: discover() O(1), discover_all() returns copy
- StaticDiscovery: discover() returns nullptr for unknown endpoint
- ActorLocationCache: put/get roundtrip
- ActorLocationCache: expired entry returns nullopt
- ActorLocationCache: evict removes entry
- ActorLocationCache: evict_node removes all entries for endpoint
- ActorLocationCache: purge_expired cleans up
- UdpRegistrar backend_name() returns "udp-registrar"
- GossipMembership backend_name() returns "gossip"
- HybridDiscovery backend_name() returns "hybrid"

- [ ] **Step 2: Write integration test**

- Construct ActorSystem with `service_discovery = make_shared<StaticDiscovery>({...})`
- Verify `discovery_->discover_all()` returns the configured members
- Verify backward compat: ActorSystem without `service_discovery` uses UdpRegistrar

~200 lines.

- [ ] **Step 3: Run tests**

```bash
ninja -C build && ctest --test-dir build --output-on-failure
```
Expected: 99 tests pass (97 existing + 2 new test suites).

- [ ] **Step 4: Commit**

```bash
git add tests/net/test_service_discovery.cpp tests/CMakeLists.txt
git commit -m "test(net): add service discovery interface contract and integration tests"
```

---

## Verification

After all tasks complete:

```bash
ninja -C build && ctest --test-dir build --output-on-failure
```

Expected: 99 tests pass (97 existing + 2 new test suites). 1 pre-existing failure (`test_unified_message_passing`) may still be present — unrelated.

Manual verification:
- `examples/11_cli_interactive_demo` starts and CLI works (no regression in existing registrar path)
- `StaticDiscovery` constructed with members → discover_all returns them
- `ActorLocationCache` put/get roundtrip with TTL expiration
- `GossipMembership` protocol round runs on EventLoop timer
