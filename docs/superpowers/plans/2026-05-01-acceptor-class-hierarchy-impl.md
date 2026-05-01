# Refactor Acceptor into TCP/UDS Class Hierarchy

## Context

The current `Acceptor` class in `net/acceptor.hpp` handles both TCP and Unix Domain Socket listening in a single class. This muddles concerns:

- `close_unix_domain()` is a no-op on TCP-only acceptors (only clears path, calls close)
- `handle_read()` only parses `sockaddr_in` (TCP), ignoring UDS connections
- Members like `uds_path_` and `bound_port_` coexist even though only one is relevant per use

Separating into abstract base + TCP/UDS subclasses gives each its own `listen()`, `handle_read()`, and `close()` semantics.

## Design

### New Hierarchy

```
Acceptor (abstract base)
├── TcpAcceptor
└── UnixDomainAcceptor
```

### Abstract Base: `Acceptor`

Common state and behavior:

- **Members**: `EventLoop* loop_`, `int listening_fd_ = -1`, `accept_handler accept_handler_`
- **Constructor**: `explicit Acceptor(EventLoop* loop)` — stores loop pointer
- **Virtual destructor**: calls `close()`
- **Non-copyable**: deleted copy/move
- **`virtual void close()`**: removes fd from event loop, closes fd, sets `listening_fd_ = -1`
- **`void set_accept_handler(accept_handler)`**: stores handler
- **`bool is_listening() const`**: returns `listening_fd_ >= 0`
- **`virtual void handle_read() = 0`**: pure virtual — each subclass implements accept + address parse

### `TcpAcceptor : public Acceptor`

- **`bool listen(uint16_t port, uint16_t port_range = 0)`**: AF_INET socket, SO_REUSEADDR, O_NONBLOCK, bind with port range fallback, `listen()`, register fd with event loop
- **`uint16_t port() const`**: returns `bound_port_`
- **`void handle_read() override`**: `accept()` with `sockaddr_in`, creates `Ipv4Endpoint`, calls `accept_handler_`
- **Extra member**: `uint16_t bound_port_ = 0`

### `UnixDomainAcceptor : public Acceptor`

- **`bool listen(const std::string& path)`**: unlink stale socket, AF_UNIX socket, O_NONBLOCK, bind, `listen()`, register fd with event loop
- **`std::string uds_path() const`**: returns `uds_path_`
- **`void close() override`**: unlinks socket file if path non-empty, clears path, calls `Acceptor::close()`
- **`void handle_read() override`**: `accept()` with `sockaddr_un`, creates default `Ipv4Endpoint{}` (no UDS variant in `CommunicationEndpoint`), calls `accept_handler_`
- **Extra member**: `std::string uds_path_`

### Files to Modify

| File | Change |
|------|--------|
| `include/hpactor/net/acceptor.hpp` | Rewrite: abstract base + two subclasses |
| `src/net/acceptor.cpp` | Rewrite: implement all three classes |
| `include/hpactor/net/registrar.hpp:335` | `Acceptor` → `TcpAcceptor` |
| `include/hpactor/net/tcp_transport.hpp:88` | `Acceptor` → `TcpAcceptor` |
| `tests/net/test_tcp_transport_comprehensive.cpp` | Split `Acceptor` → `TcpAcceptor`/`UnixDomainAcceptor` per test |
| `tests/net/test_unix_domain_socket.cpp` | Split `Acceptor` → `TcpAcceptor`/`UnixDomainAcceptor` per test |
| `tests/net/test_uds_integration.cpp` | `Acceptor` → `UnixDomainAcceptor` |

### API Compatibility

- **`accept_handler` typedef unchanged**: `std::function<void(int, CommunicationEndpoint)>` stays in base
- **`RegistrarServer`** (`registrar.hpp:335`): uses `listen(tcp_port)` — TCP only, switch to `TcpAcceptor`
- **`TcpTransport`** (`tcp_transport.hpp:88`): uses `listen(port)` — TCP only, switch to `TcpAcceptor`
- **Tests**: previously mixed TCP and UDS on the same `Acceptor` instance (e.g. `test_unix_domain_socket.cpp:166-178` calls `listen(19995)` then `close_unix_domain()` on same instance). After split, tests use the appropriate subclass for each operation.
- **`handle_read()` preserved as-is**: currently dead code (not wired to event loop via `set_read_handler`); this refactoring does not change that wiring

## Verification

```bash
cmake -S . -B build -GNinja && ninja -C build
ctest --output-on-failure
./build/tests/test_tcp_transport_comprehensive
./build/tests/test_unix_domain_socket
./build/tests/test_uds_integration
```
