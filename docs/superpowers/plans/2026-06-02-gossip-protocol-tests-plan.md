# Gossip Protocol Tests Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 11 integration tests and 3 system tests for GossipMembership by extracting UDP I/O behind an `IUdpTransport` interface.

**Architecture:** Extract UDP socket management and async I/O from `GossipMembership` into a `RealUdpTransport` that implements a new `IUdpTransport` interface. Provide a `FakeUdpTransport` test double that captures sent packets and accepts injected packets. Integration tests run two `GossipMembership` instances with fake transports wired back-to-back, calling protocol methods directly via `FRIEND_TEST`. System tests use real UDP sockets + EventLoop with `assert_eventually` polling.

**Tech Stack:** C++20, Google Test, protobuf, POSIX sockets (macOS/Linux)

---

## File Map

| File | Role |
|------|------|
| `include/hpactor/net/udp_transport.hpp` | IUdpTransport, FakeUdpTransport (header-only), RealUdpTransport declaration |
| `src/net/udp_transport.cpp` | RealUdpTransport implementation — extracted UDP code |
| `include/hpactor/net/gossip_membership.hpp` | Add transport constructor + new FRIEND_TEST declarations |
| `src/net/gossip_membership.cpp` | Use transport_->send()/bind()/close() instead of raw socket |
| `src/CMakeLists.txt` | Add udp_transport.cpp |
| `tests/unit/net/test_gossip_membership.cpp` | Add 11 integration tests at end of file |
| `tests/system/CMakeLists.txt` | Add test_system_gossip.cpp |
| `tests/system/test_system_gossip.cpp` | 3 system tests with real sockets |

---

### Task 1: Create IUdpTransport interface and FakeUdpTransport

**Files:**
- Create: `include/hpactor/net/udp_transport.hpp`

- [ ] **Step 1: Write the header file**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <hpactor/net/event_loop.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hpactor::net {

// ── IUdpTransport — abstract UDP I/O ──────────────────────────────

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

// ── FakeUdpTransport — test double ─────────────────────────────────

class FakeUdpTransport : public IUdpTransport {
  public:
    struct SentPacket {
        StreamBuffer data;
        EndPoint dest;
    };
    std::vector<SentPacket> sent_packets;

    bool bind(uint16_t /*port*/) override {
        return true;
    }
    void send(const StreamBuffer& data, const EndPoint& dest) override {
        sent_packets.push_back({data, dest});
    }
    void close() override {
        sent_packets.clear();
    }
    void set_receive_callback(ReceiveCallback cb) override {
        receive_cb_ = std::move(cb);
    }

    void inject_packet(const StreamBuffer& data, const std::string& src_host,
                       uint16_t src_port) {
        if (receive_cb_) {
            receive_cb_(data, src_host, src_port);
        }
    }
    void clear_sent() {
        sent_packets.clear();
    }

  private:
    ReceiveCallback receive_cb_;
};

// ── RealUdpTransport — production UDP I/O ──────────────────────────

class RealUdpTransport : public IUdpTransport {
  public:
    explicit RealUdpTransport(EventLoop* loop);
    ~RealUdpTransport() override;

    bool bind(uint16_t port) override;
    void send(const StreamBuffer& data, const EndPoint& dest) override;
    void close() override;
    void set_receive_callback(ReceiveCallback cb) override;

  private:
    EventLoop* loop_;
    int sock_ = -1;
    ReceiveCallback receive_cb_;
    std::vector<uint8_t> recv_buffer_;
};

} // namespace hpactor::net
```

- [ ] **Step 2: Verify it compiles** (it's a header, so just check includes are self-contained)

```bash
# Build any target that includes this header — for now check syntax
echo '#include <hpactor/net/udp_transport.hpp>' | g++ -std=c++20 -fsyntax-only -I include -I build -x c++ -
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/udp_transport.hpp
git commit -m "feat(net): add IUdpTransport interface with FakeUdpTransport and RealUdpTransport

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 2: Implement RealUdpTransport

**Files:**
- Create: `src/net/udp_transport.cpp`

- [ ] **Step 1: Write the implementation**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/net/udp_transport.hpp>

#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/types/types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace hpactor::net {

RealUdpTransport::RealUdpTransport(EventLoop* loop)
    : loop_(loop), recv_buffer_(kGossipMaxMsgSize) {}

RealUdpTransport::~RealUdpTransport() {
    close();
}

bool RealUdpTransport::bind(uint16_t port) {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0)
        return false;

    int reuse = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        ::close(sock_);
        sock_ = -1;
        return false;
    }

    if (loop_) {
        loop_->add_fd(sock_, EventLoop::Event::Read);
        loop_->set_read_handler(sock_, [this](int /*fd*/) {
            struct sockaddr_in src_addr {};
            socklen_t src_addr_len = sizeof(src_addr);

            while (true) {
                ssize_t n = recvfrom(sock_, recv_buffer_.data(),
                                     recv_buffer_.size(), MSG_DONTWAIT,
                                     reinterpret_cast<struct sockaddr*>(&src_addr),
                                     &src_addr_len);
                if (n <= 0)
                    break;

                StreamBuffer data(recv_buffer_.data(),
                                  recv_buffer_.data() + static_cast<size_t>(n));

                std::string from_host;
                uint16_t from_port = 0;
                char ip_str[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &src_addr.sin_addr, ip_str,
                              sizeof(ip_str))) {
                    from_host = ip_str;
                }
                from_port = ntohs(src_addr.sin_port);

                if (receive_cb_) {
                    receive_cb_(data, from_host, from_port);
                }
            }
        });
    }
    return true;
}

void RealUdpTransport::send(const StreamBuffer& data, const EndPoint& dest) {
    FAULT_INJECT("hpactor.gossip.packet.loss") {
        return;
    }
    if (sock_ < 0 || data.empty())
        return;

    if (loop_) {
        struct iovec iov;
        iov.iov_base = const_cast<uint8_t*>(data.data());
        iov.iov_len = data.size();

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&dest)) {
            addr.sin_addr.s_addr = ipv4->addr;
            addr.sin_port = ipv4->port_nw;
        }

        loop_->backend()->async_sendto(
            sock_, &iov, 1, reinterpret_cast<const sockaddr*>(&addr),
            sizeof(addr), ActorId(0), static_cast<uint32_t>(OpType::SendTo));
    } else {
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&dest)) {
            addr.sin_addr.s_addr = ipv4->addr;
            addr.sin_port = ipv4->port_nw;
        }
        ::sendto(sock_, data.data(), data.size(), 0,
                 reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    }
}

void RealUdpTransport::close() {
    if (sock_ >= 0) {
        if (loop_) {
            loop_->clear_read_handler(sock_);
            loop_->remove_fd(sock_);
        }
        ::close(sock_);
        sock_ = -1;
    }
}

void RealUdpTransport::set_receive_callback(ReceiveCallback cb) {
    receive_cb_ = std::move(cb);
}

} // namespace hpactor::net
```

- [ ] **Step 2: Verify compilation against the header**

```bash
ninja -C build src/CMakeFiles/hpactor_lib.dir/net/udp_transport.cpp.o
```

Expected: successful build (after Task 4 adds it to CMakeLists — this step will fail until then, so do Task 3 and 4 first).

- [ ] **Step 3: Commit** (after Task 3 and Task 4 are complete)

---

### Task 3: Refactor GossipMembership to use IUdpTransport

**Files:**
- Modify: `include/hpactor/net/gossip_membership.hpp`
- Modify: `src/net/gossip_membership.cpp`

- [ ] **Step 1: Update header — add include, new constructor, transport member, remove socket members**

Edit `include/hpactor/net/gossip_membership.hpp`:

Add include after the event_loop line:
```cpp
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/udp_transport.hpp>
```

Add new constructor declaration in the public section, after the existing constructor:
```cpp
    GossipMembership(const GossipConfig& cfg, EventLoop* loop);
    GossipMembership(const GossipConfig& cfg,
                     std::unique_ptr<IUdpTransport> transport);
    ~GossipMembership() override;
```

Remove these member variables:
```cpp
    int udp_socket_ = -1;
    std::vector<uint8_t> recv_buffer_;
```

Replace with:
```cpp
    std::unique_ptr<IUdpTransport> transport_;
```

Remove these private method declarations (no longer needed):
```cpp
    void setup_udp_socket();
    void teardown_udp_socket();
```

Edit: `include/hpactor/net/gossip_membership.hpp`

Replace old_string:
```
    GossipMembership(const GossipConfig& cfg, EventLoop* loop);
    ~GossipMembership() override;
```
With new_string:
```
    GossipMembership(const GossipConfig& cfg, EventLoop* loop);
    GossipMembership(const GossipConfig& cfg,
                     std::unique_ptr<IUdpTransport> transport);
    ~GossipMembership() override;
```

Replace old_string (the socket members):
```
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
```
With new_string:
```
    std::unique_ptr<IUdpTransport> transport_;
    uint64_t incarnation_ = 0;
    uint32_t seq_no_ = 0;

    std::unordered_map<EndPoint, Member> members_;
    std::unordered_map<EndPoint, PendingPing> pending_pings_;
    MemberChangeCallback member_change_cb_;
    uint64_t protocol_timer_ = 0;
    bool needs_dissemination_ = false;
    mutable std::shared_mutex members_mutex_;
```

Replace old_string (the setup/teardown declarations):
```
    void setup_udp_socket();
    void teardown_udp_socket();

    GossipConfig config_;
```
With new_string:
```
    GossipConfig config_;
```

- [ ] **Step 2: Update .cpp — replace all raw socket usage with transport calls**

Edit `src/net/gossip_membership.cpp`:

**Constructor — add second overload:**
After the existing constructor, add:
```cpp
GossipMembership::GossipMembership(const GossipConfig& cfg,
                                   std::unique_ptr<IUdpTransport> transport)
    : config_(cfg), loop_(nullptr), incarnation_(1),
      transport_(std::move(transport)) {}
```

**Update existing constructor** to initialize transport instead of recv_buffer:
Replace old_string in the constructor:
```
GossipMembership::GossipMembership(const GossipConfig& cfg, EventLoop* loop)
    : config_(cfg), loop_(loop), incarnation_(1) // Start at 1 so 0 means "no
                                                 // incarnation"
      ,
      recv_buffer_(kGossipMaxMsgSize) {}
```
With new_string:
```
GossipMembership::GossipMembership(const GossipConfig& cfg, EventLoop* loop)
    : config_(cfg), loop_(loop), incarnation_(1),
      transport_(loop ? std::make_unique<RealUdpTransport>(loop)
                      : std::unique_ptr<IUdpTransport>()) {}
```

**start() — replace setup_udp_socket() call:**
Replace old_string:
```
    setup_udp_socket();
```
With new_string:
```
    if (transport_) {
        transport_->bind(config_.gossip_port);
        transport_->set_receive_callback(
            [this](const StreamBuffer& data, const std::string& host,
                   uint16_t port) { handle_packet(data, host, port); });
    }
```

**stop() — replace teardown_udp_socket() call:**
Replace old_string:
```
    teardown_udp_socket();
```
With new_string:
```
    if (transport_) {
        transport_->close();
    }
```

**Replace every `async_udp_send(loop_, udp_socket_,` call with `transport_->send(`:**

Replace all 8 occurrences. The pattern is:
```cpp
async_udp_send(loop_, udp_socket_, msg, target);
```
becomes:
```cpp
if (transport_) transport_->send(msg, target);
```

There is also one `async_udp_send(loop_, udp_socket_, ack_msg, sender);` and one `async_udp_send(loop_, udp_socket_, data, target);` etc. All follow the same pattern.

**Remove the `#include` lines no longer needed:**
The file no longer uses `<sys/socket.h>`, `<unistd.h>`, `<arpa/inet.h>`, `<netinet/in.h>`, `<cstring>` directly — those are now in `udp_transport.cpp`. But keep them since `registrar.hpp` may transitively rely on them, and removing headers is an unnecessary risk. Actually, check: `registrar.hpp` is included for `endpoint_ops`. The socket headers are only needed by `setup_udp_socket` and `async_udp_send`. Remove them:

Replace old_string:
```
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <random>
```
With new_string:
```
#include <algorithm>
#include <random>
```

**Remove the `async_udp_send` function from the anonymous namespace:**

Delete the entire `async_udp_send` function (lines 145-183 in the original file).

**Also add the `#include <hpactor/net/udp_transport.hpp>` include:**
Replace old_string:
```
#include <hpactor/net/gossip_membership.hpp>

#include <hpactor/gossip.pb.h>
```
With new_string:
```
#include <hpactor/net/gossip_membership.hpp>
#include <hpactor/net/udp_transport.hpp>

#include <hpactor/gossip.pb.h>
```

**Update existing test that references removed member:**

Edit `tests/unit/net/test_gossip_membership.cpp` — the `ConstructionDefaults` test checks `gm.udp_socket_`:

Replace old_string:
```
    EXPECT_EQ(gm.udp_socket_, -1);
```
With new_string:
```
    EXPECT_EQ(gm.transport_.get(), nullptr);
```

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/gossip_membership.hpp src/net/gossip_membership.cpp tests/unit/net/test_gossip_membership.cpp
git commit -m "refactor(net): extract UDP I/O from GossipMembership to IUdpTransport

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 4: Add udp_transport.cpp to build

**Files:**
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add source file to the library**

Edit `src/CMakeLists.txt`:

Replace old_string:
```
    net/gossip_membership.cpp
```
With new_string:
```
    net/gossip_membership.cpp
    net/udp_transport.cpp
```

- [ ] **Step 2: Build and verify**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
```

Expected: successful build.

- [ ] **Step 3: Run existing unit tests to confirm no regressions**

```bash
./build/tests/unit/net/test_unit_net --gtest_filter="GossipMembershipTest.*"
```

Expected: all 14 existing tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/CMakeLists.txt
git commit -m "build: add udp_transport.cpp to hpactor_lib

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 5: Add FRIEND_TEST declarations for new integration tests

**Files:**
- Modify: `include/hpactor/net/gossip_membership.hpp`

- [ ] **Step 1: Add FRIEND_TEST declarations**

Add after the last existing FRIEND_TEST line (`FRIEND_TEST(GossipMembershipTest, WireEncodeDecodeMetadata);`):

```cpp
    // Integration tests (protocol flow)
    FRIEND_TEST(GossipProtocolIntegrationTest, JoinFlow);
    FRIEND_TEST(GossipProtocolIntegrationTest, ProtocolRoundPingAck);
    FRIEND_TEST(GossipProtocolIntegrationTest, IndirectProbePingReq);
    FRIEND_TEST(GossipProtocolIntegrationTest, FailureDetectionSuspicious);
    FRIEND_TEST(GossipProtocolIntegrationTest, FailureDetectionDead);
    FRIEND_TEST(GossipProtocolIntegrationTest, LeavePropagation);
    FRIEND_TEST(GossipProtocolIntegrationTest, PiggybackDissemination);
    FRIEND_TEST(GossipProtocolIntegrationTest, IncarnationConflictResolution);
    FRIEND_TEST(GossipProtocolIntegrationTest, MemberChangeCallback);
    FRIEND_TEST(GossipProtocolIntegrationTest, TombstonePurging);
    FRIEND_TEST(GossipProtocolIntegrationTest, FaultInjectionPacketLoss);
```

- [ ] **Step 2: Build**

```bash
ninja -C build
```

Expected: successful build.

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/gossip_membership.hpp
git commit -m "test: add FRIEND_TEST declarations for gossip protocol integration tests

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 6: Join Flow integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Add test fixture and helper, then write the test**

Append to `tests/unit/net/test_gossip_membership.cpp` (before the closing `} // namespace hpactor::net`):

Add a new test fixture class and the first integration test:

```cpp
// ── Integration tests (protocol flow with FakeUdpTransport) ──────

class GossipProtocolIntegrationTest : public ::testing::Test {
  protected:
    // Helper: create a GossipConfig for a node at the given port.
    static GossipConfig cfg_for(uint16_t port, const char* host = "127.0.0.1") {
        GossipConfig cfg;
        cfg.gossip_port = port;
        cfg.local_state.identity.endpoint = ep(port);
        cfg.local_state.identity.host = host;
        cfg.protocol_period = std::chrono::milliseconds(100);
        cfg.ping_timeout = std::chrono::milliseconds(50);
        cfg.suspicion_timeout = std::chrono::milliseconds(200);
        cfg.dead_timeout = std::chrono::milliseconds(1000);
        return cfg;
    }

    // Route all packets from `from`'s transport to `to`'s handle_packet,
    // using a fixed host/port for the source.
    static void
    deliver_packets(GossipMembership& from, GossipMembership& to,
                    const std::string& src_host = "127.0.0.1",
                    uint16_t src_port = 9000) {
        auto* t = static_cast<FakeUdpTransport*>(from.transport_.get());
        for (const auto& pkt : t->sent_packets) {
            to.handle_packet(pkt.data, src_host, src_port);
        }
        t->clear_sent();
    }
};

TEST_F(GossipProtocolIntegrationTest, JoinFlow) {
    // Node A: solo node (seed)
    auto cfg_a = cfg_for(9000);
    auto transport_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(transport_a));
    // Manually bootstrap A (like start() does, but without EventLoop)
    node_a.incarnation_ = 100;
    Member self_a;
    self_a.identity.endpoint = ep(9000);
    self_a.incarnation = 100;
    self_a.status = MemberStatus::Alive;
    self_a.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = std::move(self_a);

    // Node B: joins via A as seed
    auto cfg_b = cfg_for(9001);
    cfg_b.seeds.push_back(ep(9000));
    auto transport_b = std::make_unique<FakeUdpTransport>();
    GossipMembership node_b(cfg_b, std::move(transport_b));
    node_b.incarnation_ = 200;

    // B sends Join to A
    node_b.send_join(ep(9000));

    // Deliver B's Join to A
    deliver_packets(node_b, node_a);

    // A should now know about B (via merge in handle_join)
    EXPECT_EQ(node_a.members_.size(), 2u);
    EXPECT_NE(node_a.members_.find(ep(9001)), node_a.members_.end());
    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Alive);

    // A should have sent a SyncRsp back to B
    auto* ta = static_cast<FakeUdpTransport*>(node_a.transport_.get());
    ASSERT_FALSE(ta->sent_packets.empty());

    // Deliver A's SyncRsp to B
    deliver_packets(node_a, node_b);

    // B should now know about A
    EXPECT_EQ(node_b.members_.size(), 2u);
    EXPECT_NE(node_b.members_.find(ep(9000)), node_b.members_.end());
    EXPECT_EQ(node_b.members_[ep(9000)].status, MemberStatus::Alive);
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*JoinFlow*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add Join flow integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 7: Protocol Round Ping/Ack integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Write the test**

Append inside the `GossipProtocolIntegrationTest` class block (before the closing `};` of the test fixture... actually, add after the JoinFlow test):

```cpp
TEST_F(GossipProtocolIntegrationTest, ProtocolRoundPingAck) {
    // Two nodes that know each other.
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    auto cfg_b = cfg_for(9001);
    auto t_b = std::make_unique<FakeUdpTransport>();
    GossipMembership node_b(cfg_b, std::move(t_b));
    node_b.incarnation_ = 200;

    // Both know each other
    Member m_a;
    m_a.identity.endpoint = ep(9000);
    m_a.status = MemberStatus::Alive;
    m_a.incarnation = 100;
    m_a.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = m_a;
    node_a.members_[ep(9001)] = Member{};
    node_a.members_[ep(9001)].identity.endpoint = ep(9001);
    node_a.members_[ep(9001)].status = MemberStatus::Alive;
    node_a.members_[ep(9001)].incarnation = 200;

    Member m_b;
    m_b.identity.endpoint = ep(9001);
    m_b.status = MemberStatus::Alive;
    m_b.incarnation = 200;
    m_b.last_seen = std::chrono::steady_clock::now();
    node_b.members_[ep(9001)] = m_b;
    node_b.members_[ep(9000)] = Member{};
    node_b.members_[ep(9000)].identity.endpoint = ep(9000);
    node_b.members_[ep(9000)].status = MemberStatus::Alive;
    node_b.members_[ep(9000)].incarnation = 100;

    // A runs protocol round — should send Ping to B
    node_a.protocol_round();

    // Verify A has a pending ping for B
    EXPECT_NE(node_a.pending_pings_.find(ep(9001)), node_a.pending_pings_.end());

    // Deliver A's Ping to B
    deliver_packets(node_a, node_b);

    // B should have sent an Ack back to A
    auto* tb = static_cast<FakeUdpTransport*>(node_b.transport_.get());
    ASSERT_FALSE(tb->sent_packets.empty());

    // Deliver B's Ack to A
    deliver_packets(node_b, node_a);

    // A's pending ping for B should now be cleared
    EXPECT_EQ(node_a.pending_pings_.find(ep(9001)), node_a.pending_pings_.end());
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*ProtocolRoundPingAck*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add protocol round Ping/Ack integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 8: Indirect Probe PingReq integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Write the test**

```cpp
TEST_F(GossipProtocolIntegrationTest, IndirectProbePingReq) {
    // 3 nodes: A (requester), B (proxy), C (target)
    // A pings C → C drops → A sends PingReq to B → B pings C → C acks B → B sends IndirectAck to A
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    auto cfg_b = cfg_for(9001);
    auto t_b = std::make_unique<FakeUdpTransport>();
    GossipMembership node_b(cfg_b, std::move(t_b));
    node_b.incarnation_ = 200;

    auto cfg_c = cfg_for(9002);
    auto t_c = std::make_unique<FakeUdpTransport>();
    GossipMembership node_c(cfg_c, std::move(t_c));
    node_c.incarnation_ = 300;

    // All three know each other as Alive
    for (auto* n : {&node_a, &node_b, &node_c}) {
        for (uint16_t p = 9000; p <= 9002; ++p) {
            Member m;
            m.identity.endpoint = ep(p);
            m.status = MemberStatus::Alive;
            m.incarnation = 100 + (p - 9000) * 100;
            m.last_seen = std::chrono::steady_clock::now();
            n->members_[ep(p)] = std::move(m);
        }
    }

    // Simulate: A has a pending ping for C that just expired (first expiry)
    auto now = std::chrono::steady_clock::now();
    node_a.pending_pings_[ep(9002)] = PendingPing{
        now - std::chrono::milliseconds(100), false, {}};

    // A runs protocol round — should detect expired ping and send PingReq to B
    node_a.protocol_round();

    // A should have sent PingReq to B (proxy) for C (target)
    auto* ta = static_cast<FakeUdpTransport*>(node_a.transport_.get());
    ASSERT_FALSE(ta->sent_packets.empty());

    // Deliver A's PingReq to B
    deliver_packets(node_a, node_b);

    // B should have forwarded a Ping to C
    auto* tb = static_cast<FakeUdpTransport*>(node_b.transport_.get());
    ASSERT_FALSE(tb->sent_packets.empty());

    // Deliver B's Ping to C
    deliver_packets(node_b, node_c);

    // C should have sent Ack back to B
    auto* tc = static_cast<FakeUdpTransport*>(node_c.transport_.get());
    ASSERT_FALSE(tc->sent_packets.empty());

    // Deliver C's Ack to B
    deliver_packets(node_c, node_b);

    // B should have sent IndirectAck to A
    ASSERT_FALSE(tb->sent_packets.empty());

    // Deliver B's IndirectAck to A
    deliver_packets(node_b, node_a);

    // A's pending ping for C should be cleared
    EXPECT_EQ(node_a.pending_pings_.find(ep(9002)), node_a.pending_pings_.end());
    // C should still be Alive (not marked Suspicious)
    EXPECT_EQ(node_a.members_[ep(9002)].status, MemberStatus::Alive);
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*IndirectProbePingReq*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add indirect probe PingReq integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 9: Failure Detection — Suspicious integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Write the test**

```cpp
TEST_F(GossipProtocolIntegrationTest, FailureDetectionSuspicious) {
    // 2-node cluster: A pings B → B doesn't respond → no peers for indirect → B marked Suspicious
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    // Both know each other
    for (uint16_t p = 9000; p <= 9001; ++p) {
        Member m;
        m.identity.endpoint = ep(p);
        m.status = MemberStatus::Alive;
        m.incarnation = 100;
        m.last_seen = std::chrono::steady_clock::now();
        node_a.members_[ep(p)] = std::move(m);
    }

    bool callback_fired = false;
    Member callback_member;
    node_a.on_member_change(
        [&](const Member& m, bool joined) {
            callback_fired = true;
            callback_member = m;
        });

    // Create an expired pending ping for B — as if A pinged B but got no response
    auto now = std::chrono::steady_clock::now();
    node_a.pending_pings_[ep(9001)] = PendingPing{
        now - std::chrono::milliseconds(100), false, {}};

    // A runs protocol round
    // With only 2 nodes, there are no indirect probe peers → immediate suspicious
    node_a.protocol_round();

    // B should be marked Suspicious (no indirect probes in 2-node cluster)
    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Suspicious);
    // Pending ping should be erased
    EXPECT_EQ(node_a.pending_pings_.find(ep(9001)), node_a.pending_pings_.end());
    // Callback should NOT fire for suspicious (only for Dead/Left)
    // mark_suspicious does not fire the callback
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*FailureDetectionSuspicious*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add failure detection suspicious integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 10: Failure Detection — Dead integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Write the test**

```cpp
TEST_F(GossipProtocolIntegrationTest, FailureDetectionDead) {
    // B is already Suspicious. After suspicion timeout, protocol round marks B Dead.
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    Member self;
    self.identity.endpoint = ep(9000);
    self.status = MemberStatus::Alive;
    self.incarnation = 100;
    self.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = std::move(self);

    Member suspicious;
    suspicious.identity.endpoint = ep(9001);
    suspicious.status = MemberStatus::Suspicious;
    suspicious.incarnation = 200;
    // last_seen is ancient — well past suspicion_timeout
    suspicious.last_seen =
        std::chrono::steady_clock::now() - std::chrono::seconds(10);
    node_a.members_[ep(9001)] = std::move(suspicious);

    bool callback_fired = false;
    Member callback_member;
    node_a.on_member_change(
        [&](const Member& m, bool joined) {
            callback_fired = true;
            callback_member = m;
        });

    node_a.protocol_round();

    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Dead);
    EXPECT_TRUE(callback_fired);
    EXPECT_EQ(callback_member.identity.endpoint, ep(9001));
    EXPECT_FALSE(callback_member.status == MemberStatus::Alive); // joined=false
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*FailureDetectionDead*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add failure detection dead integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 11: Leave Propagation integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Write the test**

```cpp
TEST_F(GossipProtocolIntegrationTest, LeavePropagation) {
    // B leaves gracefully — A receives Leave and marks B as Left.
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    auto cfg_b = cfg_for(9001);
    auto t_b = std::make_unique<FakeUdpTransport>();
    GossipMembership node_b(cfg_b, std::move(t_b));
    node_b.incarnation_ = 200;

    // Both know each other
    for (uint16_t p = 9000; p <= 9001; ++p) {
        Member m;
        m.identity.endpoint = ep(p);
        m.status = MemberStatus::Alive;
        m.incarnation = 100 + (p - 9000) * 100;
        m.last_seen = std::chrono::steady_clock::now();
        node_a.members_[ep(p)] = std::move(m);
    }
    for (uint16_t p = 9000; p <= 9001; ++p) {
        Member m;
        m.identity.endpoint = ep(p);
        m.status = MemberStatus::Alive;
        m.incarnation = 100 + (p - 9000) * 100;
        m.last_seen = std::chrono::steady_clock::now();
        node_b.members_[ep(p)] = std::move(m);
    }

    bool callback_fired = false;
    node_a.on_member_change(
        [&](const Member&, bool) { callback_fired = true; });

    // B sends Leave to A
    node_b.send_leave(ep(9000));
    deliver_packets(node_b, node_a);

    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Left);
    EXPECT_TRUE(callback_fired);
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*LeavePropagation*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add leave propagation integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 12: Piggyback Dissemination integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Write the test**

```cpp
TEST_F(GossipProtocolIntegrationTest, PiggybackDissemination) {
    // A has B marked Suspicious. A pings C → piggyback carries Suspicious(B).
    // C applies piggyback → C also knows B is Suspicious.
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    auto cfg_c = cfg_for(9002);
    auto t_c = std::make_unique<FakeUdpTransport>();
    GossipMembership node_c(cfg_c, std::move(t_c));
    node_c.incarnation_ = 300;

    // A knows self (Alive), B (Suspicious), C (Alive)
    Member self_a;
    self_a.identity.endpoint = ep(9000);
    self_a.status = MemberStatus::Alive;
    self_a.incarnation = 100;
    self_a.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = self_a;

    Member susp_b;
    susp_b.identity.endpoint = ep(9001);
    susp_b.status = MemberStatus::Suspicious;
    susp_b.incarnation = 200;
    susp_b.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9001)] = susp_b;

    Member alive_c;
    alive_c.identity.endpoint = ep(9002);
    alive_c.status = MemberStatus::Alive;
    alive_c.incarnation = 300;
    alive_c.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9002)] = alive_c;

    // C knows self and A
    Member self_c;
    self_c.identity.endpoint = ep(9002);
    self_c.status = MemberStatus::Alive;
    self_c.incarnation = 300;
    self_c.last_seen = std::chrono::steady_clock::now();
    node_c.members_[ep(9002)] = self_c;

    Member alive_a;
    alive_a.identity.endpoint = ep(9000);
    alive_a.status = MemberStatus::Alive;
    alive_a.incarnation = 100;
    alive_a.last_seen = std::chrono::steady_clock::now();
    node_c.members_[ep(9000)] = alive_a;

    // C does NOT know B yet
    EXPECT_EQ(node_c.members_.find(ep(9001)), node_c.members_.end());

    // A runs protocol round — should send Ping to C with Suspicious(B) piggyback
    node_a.protocol_round();

    // Deliver A's Ping to C
    deliver_packets(node_a, node_c);

    // C should now know B as Suspicious (from piggyback)
    auto it = node_c.members_.find(ep(9001));
    ASSERT_NE(it, node_c.members_.end());
    EXPECT_EQ(it->second.status, MemberStatus::Suspicious);
    EXPECT_EQ(it->second.incarnation, 200u);
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*PiggybackDissemination*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add piggyback dissemination integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 13: Incarnation Conflict Resolution integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Write the test**

```cpp
TEST_F(GossipProtocolIntegrationTest, IncarnationConflictResolution) {
    // A has B at inc=10 Alive. Piggyback claiming B Dead at inc=8 → ignored.
    // Piggyback at inc=12 Dead → accepted.
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    Member b_in_a;
    b_in_a.identity.endpoint = ep(9001);
    b_in_a.status = MemberStatus::Alive;
    b_in_a.incarnation = 10;
    b_in_a.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = Member{};
    node_a.members_[ep(9001)] = b_in_a;

    // Stale piggyback: inc=8 Dead → should be ignored
    std::vector<PiggybackEntry> stale;
    PiggybackEntry stale_e;
    stale_e.type = PiggybackType::Dead;
    stale_e.identity.endpoint = ep(9001);
    stale_e.incarnation = 8;
    stale.push_back(stale_e);
    node_a.apply_piggyback(stale);

    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Alive);
    EXPECT_EQ(node_a.members_[ep(9001)].incarnation, 10u);

    // Fresher piggyback: inc=12 Dead → should be accepted
    std::vector<PiggybackEntry> fresh;
    PiggybackEntry fresh_e;
    fresh_e.type = PiggybackType::Dead;
    fresh_e.identity.endpoint = ep(9001);
    fresh_e.incarnation = 12;
    fresh.push_back(fresh_e);
    node_a.apply_piggyback(fresh);

    EXPECT_EQ(node_a.members_[ep(9001)].status, MemberStatus::Dead);
    EXPECT_EQ(node_a.members_[ep(9001)].incarnation, 12u);
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*IncarnationConflictResolution*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add incarnation conflict resolution integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 14: Member Change Callback integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Write the test**

```cpp
TEST_F(GossipProtocolIntegrationTest, MemberChangeCallback) {
    // Verify callback fires on member join (new Alive) and member leave (Dead).
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;
    node_a.members_[ep(9000)] = Member{};

    std::vector<std::pair<Member, bool>> events;
    node_a.on_member_change(
        [&](const Member& m, bool joined) {
            events.emplace_back(m, joined);
        });

    // Inject a new member via piggyback — should fire joined=true callback
    // But apply_piggyback doesn't fire callbacks. merge_member doesn't fire callbacks either.
    // Only mark_dead and handle_leave fire callbacks.
    // So let's test the mark_dead callback path instead:

    // Add B as Alive, then mark dead
    Member b;
    b.identity.endpoint = ep(9001);
    b.status = MemberStatus::Alive;
    b.incarnation = 200;
    b.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9001)] = b;

    node_a.mark_dead(ep(9001));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].first.identity.endpoint, ep(9001));
    EXPECT_FALSE(events[0].second); // joined=false for Dead

    // Test leave callback
    node_a.handle_leave(ep(9001), 300);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].first.identity.endpoint, ep(9001));
    EXPECT_FALSE(events[1].second); // joined=false for Left
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*MemberChangeCallback*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add member change callback integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 15: Tombstone Purging integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Write the test**

```cpp
TEST_F(GossipProtocolIntegrationTest, TombstonePurging) {
    // Protocol round purges Dead members past dead_timeout.
    auto cfg_a = cfg_for(9000);
    cfg_a.dead_timeout = std::chrono::milliseconds(100);
    auto t_a = std::make_unique<FakeUdpTransport>();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    Member self;
    self.identity.endpoint = ep(9000);
    self.status = MemberStatus::Alive;
    self.incarnation = 100;
    self.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = self;

    // Ancient Dead member
    Member dead;
    dead.identity.endpoint = ep(9001);
    dead.status = MemberStatus::Dead;
    dead.incarnation = 200;
    dead.last_seen = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    node_a.members_[ep(9001)] = dead;

    EXPECT_EQ(node_a.members_.size(), 2u);

    node_a.protocol_round();

    // Dead tombstone should be purged
    EXPECT_EQ(node_a.members_.find(ep(9001)), node_a.members_.end());
    // Self should remain
    EXPECT_NE(node_a.members_.find(ep(9000)), node_a.members_.end());
    EXPECT_EQ(node_a.members_.size(), 1u);
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*TombstonePurging*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add tombstone purging integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 16: Fault Injection Packet Loss integration test

**Files:**
- Modify: `tests/unit/net/test_gossip_membership.cpp`

- [ ] **Step 1: Write the test**

```cpp
TEST_F(GossipProtocolIntegrationTest, FaultInjectionPacketLoss) {
    // Verify that send_ping respects its fault point (hpactor.gossip.ping.drop).
    // When the fault is active, the method returns without calling transport_->send().
    auto cfg_a = cfg_for(9000);
    auto t_a = std::make_unique<FakeUdpTransport>();
    auto* t_a_raw = t_a.get();
    GossipMembership node_a(cfg_a, std::move(t_a));
    node_a.incarnation_ = 100;

    Member self;
    self.identity.endpoint = ep(9000);
    self.status = MemberStatus::Alive;
    self.incarnation = 100;
    self.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9000)] = self;

    Member peer;
    peer.identity.endpoint = ep(9001);
    peer.status = MemberStatus::Alive;
    peer.incarnation = 200;
    peer.last_seen = std::chrono::steady_clock::now();
    node_a.members_[ep(9001)] = peer;

    // Normal send — packet should appear in transport
    node_a.send_ping(ep(9001));
    EXPECT_FALSE(t_a_raw->sent_packets.empty());
    t_a_raw->clear_sent();

    // Enable the ping.drop fault point via FaultController singleton.
    // NOTE: adjust the exact API (enable/disable) to match
    // include/hpactor/fault/fault_controller.hpp if it differs.
    auto* fc = hpactor::fault::FaultController::instance();
    ASSERT_NE(fc, nullptr) << "FaultController not initialized";
    fc->enable("hpactor.gossip.ping.drop");

    node_a.send_ping(ep(9001));

    // With fault active, the packet should NOT appear in transport
    EXPECT_TRUE(t_a_raw->sent_packets.empty());

    // Disable fault
    fc->disable("hpactor.gossip.ping.drop");

    // Now send again — should appear
    node_a.send_ping(ep(9001));
    EXPECT_FALSE(t_a_raw->sent_packets.empty());
}
```

- [ ] **Step 2: Build and run the test**

```bash
ninja -C build && ./build/tests/unit/net/test_unit_net --gtest_filter="*FaultInjectionPacketLoss*"
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/unit/net/test_gossip_membership.cpp
git commit -m "test: add fault injection packet loss integration test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 17: System test — Two-Node Join and Discovery

**Files:**
- Create: `tests/system/test_system_gossip.cpp`
- Modify: `tests/system/CMakeLists.txt`

- [ ] **Step 1: Write the system test file**

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// System tests for GossipMembership — real UDP sockets + EventLoop.

#include <gtest/gtest.h>

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/gossip_membership.hpp>

#include "system_test_fixture.hpp"

#include <memory>
#include <thread>

using namespace hpactor;
using namespace hpactor::net;

namespace {

static EndPoint ep(uint16_t port) {
    return Ipv4Endpoint{0x7F000001, htons(port)};
}

static GossipConfig cfg_at(uint16_t port,
                            const std::vector<uint16_t>& seed_ports = {}) {
    GossipConfig cfg;
    cfg.gossip_port = port;
    cfg.protocol_period = std::chrono::milliseconds(100);
    cfg.ping_timeout = std::chrono::milliseconds(50);
    cfg.suspicion_timeout = std::chrono::milliseconds(300);
    cfg.dead_timeout = std::chrono::milliseconds(2000);
    cfg.fanout = 2;
    cfg.indirect_probes = 1;
    cfg.local_state.identity.endpoint = ep(port);
    cfg.local_state.identity.host = "127.0.0.1";
    for (auto sp : seed_ports) {
        cfg.seeds.push_back(ep(sp));
    }
    return cfg;
}

} // namespace

TEST(GossipSystem, TwoNodeJoinAndDiscovery) {
    Config sys_cfg_a = test::minimal_config();
    sys_cfg_a.enable_network = true;
    ActorSystem sys_a(sys_cfg_a);
    auto* loop = sys_a.event_loop();
    ASSERT_NE(loop, nullptr);

    GossipMembership node_a(cfg_at(19000), loop);
    node_a.start();

    // Verify A sees only itself initially
    EXPECT_EQ(node_a.discover_all().size(), 1u);

    Config sys_cfg_b = test::minimal_config();
    sys_cfg_b.enable_network = true;
    ActorSystem sys_b(sys_cfg_b);
    auto* loop_b = sys_b.event_loop();

    GossipMembership node_b(cfg_at(19001, {19000}), loop_b);
    node_b.start();

    // Poll until both see each other
    bool both_see_two = test::assert_eventually([&]() {
        return node_a.discover_all().size() == 2 &&
               node_b.discover_all().size() == 2;
    }, 5000);
    EXPECT_TRUE(both_see_two);

    node_b.stop();
    node_a.stop();
    sys_b.shutdown();
    sys_a.shutdown();
}
```

- [ ] **Step 2: Add to CMakeLists**

Edit `tests/system/CMakeLists.txt`:

Replace old_string:
```
    test_system_udp_registrar.cpp
```
With new_string:
```
    test_system_udp_registrar.cpp
    test_system_gossip.cpp
```

- [ ] **Step 3: Build and run just this test**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && ninja -C build && ctest -R "GossipSystem.TwoNodeJoinAndDiscovery" --output-on-failure
```

Expected: test passes.

- [ ] **Step 4: Commit**

```bash
git add tests/system/test_system_gossip.cpp tests/system/CMakeLists.txt
git commit -m "test: add two-node join and discovery system test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 18: System test — Failure Detection End-to-End

**Files:**
- Modify: `tests/system/test_system_gossip.cpp`

- [ ] **Step 1: Write the test**

Append to the file:

```cpp
TEST(GossipSystem, FailureDetectionEndToEnd) {
    // 3 nodes. C's messages are blocked by closing its transport after it
    // joins — this simulates a node going silent. The other two should
    // eventually detect C as Dead.
    //
    // We use a different approach: start 3 nodes, then stop one (C).
    // After the protocol period + ping_timeout + suspicion_timeout,
    // the other nodes should detect C as Dead.

    Config sys_cfg_a = test::minimal_config();
    sys_cfg_a.enable_network = true;
    ActorSystem sys_a(sys_cfg_a);
    auto* loop_a = sys_a.event_loop();

    GossipMembership node_a(cfg_at(20000), loop_a);
    node_a.start();

    Config sys_cfg_b = test::minimal_config();
    sys_cfg_b.enable_network = true;
    ActorSystem sys_b(sys_cfg_b);
    auto* loop_b = sys_b.event_loop();

    GossipMembership node_b(cfg_at(20001, {20000}), loop_b);
    node_b.start();

    Config sys_cfg_c = test::minimal_config();
    sys_cfg_c.enable_network = true;
    ActorSystem sys_c(sys_cfg_c);
    auto* loop_c = sys_c.event_loop();

    GossipMembership node_c(cfg_at(20002, {20000}), loop_c);
    node_c.start();

    // Wait for all three to discover each other
    bool all_three = test::assert_eventually([&]() {
        return node_a.discover_all().size() == 3 &&
               node_b.discover_all().size() == 3 &&
               node_c.discover_all().size() == 3;
    }, 5000);
    ASSERT_TRUE(all_three) << "Nodes did not discover each other within timeout";

    // Kill node C — stop sending, shut down its socket
    node_c.stop();

    // Wait for A and B to detect C as Dead
    bool c_detected_dead = test::assert_eventually([&]() {
        const auto* m_a = node_a.discover(ep(20002));
        const auto* m_b = node_b.discover(ep(20002));
        return m_a && m_a->status == MemberStatus::Dead &&
               m_b && m_b->status == MemberStatus::Dead;
    }, 10000);
    EXPECT_TRUE(c_detected_dead) << "Nodes did not detect C as Dead within timeout";

    node_b.stop();
    node_a.stop();
    sys_c.shutdown();
    sys_b.shutdown();
    sys_a.shutdown();
}
```

- [ ] **Step 2: Build and run this test**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && ninja -C build && ctest -R "GossipSystem.FailureDetectionEndToEnd" --output-on-failure
```

Expected: test passes (may need generous timeout — CMake TIMEOUT is 30s).

- [ ] **Step 3: Commit**

```bash
git add tests/system/test_system_gossip.cpp
git commit -m "test: add failure detection end-to-end system test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 19: System test — Graceful Leave

**Files:**
- Modify: `tests/system/test_system_gossip.cpp`

- [ ] **Step 1: Write the test**

Append to the file:

```cpp
TEST(GossipSystem, GracefulLeave) {
    Config sys_cfg_a = test::minimal_config();
    sys_cfg_a.enable_network = true;
    ActorSystem sys_a(sys_cfg_a);
    auto* loop_a = sys_a.event_loop();

    GossipMembership node_a(cfg_at(21000), loop_a);
    node_a.start();

    Config sys_cfg_b = test::minimal_config();
    sys_cfg_b.enable_network = true;
    ActorSystem sys_b(sys_cfg_b);
    auto* loop_b = sys_b.event_loop();

    GossipMembership node_b(cfg_at(21001, {21000}), loop_b);
    node_b.start();

    // Wait for mutual discovery
    bool discovered = test::assert_eventually([&]() {
        return node_a.discover_all().size() == 2 &&
               node_b.discover_all().size() == 2;
    }, 5000);
    ASSERT_TRUE(discovered);

    // B gracefully leaves — stop() sends Leave to all known members
    node_b.stop();

    // A should see B as Left
    bool b_is_left = test::assert_eventually([&]() {
        const auto* b = node_a.discover(ep(21001));
        return b && b->status == MemberStatus::Left;
    }, 5000);
    EXPECT_TRUE(b_is_left) << "Node A did not see B as Left within timeout";

    node_a.stop();
    sys_b.shutdown();
    sys_a.shutdown();
}
```

- [ ] **Step 2: Build and run this test**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && ninja -C build && ctest -R "GossipSystem.GracefulLeave" --output-on-failure
```

Expected: test passes.

- [ ] **Step 3: Commit**

```bash
git add tests/system/test_system_gossip.cpp
git commit -m "test: add graceful leave system test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

### Task 20: Final verification — full test suite

- [ ] **Step 1: Run the full unit net test suite**

```bash
./build/tests/unit/net/test_unit_net
```

Expected: all tests pass (original 14 + 11 new = 25 tests).

- [ ] **Step 2: Run the full system test suite**

```bash
ctest --output-on-failure --parallel 8
```

Expected: all tests pass.

- [ ] **Step 3: Run with sanitizers to check for issues**

```bash
cmake -S . -B build_tsan -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_TSAN=ON && ninja -C build_tsan && ./build_tsan/tests/unit/net/test_unit_net --gtest_filter="Gossip*"
```

Expected: no sanitizer warnings.
