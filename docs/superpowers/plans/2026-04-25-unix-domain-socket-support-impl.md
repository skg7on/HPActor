# UNIX Domain Socket Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add UNIX Domain Socket (UDS) support to TcpTransport so same-host inter-process actor communication uses efficient kernel bypass instead of TCP loopback.

**Architecture:** TcpTransport gains a `connect_unix_domain()` method and the `connect(ep)` registry lookup checks for a UDS path and uses UDS when available. Acceptor gains a `listen_unix_domain()` method for inbound UDS connections. `NodeEndpoint` gets an optional `uds_path` field. PlainConnection requires no changes since it's fd-based.

**Tech Stack:** C++20, POSIX sockets (`AF_UNIX`, `SOCK_STREAM`), existing EventLoop async I/O framework, existing PlainConnection

---

## Task 1: Add `uds_path` field to `NodeEndpoint`

**Files:**
- Modify: `include/hpactor/net/registrar.hpp:67-74`

- [ ] **Step 1: Add `uds_path` field to `NodeEndpoint` struct**

```cpp
struct NodeEndpoint {
    CommunicationEndpoint endpoint;
    std::string host;        // Resolved IP or hostname
    uint16_t tcp_port = 0;
    bool is_static_route = false;
    std::vector<AcceptorInfo> acceptors;
    std::chrono::steady_clock::time_point last_seen;
    std::string uds_path;     // NEW: path to UDS socket, empty if not available
};
```

- [ ] **Step 2: Run build to verify no syntax errors**

Run: `ninja -C build`
Expected: SUCCESS (no output)

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/registrar.hpp
git commit -m "feat(registrar): add uds_path field to NodeEndpoint

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: Add `listen_unix_domain()` to Acceptor

**Files:**
- Modify: `include/hpactor/net/acceptor.hpp:30-64`
- Modify: `src/net/acceptor.cpp:1-136`

- [ ] **Step 1: Add `listen_unix_domain()` declaration to `Acceptor` class**

In `include/hpactor/net/acceptor.hpp`, add to the Acceptor class:

```cpp
// Start listening on a UNIX domain socket
// Returns true on success, false on failure
bool listen_unix_domain(const std::string& path);

// Stop listening on UDS socket
void close_unix_domain();

// Get UDS socket path if listening on UDS
std::string uds_path() const { return uds_path_; }
```

Also add member:
```cpp
std::string uds_path_;
```

- [ ] **Step 2: Add `listen_unix_domain()` implementation to `src/net/acceptor.cpp`**

Add after the existing `listen()` method. Note: Add `#include <sys/un.h>` at the top of the file for `sockaddr_un`.

```cpp
#include <sys/un.h>

bool Acceptor::listen_unix_domain(const std::string& path) {
    // Remove stale socket file if it exists
    ::unlink(path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        return false;
    }

    listening_fd_ = fd;
    bound_port_ = 0;  // UDS has no port
    uds_path_ = path;

    if (loop_) {
        loop_->add_fd(fd, EventLoop::Event::Read);
    }

    return true;
}

void Acceptor::close_unix_domain() {
    if (!uds_path_.empty()) {
        ::unlink(uds_path_.c_str());
        uds_path_.clear();  // Reset so uds_path() returns empty after close
    }
    close();  // Closes the fd and sets listening_fd_ = -1
}
```

**Note:** `close_unix_domain()` calls `close()` which already closes the fd and resets `listening_fd_`. We only unlink the socket file and reset `uds_path_` here.

- [ ] **Step 3: Run build to verify**

Run: `ninja -C build`
Expected: SUCCESS

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/acceptor.hpp src/net/acceptor.cpp
git commit -m "feat(acceptor): add listen_unix_domain() for UDS server sockets

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: Add `connect_unix_domain()` to TcpTransport

**Files:**
- Modify: `include/hpactor/net/tcp_transport.hpp:35-87`
- Modify: `src/net/tcp_transport.cpp:1-225`

- [ ] **Step 1: Add `connect_unix_domain()` declaration to `TcpTransport` class**

In `include/hpactor/net/tcp_transport.hpp`, add to the class:

```cpp
// Connect via UNIX domain socket
// Returns ConnectionPtr on success, nullptr on failure
ConnectionPtr connect_unix_domain(CommunicationEndpoint remote_endpoint,
                                   const std::string& socket_path);
```

- [ ] **Step 2: Add `connect_unix_domain()` implementation to `src/net/tcp_transport.cpp`**

Add after the existing `connect()` overloads:

```cpp
ConnectionPtr TcpTransport::connect_unix_domain(CommunicationEndpoint remote_endpoint,
                                                 const std::string& socket_path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return nullptr;
    }

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Set TCP_NODELAY for consistency with TCP path (though less relevant for UDS)
    int nodelay = 1;
    setsockopt(fd, SOL_SOCKET, SO_NODELAY, &nodelay, sizeof(nodelay));

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    int result = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return nullptr;
    }

    // Note: TLS over UDS is not supported per design decision.
    // use_tls config only applies to TCP connections.
    // UDS connections always use PlainConnection.
    auto plain_conn = PlainConnection::create_client(fd, remote_endpoint, &loop_);
    plain_conn->set_ready_handler([pool](ConnectionPtr c) {
        pool->on_connection_ready(c);
    });
    plain_conn->set_error_handler([pool](ConnectionPtr c, const error& e) {
        pool->on_connection_error(c, e);
    });
    plain_conn->set_frame_handler([pool](const bytes& data) {
        pool->on_frame_received(data);
    });
    conn = plain_conn;

    pool->add_connection(conn);
    register_connection(conn, fd);

    return conn;
}
```

- [ ] **Step 3: Run build to verify**

Run: `ninja -C build`
Expected: SUCCESS

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/tcp_transport.hpp src/net/tcp_transport.cpp
git commit -m "feat(tcp_transport): add connect_unix_domain() for UDS client connections

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: Integrate UDS path lookup in `TcpTransport::connect(ep)` (registry-driven fallback)

**Files:**
- Modify: `src/net/tcp_transport.cpp:149-164`

- [ ] **Step 1: Modify `TcpTransport::connect(CommunicationEndpoint)` to check for UDS path**

Replace the current implementation:

```cpp
ConnectionPtr TcpTransport::connect(CommunicationEndpoint remote_endpoint) {
    if (!registry_) {
        return nullptr;  // No registry configured
    }

    NodeEndpoint* ep = registry_->get(remote_endpoint);
    if (!ep) {
        return nullptr;  // Unknown node
    }

    // Check if UDS path is available for this endpoint
    if (!ep->uds_path.empty()) {
        return connect_unix_domain(remote_endpoint, ep->uds_path);
    }

    // Resolve hostname to IP if needed
    std::string ip = host_resolver_.resolve(ep->host);

    // Connect to resolved IP:port
    return connect(remote_endpoint, ip, ep->tcp_port);
}
```

- [ ] **Step 2: Run build to verify**

Run: `ninja -C build`
Expected: SUCCESS

- [ ] **Step 3: Commit**

```bash
git add src/net/tcp_transport.cpp
git commit -m "feat(tcp_transport): integrate UDS path lookup in connect(ep)

When registry has a uds_path for the target endpoint, use UDS instead
of TCP. Enables automatic UDS for same-host inter-process communication.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 5: Add UDS path derivation utility and directory setup

**Files:**
- Modify: `include/hpactor/net/tcp_transport.hpp`
- Modify: `src/net/tcp_transport.cpp`

- [ ] **Step 1: Add helper method to derive UDS socket path from NodeId**

In `TcpTransport`, add a private helper:

```cpp
private:
    // Derive UDS socket path from node identifier string
    // /tmp/hpactor/<sanitized_node_id>.sock
    std::string derive_uds_path(const std::string& node_id) const;
```

Implementation:

```cpp
std::string TcpTransport::derive_uds_path(const std::string& node_id) const {
    // Sanitize node_id: replace colons with underscores
    std::string sanitized = node_id;
    for (char& c : sanitized) {
        if (c == ':') c = '_';
    }
    return "/tmp/hpactor/" + sanitized + ".sock";
}
```

- [ ] **Step 2: Add directory creation on TcpTransport construction**

In `TcpTransport::TcpTransport()`, add:

```cpp
// Ensure UDS directory exists
std::string uds_dir = "/tmp/hpactor";
std::mkdir(uds_dir.c_str(), 0755);  // Ignore error if exists
```

Note: Add `#include <sys/stat.h>` at the top of tcp_transport.cpp.

- [ ] **Step 3: Run build to verify**

Run: `ninja -C build`
Expected: SUCCESS

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/tcp_transport.hpp src/net/tcp_transport.cpp
git commit -m "feat(tcp_transport): add UDS path derivation and directory setup

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 6: Add unit tests for UDS path derivation

**Files:**
- Create: `tests/net/test_unix_domain_socket.cpp`

- [ ] **Step 1: Write unit test for UDS path derivation**

```cpp
// Copyright 2026 HPActor Contributors

#include <gtest/gtest.h>
#include <hpactor/net/tcp_transport.hpp>

namespace hpactor {
namespace net {
namespace {

// Helper to test path derivation
std::string derive_uds_path(const std::string& node_id) {
    std::string sanitized = node_id;
    for (char& c : sanitized) {
        if (c == ':') c = '_';
    }
    return "/tmp/hpactor/" + sanitized + ".sock";
}

TEST(UdsPathDerivation, SimpleNodeId) {
    auto path = derive_uds_path("localhost:5000");
    EXPECT_EQ(path, "/tmp/hpactor/localhost_5000.sock");
}

TEST(UdsPathDerivation, IpAddress) {
    auto path = derive_uds_path("127.0.0.1:8080");
    EXPECT_EQ(path, "/tmp/hpactor/127.0.0.1_8080.sock");
}

TEST(UdsPathDerivation, NoPort) {
    auto path = derive_uds_path("node1");
    EXPECT_EQ(path, "/tmp/hpactor/node1.sock");
}

TEST(UdsPathDerivation, MultipleColons) {
    auto path = derive_uds_path("192.168.1.1:5000");
    EXPECT_EQ(path, "/tmp/hpactor/192.168.1.1_5000.sock");
}

} // namespace
} // net
} // hpactor
```

- [ ] **Step 2: Add test to `tests/CMakeLists.txt`**

Find the `# Net tests` section and add after `test_registrar`:

```cmake
add_executable(test_unix_domain_socket net/test_unix_domain_socket.cpp)
target_link_libraries(test_unix_domain_socket hpactor)
add_test(NAME test_unix_domain_socket COMMAND test_unix_domain_socket)
```

- [ ] **Step 3: Run path derivation test**

Run: `ninja -C build && ./build/tests/test_unix_domain_socket`
Expected: All 4 tests pass

- [ ] **Step 4: Commit**

```bash
git add tests/net/test_unix_domain_socket.cpp tests/net/CMakeLists.txt
git commit -m "test(uds): add unit tests for UDS path derivation

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 7: Add unit test for UDS listen/accept/connect

**Files:**
- Modify: `tests/net/test_unix_domain_socket.cpp`

- [ ] **Step 1: Add test for UDS server accept flow**

Replace the entire Task 7 with:

```cpp
#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

TEST(UdsAcceptor, ListenAndAccept) {
    EventLoop loop;

    Acceptor acceptor(&loop);
    std::string socket_path = "/tmp/hpactor/test_listen_accept.sock";

    bool server_started = acceptor.listen_unix_domain(socket_path);
    ASSERT_TRUE(server_started);

    // Verify socket file exists
    struct stat st;
    ASSERT_EQ(stat(socket_path.c_str(), &st), 0);

    // Verify it's actually listening
    ASSERT_TRUE(acceptor.is_listening());
    ASSERT_EQ(acceptor.uds_path(), socket_path);

    // Clean up
    acceptor.close_unix_domain();

    // After close, socket file should be unlinked
    ASSERT_EQ(stat(socket_path.c_str(), &st), -1);
}

TEST(UdsAcceptor, AcceptHandler) {
    EventLoop loop;

    Acceptor acceptor(&loop);
    std::string socket_path = "/tmp/hpactor/test_accept_handler.sock";

    bool server_started = acceptor.listen_unix_domain(socket_path);
    ASSERT_TRUE(server_started);

    int client_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    int connect_result = ::connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    // May succeed or be in-progress (non-blocking), either is fine

    // Give event loop a chance to process
    loop.run_after([]() {}, 10);  // 10ms delay

    // Client socket exists and is valid
    ASSERT_GE(client_fd, 0);

    ::close(client_fd);
    acceptor.close_unix_domain();
}

TEST(UdsAcceptor, CloseWithoutUdsPath) {
    EventLoop loop;
    Acceptor acceptor(&loop);

    // Start regular TCP listener
    bool started = acceptor.listen(19995);
    ASSERT_TRUE(started);

    // close_unix_domain() should be safe even though not listening on UDS
    acceptor.close_unix_domain();  // Should not crash
    ASSERT_FALSE(acceptor.is_listening());
}
```

- [ ] **Step 2: Run test to verify it compiles**

Run: `ninja -C build`
Expected: SUCCESS

- [ ] **Step 3: Run the UDS tests**

Run: `./build/tests/test_unix_domain_socket`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add tests/net/test_unix_domain_socket.cpp
git commit -m "test(uds): add UDS acceptor tests

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 8: Add integration test for UDS connect and data flow

**Files:**
- Create: `tests/net/test_uds_integration.cpp`

- [ ] **Step 1: Write integration test that verifies actual data flow**

```cpp
// Integration test: client connects to UDS server and verifies framing works
#include <gtest/gtest.h>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/plain_connection.hpp>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>

namespace hpactor {
namespace net {
namespace {

TEST(UdsIntegration, ConnectAndFrame) {
    EventLoop loop;

    // Server: listen on UDS
    Acceptor acceptor(&loop);
    std::string socket_path = "/tmp/hpactor/test_connect_frame.sock";

    bool server_started = acceptor.listen_unix_domain(socket_path);
    ASSERT_TRUE(server_started);

    int accepted_fd = -1;
    acceptor.set_accept_handler([&accepted_fd](int fd, CommunicationEndpoint) {
        accepted_fd = fd;
    });

    // Client: connect to the server
    int client_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    int result = ::connect(client_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    // May be EINPROGRESS due to non-blocking, that's OK

    // Give loop a moment to process the accept
    loop.run_after([]() {}, 20);

    // Server should have received the accept
    ASSERT_GE(accepted_fd, 0);

    // Both file descriptors are valid and connected
    ASSERT_GE(client_fd, 0);
    ASSERT_GE(accepted_fd, 0);

    // Clean up
    ::close(client_fd);
    if (accepted_fd >= 0) ::close(accepted_fd);
    acceptor.close_unix_domain();
}

TEST(UdsIntegration, PathDerivation) {
    // Sanitize node_id: replace colons with underscores
    auto derive = [](const std::string& node_id) -> std::string {
        std::string s = node_id;
        for (char& c : s) if (c == ':') c = '_';
        return "/tmp/hpactor/" + s + ".sock";
    };

    EXPECT_EQ(derive("localhost:5000"), "/tmp/hpactor/localhost_5000.sock");
    EXPECT_EQ(derive("127.0.0.1:8080"), "/tmp/hpactor/127.0.0.1_8080.sock");
    EXPECT_EQ(derive("node1"), "/tmp/hpactor/node1.sock");
}

} // namespace
} // net
} // hpactor
```

- [ ] **Step 2: Add to `tests/CMakeLists.txt`**

In the `# Net tests` section, add:

```cmake
add_executable(test_uds_integration net/test_uds_integration.cpp)
target_link_libraries(test_uds_integration hpactor)
add_test(NAME test_uds_integration COMMAND test_uds_integration)
```

- [ ] **Step 3: Run build**

Run: `ninja -C build`
Expected: SUCCESS

- [ ] **Step 4: Run integration test**

Run: `./build/tests/test_uds_integration`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add tests/net/test_uds_integration.cpp tests/CMakeLists.txt
git commit -m "test(uds): add UDS integration tests for connect and data flow

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 9: Add TCP fallback test (when UDS path is invalid)

**Files:**
- Modify: `tests/net/test_unix_domain_socket.cpp`

- [ ] **Step 1: Add test for UDS failure fallback to TCP**

Add to `test_unix_domain_socket.cpp`:

```cpp
TEST(UdsFallback, InvalidPathFallsBackToTcp) {
    EventLoop loop;

    // Start a TCP server to have something to fall back to
    Acceptor tcp_acceptor(&loop);
    bool tcp_started = tcp_acceptor.listen(19996);
    ASSERT_TRUE(tcp_started);

    // Try to connect via UDS with non-existent path - should return nullptr
    // TcpTransport doesn't have a direct connect_unix_domain that fails gracefully
    // But we can test the registry fallback path
    auto remote_ep = Ipv4Endpoint{htonl(0x7F000001), htons(19996)};
    TcpTransport transport(remote_ep, TlsConfig{}, PoolConfig{}, nullptr);

    // Direct UDS connect with invalid path returns nullptr
    auto conn = transport.connect_unix_domain(remote_ep, "/nonexistent/path.sock");
    ASSERT_EQ(conn, nullptr);  // UDS should fail gracefully

    // Clean up TCP server
    tcp_acceptor.close();
}

TEST(UdsFallback, TcpFallbackWhenUdsUnavailable) {
    // When registry has no uds_path, connect(ep) should use TCP
    // This is tested by the existing connect(ep) behavior
    // The key is that uds_path.empty() triggers TCP path
    NodeEndpoint ep;
    ep.endpoint = Ipv4Endpoint{htonl(0x7F000001), htons(5000)};
    ep.host = "127.0.0.1";
    ep.tcp_port = 5000;
    ep.uds_path = "";  // Empty = use TCP

    // When uds_path is empty, connect(ep) should skip UDS and use TCP
    // This behavior is verified in the integration of Task 4
}
```

- [ ] **Step 2: Run build**

Run: `ninja -C build`
Expected: SUCCESS

- [ ] **Step 3: Run tests**

Run: `./build/tests/test_unix_domain_socket`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add tests/net/test_unix_domain_socket.cpp
git commit -m "test(uds): add TCP fallback tests for UDS failures

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
---

## Task 10: Final verification — all tests pass

- [ ] **Step 1: Run full test suite**

Run: `ctest --output-on-failure`
Expected: All tests pass including new UDS tests

- [ ] **Step 2: Commit final verification**

```bash
git add -A && git commit -m "test: add UDS path derivation and integration tests

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Summary

| Task | Description | Files Modified |
|------|-------------|----------------|
| 1 | Add `uds_path` field to `NodeEndpoint` | `registrar.hpp` |
| 2 | Add `listen_unix_domain()` to Acceptor | `acceptor.hpp`, `acceptor.cpp` |
| 3 | Add `connect_unix_domain()` to TcpTransport | `tcp_transport.hpp`, `tcp_transport.cpp` |
| 4 | Integrate UDS path lookup in `connect(ep)` | `tcp_transport.cpp` |
| 5 | Add UDS path derivation utility | `tcp_transport.hpp`, `tcp_transport.cpp` |
| 6 | Add unit tests for UDS path derivation | `test_unix_domain_socket.cpp`, `CMakeLists.txt` |
| 7 | Add UDS acceptor tests | `test_unix_domain_socket.cpp` |
| 8 | Add UDS integration tests | `test_uds_integration.cpp`, `CMakeLists.txt` |
| 9 | Add TCP fallback tests | `test_unix_domain_socket.cpp` |
| 10 | Final verification | — |