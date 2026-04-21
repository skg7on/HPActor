# TlsConnection EventLoop Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect TlsConnection to EventLoop's completion callback mechanism via TcpTransport, enabling non-blocking async send with proper completion routing.

**Architecture:** TcpTransport maintains a map of fd → TlsConnectionPtr, sets a completion callback on EventLoop, and routes send completions to the appropriate TlsConnection via `handle_send_completion()`.

**Tech Stack:** C++20, EventLoop, KqueueBackend/EpollBackend, async_send

---

## File Structure

**Files to modify:**
- `include/hpactor/net/tls_connection.hpp` - add public `handle_send_completion()`, add `set_send_completion_handler()`
- `src/net/tls_connection.cpp` - implement `handle_send_completion()` as public, add completion handler member
- `include/hpactor/net/tcp_transport.hpp` - add `connections_` map, `completion_callback_`, `register_connection()`/`unregister_connection()`
- `src/net/tcp_transport.cpp` - set up completion callback in constructor, implement registration methods

---

## Task 1: Make handle_send_completion public in TlsConnection

**Files:**
- Modify: `include/hpactor/net/tls_connection.hpp:168`

- [ ] **Step 1: Move `handle_send_completion` from private to public section**

In `include/hpactor/net/tls_connection.hpp`, move `handle_send_completion(int result)` from the private `// Event loop callbacks` section to the public section (after `close()`).

**Change:**
```cpp
// Move from private to public:
void handle_send_completion(int result);
```

- [ ] **Step 2: Build to verify**

Run: `ninja -C build 2>&1 | tail -10`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add include/hpactor/net/tls_connection.hpp
git commit -m "fix(tls): make handle_send_completion public for completion routing"
```

---

## Task 2: Add set_send_completion_handler to TlsConnection

**Files:**
- Modify: `include/hpactor/net/tls_connection.hpp`
- Modify: `src/net/tls_connection.cpp`

- [ ] **Step 1: Add send_completion_handler_ member and setter to TlsConnection**

In `include/hpactor/net/tls_connection.hpp`, add after `error_handler_` member:

```cpp
// Send completion handler (called by TcpTransport on async_send completion)
std::function<void(int result)> send_completion_handler_;
```

Add public method after `set_error_handler`:

```cpp
void set_send_completion_handler(std::function<void(int result)> handler) {
    send_completion_handler_ = std::move(handler);
}
```

- [ ] **Step 2: Modify handle_send_completion to invoke the handler**

In `src/net/tls_connection.cpp`, modify `handle_send_completion()`:

```cpp
void TlsConnection::handle_send_completion(int result) {
    if (send_completion_handler_) {
        send_completion_handler_(result);
    }
    // ... rest of existing implementation
}
```

- [ ] **Step 3: Build to verify**

Run: `ninja -C build 2>&1 | tail -10`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/net/tls_connection.hpp src/net/tls_connection.cpp
git commit -m "feat(tls): add set_send_completion_handler for callback integration"
```

---

## Task 3: Add completion callback infrastructure to TcpTransport

**Files:**
- Modify: `include/hpactor/net/tcp_transport.hpp`
- Modify: `src/net/tcp_transport.cpp`

- [ ] **Step 1: Add connections map and completion callback to TcpTransport**

In `include/hpactor/net/tcp_transport.hpp`, add after `rpc_handler_`:

```cpp
// Map of fd -> TlsConnection for completion routing
std::unordered_map<int, TlsConnectionPtr> connections_;

// Completion callback for async send routing
std::function<void(OpCompletion)> completion_callback_;
```

Add after `handle_accept` declaration:

```cpp
void register_connection(TlsConnectionPtr conn, int fd);
void unregister_connection(int fd);
```

- [ ] **Step 2: Implement registration methods and completion callback in TcpTransport**

In `src/net/tcp_transport.cpp`, add before `handle_accept`:

```cpp
void TcpTransport::register_connection(TlsConnectionPtr conn, int fd) {
    connections_[fd] = conn;
}

void TcpTransport::unregister_connection(int fd) {
    connections_.erase(fd);
}
```

- [ ] **Step 3: Set up completion callback in TcpTransport constructor**

In `src/net/tcp_transport.cpp`, modify the constructor to set the callback:

```cpp
TcpTransport::TcpTransport(NodeId node_id,
                           const TlsConfig& tls_config,
                           const PoolConfig& pool_config,
                           NodeRegistry* registry)
    : node_id_(node_id),
      loop_(),
      acceptor_(&loop_),
      tls_context_(TlsContext::from_config(tls_config)),
      pool_config_(pool_config),
      registry_(registry) {
    // Set up completion callback to route send completions to TlsConnection
    completion_callback_ = [this](OpCompletion c) {
        if (c.type == OpType::Send) {
            auto it = connections_.find(c.fd);
            if (it != connections_.end()) {
                it->second->handle_send_completion(c.result);
            }
        }
    };
    loop_.set_completion_callback(completion_callback_);
}
```

- [ ] **Step 4: Build to verify**

Run: `ninja -C build 2>&1 | tail -10`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/net/tcp_transport.hpp src/net/tcp_transport.cpp
git commit -m "feat(tcp): add completion callback infrastructure for TlsConnection routing"
```

---

## Task 4: Register TlsConnection in handle_accept

**Files:**
- Modify: `src/net/tcp_transport.cpp`

- [ ] **Step 1: Register the connection with TcpTransport**

In `src/net/tcp_transport.cpp`, modify `handle_accept`:

```cpp
void TcpTransport::handle_accept(int client_fd) {
    auto conn = TlsConnection::create_server(client_fd, 0, &tls_context_, &loop_);
    register_connection(conn, client_fd);
}
```

- [ ] **Step 2: Build to verify**

Run: `ninja -C build 2>&1 | tail -10`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add src/net/tcp_transport.cpp
git commit -m "feat(tcp): register TlsConnection in handle_accept for completion routing"
```

---

## Task 5: Verify tests pass

- [ ] **Step 1: Run all tests**

Run: `ctest --output-on-failure 2>&1 | tail -30`
Expected: All 50 tests pass

- [ ] **Step 2: Run tls-specific tests**

Run: `ctest -R tls --output-on-failure 2>&1`
Expected: All TLS tests pass

---

## Summary

After completing all tasks:
1. TlsConnection has public `handle_send_completion()` method
2. TlsConnection has `set_send_completion_handler()` for callback registration
3. TcpTransport maintains `connections_` map (fd → TlsConnectionPtr)
4. TcpTransport sets completion callback on EventLoop in constructor
5. Completion callback routes send completions to correct TlsConnection
6. Accepted connections are registered with TcpTransport for routing
