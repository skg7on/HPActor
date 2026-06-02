# Gossip Protocol Test Design

**Date:** 2026-06-02
**Status:** Approved
**Scope:** Integration and system tests for `GossipMembership` SWIM protocol implementation

## Motivation

`src/net/gossip_membership.cpp` has very low test coverage. The existing 14 unit
tests in `tests/unit/net/test_gossip_membership.cpp` only exercise members
accessed directly via `FRIEND_TEST` — construction, merge logic, wire
encode/decode, and tombstone purging. They never exercise the protocol layer:
message handlers, timer-driven rounds, failure detection, join/leave flows,
indirect probing, or piggyback dissemination.

## Approach

Two test layers:

1. **Integration tests** (11 scenarios) added to the existing
   `tests/unit/net/test_gossip_membership.cpp` in the `test_unit_net` binary.
   These use a `FakeUdpTransport` and manual timer stepping — no real sockets,
   fully deterministic.

2. **System tests** (3 scenarios) in a new `tests/system/test_system_gossip.cpp`
   file in the existing `test_system` binary. Real UDP sockets + EventLoop with
   `assert_eventually` polling.

## Transport Abstraction

### Interface (`include/hpactor/net/udp_transport.hpp`)

```cpp
class IUdpTransport {
public:
    virtual ~IUdpTransport() = default;
    virtual bool bind(uint16_t port) = 0;
    virtual void send(const StreamBuffer& data, const EndPoint& dest) = 0;
    virtual void close() = 0;

    using ReceiveCallback =
        std::function<void(const StreamBuffer&, const std::string&, uint16_t)>;
    virtual void set_receive_callback(ReceiveCallback) = 0;
};
```

### RealUdpTransport

Production implementation extracted from the current `setup_udp_socket()` /
`async_udp_send()` / `teardown_udp_socket()` code path. Delegates to the
EventLoop for async I/O. Lives in `src/net/udp_transport.cpp`.

### FakeUdpTransport

Header-only test double. Captures sent packets into a `std::vector`, exposes
`inject_packet()` to simulate inbound data.

```cpp
class FakeUdpTransport : public IUdpTransport {
public:
    struct SentPacket { StreamBuffer data; EndPoint dest; };
    std::vector<SentPacket> sent_packets;

    void inject_packet(const StreamBuffer& data,
                       const std::string& src_host, uint16_t src_port);
    void clear_sent() { sent_packets.clear(); }

    // bind/close are no-ops; send pushes to sent_packets;
    // set_receive_callback stores the callback for inject_packet to use.
};
```

### GossipMembership changes

Constructor takes `std::unique_ptr<IUdpTransport>` instead of directly managing
a socket. Default is `RealUdpTransport(loop)`. Internal methods call
`transport_->send()` / `transport_->close()` etc.

Impact: ~30 lines changed in `gossip_membership.cpp`. The read handler setup
moves into `RealUdpTransport` along with the `recvfrom` loop.

## Integration Tests (11 scenarios)

All run within `test_unit_net`. Each test wires two or three
`GossipMembership` instances with `FakeUdpTransport`, routing sent packets
manually via `inject_packet()`. Timer-driven logic is stepped explicitly by
calling `protocol_round()` directly (exposed via `FRIEND_TEST`).

1. **Join flow (2 nodes):** B sends Join to A; A responds with SyncRsp; B
   merges A's table; both see each other Alive; join retry cancelled.

2. **Protocol round Ping/Ack:** Two nodes know each other; A runs
   `protocol_round()` → Ping to B; B handles Ping → Ack to A; A handles Ack →
   pending ping cleared.

3. **Indirect probe PingReq:** 3 nodes A, B, C. A pings C; C drops. Timeout
   expires; A sends PingReq(B, C); B forwards Ping to C; C Acks B; B sends
   IndirectAck to A; A clears pending ping for C. C never marked suspicious.

4. **Failure detection — Suspicious (2-node):** A pings B; B doesn't respond.
   Direct ping times out; no other peers for indirect probes; A marks B
   Suspicious immediately. Verify `on_member_change` fires.

5. **Failure detection — Dead:** B is Suspicious. Time advances past
   `suspicion_timeout`. Protocol round marks B Dead. Callback fires.

6. **Leave propagation:** B calls `stop()` → sends Leave to A. A handles
   Leave → marks B Left. Callback fires.

7. **Piggyback dissemination:** 3 nodes. A marks B Suspicious. A's protocol
   round pings C with Suspicious(B) piggyback. C applies piggyback → C also
   knows B is Suspicious.

8. **Incarnation conflict resolution:** A has B at inc=10 Alive. Piggyback
   claiming B Dead at inc=8 → A ignores. Piggyback at inc=12 Dead → A accepts.

9. **Member change callback:** Inject new member via piggyback → callback fires
   with `joined=true`. Mark Dead → callback fires with `joined=false`.

10. **Tombstone purging:** Dead member with ancient `last_seen`; protocol round
    calls `purge_dead_tombstones()`; Dead removed, Alive remain.

11. **Fault injection — packet loss:** Enable `hpactor.gossip.packet.loss`
    fault point. Send Ping → silently dropped. Disable → delivery resumes.

## System Tests (3 scenarios)

Added to the existing `test_system` binary. Real UDP sockets + EventLoop on
localhost. Polling via `assert_eventually`.

S1. **Two-node join and discovery:** Two instances on different localhost ports.
    A starts solo, B seeds from A. Poll until both `discover_all()` return 2.

S2. **Failure detection end-to-end:** 3 nodes. Isolate one (firewall or socket
    option). Poll until other 2 converge on Dead within timeout.

S3. **Graceful leave:** 2 nodes. B leaves via `stop()`. Poll until A sees B as
    Left.

## Files

### New
- `include/hpactor/net/udp_transport.hpp` — IUdpTransport, FakeUdpTransport (header-only), RealUdpTransport declaration
- `src/net/udp_transport.cpp` — RealUdpTransport implementation
- `tests/system/test_system_gossip.cpp` — 3 system tests

### Modified
- `include/hpactor/net/gossip_membership.hpp` — take `std::unique_ptr<IUdpTransport>`, new `FRIEND_TEST` declarations
- `src/net/gossip_membership.cpp` — use transport instead of raw socket
- `src/net/CMakeLists.txt` — add `udp_transport.cpp`
- `tests/unit/net/test_gossip_membership.cpp` — add 11 integration tests
- `tests/system/CMakeLists.txt` — add `test_system_gossip.cpp`

### Not modified
- Production code continues to default to `RealUdpTransport` — no API break.
- Existing 14 unit tests continue to pass (they don't touch the transport path).
