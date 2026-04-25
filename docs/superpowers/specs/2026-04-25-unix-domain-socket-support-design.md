# UNIX Domain Socket Support Design

**Date:** 2026-04-25
**Status:** Design
**Author:** HPActor Team

## Context

When two HPActor processes on the same host communicate, `TcpTransport` uses TCP loopback (`127.0.0.1:port`). This traverses the kernel TCP/IP stack unnecessarily — checksum calculation, TCP state machine, socket buffers, etc. UNIX Domain Sockets (UDS) provide a more efficient path: data never leaves the kernel, no TCP state, no buffer copies.

**Goal:** `TcpTransport` automatically uses UDS for same-host inter-process communication instead of TCP when the OS supports it.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Socket path discovery | Node ID convention | `/tmp/hpactor/<node_id>.sock` — no registry changes needed |
| Socket cleanup | Runtime-only on startup | `unlink()` before `bind()` — standard UDS practice |
| Peer detection | Registry-driven | `NodeRegistry` stores UDS path per node |
| API | `connect_unix_domain()` method | Explicit separation from TCP path |
| TLS over UDS | Not supported | UDS is local-only; kernel provides process isolation |

## Architecture

### Connection Flow

**Before (TCP loopback):**
```
Actor A (pid 1234) → TcpTransport → ConnectionPool → TCP socket 127.0.0.1:5000
                                          ↓
                                Kernel TCP/IP stack
                                          ↓
Actor B (pid 5678) ← TcpTransport ← ConnectionPool ← accepted socket
```

**After (UDS for same-host):**
```
Actor A (pid 1234) → TcpTransport → ConnectionPool → UDS socket /tmp/hpactor/node_b.sock
                                          ↓
                                Kernel (no network stack)
                                          ↓
Actor B (pid 5678) ← TcpTransport ← ConnectionPool ← accepted socket
```

### Transport Interface

```
TcpTransport
├── connect(ep, host, port)          → TCP path (existing)
├── connect(ep)                      → TCP path (existing, registry lookup)
├── connect_unix_domain(ep, path)    → UDS path (NEW)
└── listen(port)                     → TCP listening (existing)
```

### Registry Integration

```
NodeRegistry
├── NodeEndpoint { host, tcp_port, uds_path (NEW field) }
├── get(ep) → NodeEndpoint*
└── set_uds_path(ep, path)
```

```
TcpTransport::connect(ep)
  → Check if ep is loopback (127.0.0.1)
  → Check registry for uds_path
  → If uds_path exists: connect_unix_domain(ep, uds_path)
  → Else: connect over TCP as before
```

## UDS Socket Path Convention

### Path Format
- **Directory:** `/tmp/hpactor/` (created via `mkdir -p` on startup)
- **Filename:** NodeId sanitized (colons `:` replaced with `_`)
- **Example:** NodeId `"localhost:5000"` → `/tmp/hpactor/localhost_5000.sock`

### Cleanup on Startup
- Before `bind()`, call `unlink(path.c_str())` to remove any stale socket from a previous crash
- Directory cleanup is not needed — `/tmp` is cleared on system reboot

### Registration
- When a node registers via `RegistrarClient`, it passes its UDS socket path to the registry
- Alternatively, the path can be derived locally without registry involvement (node knows its own NodeId)

## File Changes

| File | Change |
|------|--------|
| `include/hpactor/net/registrar.hpp` | Add `uds_path` field to `NodeEndpoint` struct |
| `include/hpactor/net/tcp_transport.hpp` | Add `connect_unix_domain()` method declaration |
| `src/net/tcp_transport.cpp` | Implement `connect_unix_domain()` with UDS socket creation and async connect |
| `src/net/acceptor.cpp` | Add `listen_unix_domain()` for inbound UDS connections |
| `src/net/connection_pool.cpp` | Ensure pool keying works with UDS endpoints (endpoint is the key, regardless of transport type) |
| `tests/net/test_tcp_transport.cpp` | Add tests for UDS connection, path convention, fallback to TCP |
| `tests/net/test_unix_domain_socket.cpp` | New test file for UDS-specific functionality |

## Implementation Details

### UDS Server (listen_unix_domain)

```cpp
void Acceptor::listen_unix_domain(const std::string& path) {
    // Remove stale socket
    ::unlink(path.c_str());

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    ::listen(fd, SOMAXCONN);
    listening_fd_ = fd;
    uds_path_ = path;

    loop_->add_fd(fd, EventLoop::Event::Read);
}
```

### UDS Client (connect_unix_domain)

```cpp
ConnectionPtr TcpTransport::connect_unix_domain(CommunicationEndpoint remote_endpoint,
                                                 const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int result = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return nullptr;
    }

    // Create PlainConnection with UDS fd, add to pool
    auto pool = get_or_create_pool(remote_endpoint);
    auto conn = PlainConnection::create_client(fd, remote_endpoint, &loop_);

    pool->add_connection(conn);
    register_connection(conn, fd);

    return conn;
}
```

### PlainConnection UDS Support

`PlainConnection` is already transport-agnostic — it uses a file descriptor for I/O. No changes needed to PlainConnection itself; the only requirement is that the `fd` is a UDS socket instead of a TCP socket.

### Connection Pool Keying

`ConnectionPool` uses `CommunicationEndpoint` as the key for pooling. Since UDS endpoints use a different variant or are distinguished by the registry providing the path, the pool correctly separates UDS and TCP connections to the same logical node.

## Error Handling

| Scenario | Behavior |
|----------|----------|
| UDS socket creation fails | Fall back to TCP loopback |
| UDS connect fails | Log warning, fall back to TCP |
| Platform lacks AF_UNIX | Detect at runtime, skip UDS entirely |
| Registry has no UDS path | Use TCP as default |
| UDS path is stale/unreachable | Fall back to TCP after timeout |

## Platform Considerations

### Linux
- Full support for `AF_UNIX` SOCK_STREAM (connection-oriented, like TCP)
- Abstract namespace sockets available via `sun_path[0] = '\0'` (optional future enhancement)
- Socket files are filesystem entities but auto-cleaned on close if unlinked

### macOS
- Full support for `AF_UNIX` SOCK_STREAM
- No abstract namespace — all paths must be valid filesystem paths
- Socket files persist after process exit unless explicitly unlinked

## Testing Strategy

1. **Unit test: path derivation** — Verify `node_id:port` → `/tmp/hpactor/<node_id>_<port>.sock`
2. **Unit test: UDS server accept** — Create server socket, connect client, verify framing works
3. **Integration test: same-host actor communication** — Two processes on same host, verify messages flow over UDS
4. **Fallback test: TCP recovery** — UDS path missing/incorrect → verify TCP fallback works
5. **Concurrent access test** — Multiple processes connecting to same UDS socket simultaneously

## Open Questions

None. Design decisions are final per user approval.

## Implementation Order

1. **Phase 1:** Add `uds_path` field to `NodeEndpoint` in `registrar.hpp`
2. **Phase 2:** Implement `listen_unix_domain()` in `acceptor.cpp`
3. **Phase 3:** Implement `connect_unix_domain()` in `tcp_transport.cpp`
4. **Phase 4:** Integrate UDS path lookup in `TcpTransport::connect(ep)` for registry-driven fallback
5. **Phase 5:** Add unit tests for path derivation and UDS connection
6. **Phase 6:** Add integration tests for same-host actor communication over UDS