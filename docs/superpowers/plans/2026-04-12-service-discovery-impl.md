# Phase 5: Actor Service Discovery - Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Implement UDP-based node discovery with static route support and DNS resolution for the HPActor distributed actor framework.

**Architecture:** UDP registrar for broadcast-based discovery, static route configuration with hostname support, DNS resolution with caching, integration with ActorSystem and TcpTransport.

**Tech Stack:** C++20, POSIX sockets (UDP), getaddrinfo for DNS, kqueue/epoll event loop.

---

## Task 1: UdpRegistrar and HostResolver

**Files:**
- Create: `include/hpactor/net/registrar.hpp`
- Create: `src/net/registrar.cpp`
- Modify: `CMakeLists.txt` (add registrar.cpp)
- Test: `tests/net/test_registrar.cpp`

- [ ] **Step 1: Create registrar.hpp with core types**

```cpp
// NodeEndpoint - information about a known node
struct NodeEndpoint {
    NodeId node_id = 0;
    std::string host;        // IP or DNS hostname
    uint16_t tcp_port = 0;   // TCP listening port
    std::chrono::steady_clock::time_point last_seen;
    bool is_static_route = false;
};

// HostResolver - hostname to IP resolution with caching
class HostResolver {
public:
    std::string resolve(const std::string& hostname);
    void resolve_async(const std::string& hostname, std::function<void(std::string)> callback);
    std::string get_cached(const std::string& hostname) const;
    void cache(const std::string& hostname, const std::string& ip, std::chrono::seconds ttl = 300s);
};

// NodeRegistry - registry of known nodes
class NodeRegistry {
public:
    explicit NodeRegistry(const RegistrarConfig& config);
    void upsert_endpoint(NodeEndpoint endpoint);
    bool remove_endpoint(NodeId node_id);
    NodeEndpoint* get(NodeId node_id);
    bool has(NodeId node_id) const;
    std::vector<NodeEndpoint> all() const;
    size_t remove_expired();
};

// UDP Registrar Protocol
enum class RegistrarMessageType : uint8_t {
    NodeAnnounce = 0x01,
    NodeQuery = 0x02,
    NodeResponse = 0x03,
    NodeLeave = 0x04,
    NodeProbe = 0x05,
    NodeProbeAck = 0x06,
};

// UdpRegistrar - UDP-based node discovery
class UdpRegistrar {
public:
    UdpRegistrar(const RegistrarConfig& config, NodeId local_node_id);
    void start();
    void stop();
    void add_static_route(NodeId node_id, const std::string& host, uint16_t port);
    NodeEndpoint* get_endpoint(NodeId node_id);
};
```

- [ ] **Step 2: Create registrar.cpp with implementation**

UDP socket setup, broadcast/unicast, message parsing, DNS resolution via getaddrinfo, expiration of stale nodes.

- [ ] **Step 3: Add registrar.cpp to CMakeLists.txt**

```cmake
src/net/registrar.cpp
```

- [ ] **Step 4: Create test_registrar.cpp**

Test RegistrarConfig defaults, HostResolver cache, NodeRegistry operations.

- [ ] **Step 5: Build and run tests**

Run: `cmake -S . -B build -GNinja && ninja -C build && ctest --output-on-failure`
Expected: test_registrar passes

---

## Task 2: Transport connect(NodeId) Overload

**Files:**
- Modify: `include/hpactor/net/tcp_transport.hpp`
- Modify: `src/net/tcp_transport.cpp`

- [ ] **Step 1: Add connect(NodeId) method declaration**

```cpp
ConnectionPtr connect(NodeId remote_node) override;
```

- [ ] **Step 2: Implement connect(NodeId)**

```cpp
ConnectionPtr TcpTransport::connect(NodeId remote_node_id) {
    // 1. Lookup endpoint in registry
    NodeEndpoint* ep = registry_->get(remote_node_id);
    if (!ep) return nullptr;

    // 2. Resolve hostname to IP (if needed)
    std::string ip = host_resolver_.resolve(ep->host);

    // 3. Connect to resolved IP:port
    return connect(remote_node_id, ip, ep->tcp_port);
}
```

- [ ] **Step 3: Add HostResolver member to TcpTransport**

```cpp
HostResolver host_resolver_;
```

- [ ] **Step 4: Build and test**

Expected: TcpTransport accepts NodeId and resolves via registry.

---

## Task 3: ActorSystem Network Integration

**Files:**
- Modify: `include/hpactor/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add network config to ActorSystem::Config**

```cpp
struct Config {
    bool enable_network = false;
    uint16_t tcp_port = 0;
    uint16_t udp_port = 5353;
    net::TlsConfig tls;
    net::PoolConfig pool;
    net::RegistrarConfig registrar;
};
```

- [ ] **Step 2: Add network members to ActorSystem**

```cpp
std::unique_ptr<net::TcpTransport> transport_;
std::unique_ptr<net::UdpRegistrar> registrar_;
std::unique_ptr<net::EventLoop> network_loop_;
std::thread network_thread_;
```

- [ ] **Step 3: Add accessors**

```cpp
net::Transport* transport() { return transport_.get(); }
net::UdpRegistrar* registrar() { return registrar_.get(); }
```

- [ ] **Step 4: Initialize network in constructor**

```cpp
if (config.enable_network) {
    network_loop_ = std::make_unique<net::EventLoop>();
    registrar_ = std::make_unique<net::UdpRegistrar>(config.registrar, node_id_);
    registrar_->start();
    transport_ = std::make_unique<net::TcpTransport>(node_id_, config.tls, config.pool, nullptr);
    if (config.tcp_port > 0) {
        transport_->listen(config.tcp_port);
    }
    network_thread_ = std::thread([this]() {
        while (network_loop_->wait(100) >= 0) { }
    });
}
```

- [ ] **Step 5: Cleanup in destructor**

Stop network thread, close transport, stop registrar.

- [ ] **Step 6: Build and test**

Expected: ActorSystem with enable_network=true creates transport and registrar.

---

## Task 4: ActorProxy send() Implementation

**Files:**
- Modify: `src/ref/actor_proxy.cpp`

- [ ] **Step 1: Implement ActorProxy::send()**

```cpp
void ActorProxy::send(const ActorAddress& target, MessageVariant msg) {
    // Determine TypeTag using std::visit
    TypeTag tag = std::visit([](const auto& m) -> TypeTag {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, down_msg>) return TypeTag::DownMsg;
        else if constexpr (std::is_same_v<T, exit_msg>) return TypeTag::ExitMsg;
        else if constexpr (std::is_same_v<T, link_msg>) return TypeTag::LinkMsg;
        else if constexpr (std::is_same_v<T, unlink_msg>) return TypeTag::UnlinkMsg;
        else return TypeTag::User;
    }, msg);

    // Serialize message
    DefaultSerializer serializer;
    bytes payload = serializer.encode(tag, msg);

    // Create frame
    net::Frame frame;
    frame.sender = address_;
    frame.receiver = target;
    frame.message_id = MessageId::generate().value();
    frame.payload = std::move(payload);

    // Send via transport
    transport_->send(target, frame.encode());
}
```

- [ ] **Step 2: Build and test**

Expected: ActorProxy::send() compiles and works with remote actors.

---

## Task 5: Service Discovery Tests

**Files:**
- Create: `tests/net/test_service_discovery.cpp` (optional, if needed)

- [ ] **Step 1: Run existing registrar tests**

Run: `./build/tests/test_registrar`
Expected: All tests pass

- [ ] **Step 2: Run all tests**

Run: `ctest --output-on-failure`
Expected: 31 tests pass

---

## Verification

```bash
# Build
cmake -S . -B build -GNinja && ninja -C build

# Tests
ctest --output-on-failure

# Manual test (two terminals):
# Terminal 1: ./build/examples/net_node --node-id=1 --udp-port=5353
# Terminal 2: ./build/examples/net_node --node-id=2 --udp-port=5354
```

---

## Out of Scope

- Remote actor spawn (Phase 6)
- Actor migration
- Distributed supervision
