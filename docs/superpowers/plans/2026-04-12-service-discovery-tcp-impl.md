# Phase 5: Service Discovery - TCP Server/Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the UDP broadcast-based registrar to a dual-mode TCP/UDP registrar with server/client failover, matching the refined design spec.

**Architecture:** TCP-based registration with persistent connections for keepalive. UDP unicast for lightweight resolution queries. Server mode binds TCP port 5353; client mode connects to existing server. Automatic failover via port binding race when server dies.

**Tech Stack:** C++20, POSIX sockets (TCP + UDP), getaddrinfo for DNS, kqueue/epoll event loop.

---

## File Changes Overview

| File | Change |
|------|--------|
| `include/hpactor/net/registrar.hpp` | Refactor: add AcceptorInfo, dual-mode Registrar, TCP server/client |
| `src/net/registrar.cpp` | Full rewrite: TCP server/client, failover, UDP resolution |
| `tests/net/test_registrar.cpp` | Update tests for new protocol and types |

---

## Task 1: Update Registrar Types

**Files:**
- Modify: `include/hpactor/net/registrar.hpp:22-46`
- Test: `tests/net/test_registrar.cpp`

- [ ] **Step 1: Add AcceptorInfo struct**

```cpp
struct AcceptorInfo {
    uint16_t port = 0;
    uint8_t handshake_version = 0;
    uint8_t protocol_version = 0;
    bool tls_required = false;
};
```

- [ ] **Step 2: Add tcp_port to RegistrarConfig**

```cpp
struct RegistrarConfig {
    uint16_t udp_port = 5353;
    uint16_t tcp_port = 5353;
    std::chrono::milliseconds heartbeat_interval{5000};
    std::chrono::milliseconds expiration_timeout{15000};
    std::chrono::milliseconds probe_interval{30000};
    std::vector<StaticRouteConfig> static_routes;
    bool disable_server = false;
};
```

- [ ] **Step 3: Add acceptors to NodeEndpoint**

```cpp
struct NodeEndpoint {
    NodeId node_id = 0;
    std::string host;
    uint16_t tcp_port = 0;
    bool is_static_route = false;
    std::vector<AcceptorInfo> acceptors;
    std::chrono::steady_clock::time_point last_seen;
};
```

- [ ] **Step 4: Remove old UDP message types (NodeAnnounce, NodeProbe, NodeProbeAck) and add TCP message types**

```cpp
enum class RegistrarMessageType : uint8_t {
    // TCP messages (server/client registration)
    Register = 0x01,
    Heartbeat = 0x02,
    NodeJoin = 0x03,
    NodeLeave = 0x04,
    Accept = 0x05,
    Error = 0x06,
    // UDP messages (resolution)
    ResolveQuery = 0x10,
    ResolveResponse = 0x11,
};
```

- [ ] **Step 5: Update test_registrar.cpp to test new types**

```cpp
// Test AcceptorInfo defaults
AcceptorInfo info;
assert(info.port == 0);
assert(info.tls_required == false);

// Test NodeEndpoint with acceptors
NodeEndpoint ep;
ep.node_id = 1;
ep.host = "localhost";
ep.tcp_port = 9000;
ep.acceptors.push_back({9000, 1, 1, false});
assert(ep.acceptors.size() == 1);
assert(ep.acceptors[0].port == 9000);

// Test RegistrarConfig tcp_port
RegistrarConfig config;
assert(config.tcp_port == 5353);
assert(config.disable_server == false);
```

- [ ] **Step 6: Build and run tests**

Run: `ninja -C build && ./build/tests/test_registrar`
Expected: PASS with new type tests

---

## Task 2: Refactor NodeRegistry for Acceptors

**Files:**
- Modify: `include/hpactor/net/registrar.hpp:84-110`
- Modify: `src/net/registrar.cpp` (upsert_endpoint)
- Test: `tests/net/test_registrar.cpp`

- [ ] **Step 1: Update upsert_endpoint signature to accept full NodeEndpoint**

```cpp
void NodeRegistry::upsert_endpoint(NodeEndpoint endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    endpoint.last_seen = std::chrono::steady_clock::now();
    endpoints_[endpoint.node_id] = endpoint;
}
```

- [ ] **Step 2: Add test for acceptor persistence in registry**

```cpp
NodeRegistry registry(reg_config);
NodeEndpoint ep;
ep.node_id = 42;
ep.host = "192.168.1.100";
ep.tcp_port = 9001;
ep.acceptors.push_back({9001, 1, 1, false});
registry.upsert_endpoint(ep);

NodeEndpoint* found = registry.get(42);
assert(found != nullptr);
assert(found->acceptors.size() == 1);
assert(found->acceptors[0].port == 9001);
```

- [ ] **Step 3: Build and run tests**

Run: `ninja -C build && ./build/tests/test_registrar`
Expected: PASS

---

## Task 3: Implement TCP Server Mode

**Files:**
- Modify: `include/hpactor/net/registrar.hpp` (add Server class)
- Create: `src/net/registrar_server.cpp` (new file)
- Modify: `CMakeLists.txt` (add registrar_server.cpp)

- [ ] **Step 0: Define TCP Message Framing**

TCP is a stream protocol - messages must be framed with length prefixes.

```cpp
// TCP Frame format: [Magic: 4][Version: 1][Type: 1][Length: 4][Payload: N]
constexpr uint32_t TcpRegistrarMagic = 0x48505243;  // "HPRC"
constexpr uint8_t TcpRegistrarVersion = 0x01;
constexpr size_t TcpHeaderSize = 10;

// TCP Message types (different from UDP types)
enum class TcpMessageType : uint8_t {
    Register = 0x01,
    Heartbeat = 0x02,
    NodeJoin = 0x03,
    NodeLeave = 0x04,
    Accept = 0x05,
    Error = 0x06,
};

// Register payload: [NodeId: 4][HostLen: 1][Host: N][Port: 2][AcceptorCount: 1][Acceptors...]
// Acceptor: [Port: 2][HandshakeVer: 1][ProtocolVer: 1][Tls: 1]

// Error payload: [ErrorCode: 1]
enum class RegistrarError : uint8_t {
    None = 0,
    NameTaken = 1,
    InvalidMessage = 2,
};
```

- [ ] **Step 1: Define RegistrarServer class in registrar.hpp**

```cpp
class RegistrarServer {
public:
    RegistrarServer(const RegistrarConfig& config, NodeId local_node_id);
    ~RegistrarServer();

    // Start TCP server and UDP listener
    void start();
    void stop();

    // Get registry for reading
    NodeRegistry* registry() { return &registry_; }

    // Handle incoming TCP connection
    void handle_accept(int client_fd);

    // Broadcast event to all connected clients
    void broadcast_node_joined(NodeId node_id, const NodeEndpoint& ep);
    void broadcast_node_left(NodeId node_id);

private:
    void accept_loop();
    void handle_tcp_message(int client_fd, const bytes& data);

    RegistrarConfig config_;
    NodeId local_node_id_;
    NodeRegistry registry_;

    int tcp_socket_ = -1;
    int udp_socket_ = -1;
    std::atomic<bool> running_{false};

    // Connected clients (node_id -> fd)
    std::unordered_map<NodeId, int> clients_;
    std::mutex clients_mutex_;

    std::thread accept_thread_;
};
```

- [ ] **Step 2: Implement RegistrarServer::start()**

**TCP/UDP Port Separation:** TCP uses `config_.tcp_port`, UDP uses `config_.udp_port`.

```cpp
void RegistrarServer::start() {
    // Create TCP socket
    tcp_socket_ = socket(AF_INET, SOCK_STREAM, 0);

    // Allow address reuse
    int reuse = 1;
    setsockopt(tcp_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Bind TCP to tcp_port
    struct sockaddr_in tcp_addr;
    memset(&tcp_addr, 0, sizeof(tcp_addr));
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_addr.s_addr = INADDR_ANY;
    tcp_addr.sin_port = htons(config_.tcp_port);

    if (bind(tcp_socket_, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr)) < 0) {
        close(tcp_socket_);
        tcp_socket_ = -1;
        return;
    }

    listen(tcp_socket_, 5);

    // Create UDP socket for resolution - use udp_port
    udp_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port = htons(config_.udp_port);
    bind(udp_socket_, (struct sockaddr*)&udp_addr, sizeof(udp_addr));

    running_.store(true);
    accept_thread_ = std::thread(&RegistrarServer::accept_loop, this);
}
```

- [ ] **Step 3: Implement TCP accept loop**

```cpp
void RegistrarServer::accept_loop() {
    while (running_.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(tcp_socket_, &read_fds);

        struct timeval tv = {1, 0}; // 1 second timeout
        int ret = select(tcp_socket_ + 1, &read_fds, nullptr, nullptr, &tv);

        if (ret > 0 && FD_ISSET(tcp_socket_, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(tcp_socket_, (struct sockaddr*)&client_addr, &len);
            if (client_fd >= 0) {
                handle_accept(client_fd);
            }
        }
    }
}
```

- [ ] **Step 4: Implement message handling (Register, Heartbeat)**

Parse incoming TCP messages and update registry accordingly.

- [ ] **Step 5: Add registrar_server.cpp to CMakeLists.txt**

```cmake
src/net/registrar_server.cpp
```

- [ ] **Step 6: Build**

Run: `ninja -C build`
Expected: Compiles with stub implementation

---

## Task 4: Implement TCP Client Mode

**Files:**
- Modify: `include/hpactor/net/registrar.hpp` (add RegistrarClient class)
- Create: `src/net/registrar_client.cpp` (new file)
- Modify: `CMakeLists.txt`

**Registry Ownership Note:** `RegistrarClient` does NOT own its own registry. It receives `NodeJoin`/`NodeLeave` events from the server and updates the shared `UdpRegistrar`'s registry. The client delegates registry access to `UdpRegistrar`.

- [ ] **Step 1: Define RegistrarClient class**

```cpp
class RegistrarClient {
public:
    RegistrarClient(const RegistrarConfig& config, NodeId local_node_id,
                    NodeId server_node_id, NodeRegistry* shared_registry);
    ~RegistrarClient();

    void start();
    void stop();

    // Reconnect to server (used after disconnection)
    void reconnect();

private:
    void connection_loop();
    void send_registration();
    void send_heartbeat();

    RegistrarConfig config_;
    NodeId local_node_id_;
    NodeId server_node_id_;
    NodeRegistry* shared_registry_;  // Not owned

    int tcp_socket_ = -1;
    int udp_socket_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::thread connection_thread_;
    std::thread heartbeat_thread_;
};
```

- [ ] **Step 2: Implement TCP connection to server**

Connect to server's TCP port, send registration, maintain heartbeat.

- [ ] **Step 3: Implement UDP resolution queries**

Send ResolveQuery to server's UDP port, handle ResolveResponse.

- [ ] **Step 4: Implement reconnection with failover**

**Failover Algorithm:**
1. On TCP disconnect, client enters "reconnecting" state
2. Client tries to bind TCP port 5353 (same port as server)
3. If bind succeeds → become server, start listening for other clients
4. If bind fails (another node won) → try to connect to that node as client
5. All surviving clients race to bind; first to succeed becomes new server

```cpp
void RegistrarClient::reconnect() {
    // Race to become server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.tcp_port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        // Won the race - become server
        close(sock);
        start_server_mode();
    } else {
        // Lost - need to find another server
        // Query UDP broadcast for any known nodes
        close(sock);
        // ... query logic to find existing server
    }
}
```

- [ ] **Step 5: Add registrar_client.cpp to CMakeLists.txt**

```cmake
src/net/registrar_client.cpp
```

---

## Task 5: Unify as Dual-Mode UdpRegistrar

**Files:**
- Modify: `include/hpactor/net/registrar.hpp` (UdpRegistrar class)
- Modify: `src/net/registrar.cpp` (major refactor)
- Test: `tests/net/test_registrar.cpp`

- [ ] **Step 1: Refactor UdpRegistrar to use Server or Client based on bind result**

```cpp
class UdpRegistrar {
public:
    UdpRegistrar(const RegistrarConfig& config, NodeId local_node_id);
    ~UdpRegistrar();

    void start();
    void stop();

    // Query endpoint
    NodeEndpoint* get_endpoint(NodeId node_id);
    std::vector<NodeEndpoint> get_all_endpoints() const;

    // Set callback for node events
    using node_callback = std::function<void(NodeId, bool online)>;
    void set_node_callback(node_callback cb);

    // Handle incoming UDP packet (for resolution)
    void handle_udp_packet(const bytes& data, const std::string& from_host, uint16_t from_port);

private:
    void start_server_mode();
    void start_client_mode();
    void failover();

    RegistrarConfig config_;
    NodeId local_node_id_;

    // Either server or client
    std::unique_ptr<RegistrarServer> server_;
    std::unique_ptr<RegistrarClient> client_;

    node_callback node_callback_;
};
```

- [ ] **Step 2: Implement server/client mode selection**

```cpp
void UdpRegistrar::start() {
    // Try to bind TCP port
    int test_sock = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(test_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.tcp_port);

    if (bind(test_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        // Success - we can be server
        close(test_sock);
        start_server_mode();
    } else {
        // Port taken - be client
        close(test_sock);
        start_client_mode();
    }
}
```

- [ ] **Step 3: Implement UDP packet handling for resolution**

Handle ResolveQuery and ResolveResponse messages.

- [ ] **Step 4: Update tests**

- [ ] **Step 5: Build and run tests**

---

## Task 6: Verify Integration with ActorSystem

**Files:**
- Modify: `include/hpactor/actor_system.hpp` (if needed)
- Modify: `src/actor/actor_system.cpp` (if needed)
- Test: existing tests pass

- [ ] **Step 1: Verify registrar starts in correct mode**

- [ ] **Step 2: Verify TCP registration works**

- [ ] **Step 3: Verify failover on server death**

- [ ] **Step 4: Build all tests**

Run: `cmake -S . -B build -GNinja && ninja -C build && ctest --output-on-failure`

---

## Task 7: Final Verification

- [ ] **Step 1: Run all tests**

```bash
ctest --output-on-failure
```

- [ ] **Step 2: Manual two-node test**

Terminal 1:
```bash
./build/examples/net_node --node-id=1 --tcp-port=9001
```

Terminal 2:
```bash
./build/examples/net_node --node-id=2 --tcp-port=9002
```

Verify: Nodes discover each other via TCP registration.

---

## Out of Scope

- Remote actor spawn (Phase 6)
- Actor migration
- Distributed supervision
- Application discovery (weights, load balancing)
- Event notifications

---

## Dependencies

- Task 1 must complete before Tasks 3-5
- Task 3 (Server) and Task 4 (Client) can be done in parallel
- Task 5 combines Server and Client into unified UdpRegistrar
- Task 6 and 7 verify end-to-end
