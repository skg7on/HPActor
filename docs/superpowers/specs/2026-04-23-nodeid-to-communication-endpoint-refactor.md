# NodeId → CommunicationEndpoint Refactor — Execution Plan

**Date:** 2026-04-23
**Status:** Draft
**Supersedes:** `2026-04-22-nodeid-string-refactor.md` (NodeId migrated from `uint32_t` to `std::string`)

## Context

`CommunicationEndpoint` (`std::variant<Ipv4Endpoint, Ipv6Endpoint>`) was introduced to replace the string-based `NodeId = std::string`. `ActorAddress` and the binary frame layer were migrated. However, the network transport layer — `Transport`, `TcpTransport`, `Connection`, `ConnectionPool`, `PlainConnection`, `TlsConnection`, `TlsContext`, the registrar system, and `AsyncActor` — still use `NodeId` throughout.

This plan details the remaining refactor work.

## What's Done

| Component | Status |
|-----------|--------|
| `CommunicationEndpoint` type definition | Done (`types.hpp:221`) |
| `Ipv4Endpoint` / `Ipv6Endpoint` structs | Done (`types.hpp:200-220`) |
| `std::hash<CommunicationEndpoint>` | Done (`types.hpp:494-496`) |
| `ActorAddress.endpoint` field | Done (`actor_address.hpp:89`) |
| `ActorProxy.endpoint()` | Done (`actor_proxy.hpp:45`) |
| Binary frame (`encode_endpoint`, `decode_endpoint`) | Done (`frame.hpp:65-66`, `frame.cpp`) |
| `parse_endpoint(const NodeId&)` bridge | Done (`endpoint.cpp:50`) |

## What's Remaining

### Phase 1 — Remove `NodeId` type and helpers

**File: `include/hpactor/types/types.hpp`**

- [ ] Remove `using NodeId = std::string` (line ~56)
- [ ] Remove `node_id_host()`, `node_id_port()`, `make_node_id()`, `is_local_node_id()` (lines ~60-104)
- [ ] Update `LocalNodeId` — change type from `NodeId` to `CommunicationEndpoint` (likely `Ipv4Endpoint` with `127.0.0.1` and port 0)
- [ ] Update any `LocalNodeId` usages throughout codebase

### Phase 2 — Actor system and spawn

**File: `include/hpactor/core/actor_system.hpp`**
- [ ] `NodeId node_id_` → `CommunicationEndpoint endpoint_`
- [ ] `node_id()` → `endpoint()`
- [ ] Constructor signature update

**File: `include/hpactor/spawn.hpp`**
- [ ] `AsyncActor(NodeId node_id, ...)` → `AsyncActor(CommunicationEndpoint endpoint, ...)`
- [ ] `node_id() const` → `endpoint() const`
- [ ] `NodeId node_id_` → `CommunicationEndpoint endpoint_`

**File: `src/actor/actor_system.cpp`**
- [ ] Update `actor_registry(NodeId)` → `actor_registry(CommunicationEndpoint)`
- [ ] Remove/rewrite comment about `NodeId` format (line ~226-227)

**File: `src/spawn.cpp`**
- [ ] `AsyncActor::AsyncActor(NodeId node_id, ...)` → `AsyncActor::AsyncActor(CommunicationEndpoint endpoint, ...)`

**File: `include/hpactor/core/actor_registry.hpp`**
- [ ] `actor_registry(NodeId node_id)` → `actor_registry(CommunicationEndpoint endpoint)`
- [ ] `NodeId node_id_` → `CommunicationEndpoint node_endpoint_`

### Phase 3 — Transport layer

**File: `include/hpactor/net/transport.hpp`**
- [ ] `Connection(NodeId remote_node)` → `Connection(CommunicationEndpoint remote_endpoint)`
- [ ] `remote_node_` field type change
- [ ] All virtual method signatures using `NodeId` → `CommunicationEndpoint`

**File: `include/hpactor/net/tcp_transport.hpp`**
- [ ] All constructor and method signatures using `NodeId` → `CommunicationEndpoint`

**File: `include/hpactor/net/connection_pool.hpp`**
- [ ] `ConnectionPool(NodeId remote_node_id)` → `ConnectionPool(CommunicationEndpoint remote_endpoint)`
- [ ] `remote_node_id()` → `remote_endpoint()`
- [ ] `NodeId remote_node_id_` → `CommunicationEndpoint remote_endpoint_`

**File: `src/net/tcp_transport.cpp`**
- [ ] Update all `NodeId` parameters to `CommunicationEndpoint`
- [ ] Update internal socket connection logic (inet_pton → endpoint address bytes)

**File: `src/net/connection_pool.cpp`**
- [ ] `ConnectionPool::ConnectionPool(NodeId remote_node_id, ...)` → `ConnectionPool::ConnectionPool(CommunicationEndpoint remote_endpoint, ...)`

**File: `src/net/connection.cpp`**
- [ ] `Connection::Connection(NodeId remote_node)` → `Connection::Connection(CommunicationEndpoint remote_endpoint)`

### Phase 4 — Connection implementations

**File: `include/hpactor/net/plain_connection.hpp`**
- [ ] Constructor `PlainConnection(NodeId remote_node, ...)` → `PlainConnection(CommunicationEndpoint remote_endpoint, ...)`
- [ ] `NodeId remote_node_` → `CommunicationEndpoint remote_endpoint_`

**File: `include/hpactor/net/tls_connection.hpp`**
- [ ] All `NodeId` → `CommunicationEndpoint` in signatures and fields

**File: `src/net/plain_connection.cpp`**
- [ ] Constructor signature and implementation updates

**File: `src/net/tls_connection.cpp`**
- [ ] Constructor signature and implementation updates

### Phase 5 — TlsContext

**File: `include/hpactor/net/tls_context.hpp`**
- [ ] `TlsContext::from_filesystem(NodeId node_id, ...)` → `TlsContext::from_filesystem(CommunicationEndpoint endpoint, ...)`
- [ ] All `NodeId` references → `CommunicationEndpoint`

**File: `src/net/tls_context.cpp`**
- [ ] `from_filesystem(NodeId node_id, ...)` → `from_filesystem(CommunicationEndpoint endpoint, ...)`

### Phase 6 — Registrar system

**File: `include/hpactor/net/registrar.hpp`**
- [ ] `NodeEndpoint` — `NodeId node_id` → `CommunicationEndpoint endpoint`
- [ ] `NodeRegistry` — all `NodeId` → `CommunicationEndpoint`
- [ ] `RegistrarServer` — all `NodeId` → `CommunicationEndpoint`
- [ ] `UdpRegistrar` — all `NodeId` → `CommunicationEndpoint`
- [ ] `RegistrarClient` — all `NodeId` → `CommunicationEndpoint`

**File: `src/net/registrar.cpp`**
- [ ] Update all `NodeId` parameters and fields

**File: `src/net/registrar_server.cpp`**
- [ ] Update all `NodeId` parameters and fields

**File: `src/net/registrar_client.cpp`**
- [ ] Update all `NodeId` parameters and fields

**File: `src/net/acceptor.cpp`**
- [ ] `NodeId node_hint = std::string(ip_str) + ":0"` → construct `CommunicationEndpoint` directly

### Phase 7 — Test updates

**Files: `tests/core/test_types.cpp`, `tests/rpc/test_rpc_channel.cpp`, `tests/ref/test_actor_address.cpp`, `tests/spawn/test_actor_type_registry.cpp`**

- [ ] Replace `hpactor::NodeId` and `hpactor::LocalNodeId` usage with `CommunicationEndpoint` equivalents
- [ ] Use `parse_endpoint()` or direct endpoint construction for test fixtures

### Phase 8 — Documentation

- [ ] Update `docs/superpowers/specs/2026-04-23-communication-endpoint-design.md` — mark implementation complete
- [ ] Update memory: `architectural_decisions.md` — `NodeId` → `CommunicationEndpoint` in Core Type System section
- [ ] Update any architecture docs referencing `NodeId` in the transport layer

## Key Translation Guide

| Old (`NodeId = std::string`) | New (`CommunicationEndpoint`) |
|------------------------------|-------------------------------|
| `NodeId node_id = "127.0.0.1:8080"` | `CommunicationEndpoint endpoint = parse_endpoint("127.0.0.1:8080")` |
| `node_id_host(node_id)` | `endpoint.address()` via `std::get<Ipv4Endpoint>(endpoint).addr` |
| `node_id_port(node_id)` | `endpoint.port()` |
| `make_node_id(host, port)` | `Ipv4Endpoint(inet_pton(host), htons(port))` |
| `is_local_node_id(node_id)` | `endpoint.is_loopback()` |
| `NodeId` as map key | `CommunicationEndpoint` (has `std::hash`) |

## Constraints

- C++20, no exceptions (`-fno-exceptions`), no RTTI (`-fno-rtti`)
- Binary serialization must handle both IPv4 and IPv6 uniformly
- No heap allocation in endpoint types (stack-only `Ipv4Endpoint`/`Ipv6Endpoint`)
- The `parse_endpoint(const NodeId&)` bridge in `endpoint.cpp` is transitional — remove after full migration

## Dependencies

Phase 1 must complete before all others. Phases 2–6 are independent and can run in parallel across files. Phase 7 (tests) depends on all previous phases. Phase 8 (docs) depends on all previous phases.

## Verification

After each phase:
1. Build: `cmake -S . -B build -GNinja && ninja -C build`
2. Tests: `ctest --output-on-failure`
3. No remaining `NodeId` references in modified files (grep check)
