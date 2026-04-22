# Plan: NodeId from uint32_t to String (host:port format)

## Status: Completed

## Context

`NodeId` was previously a `uint32_t` placeholder (0=local, >0=remote). This didn't work for real distributed systems because `ActorAddress::node_id` should directly contain the network location for socket connections without requiring a registry lookup.

**Decision:** `NodeId` became `std::string` in `"host:port"` format (e.g., `"192.168.1.1:5353"` or `"node.example.com:5353"`). Local actors use empty string `""`.

---

## Phase 1: Core Type Changes

### 1.1 `include/hpactor/types/types.hpp`
- Changed `NodeId` from `uint32_t` to `std::string`
- Changed `LocalNodeId` from `0` to `""` (empty string)
- Added helper functions:
  - `node_id_host(const NodeId& id)` — extract host from "host:port"
  - `node_id_port(const NodeId& id)` — extract port from "host:port"
  - `make_node_id(const std::string& host, uint16_t port)` — combine to "host:port"
  - `is_local_node_id(const NodeId& id)` — check if id.empty()

### 1.2 `include/hpactor/ref/actor_address.hpp`
- Updated `std::hash<hpactor::NodeId>` from `std::hash<uint32_t>` to `std::hash<std::string>`
- Updated `ActorAddress::is_local()` to use `is_local_node_id(node_id)`
- Default `node_id = ""` instead of `= 0`

---

## Phase 2: Serialization Changes

### 2.1 `src/core/serialization.cpp`
Changed all `uint32_t node_id` serialization to length-prefixed string:

**encode_system functions** (down_msg, exit_msg, link_msg, unlink_msg):
```cpp
// OLD: uint32_t node_id; memcpy with sizeof(uint32_t)
// NEW: uint32_t len = node_id.size(); memcpy(len); memcpy(node_id.data(), len);
```

**decode_system functions** — reverse pattern:
```cpp
uint32_t len; memcpy(&len, ...); offset += sizeof(uint32_t);
std::string node_id;
if (len > 0) { node_id.resize(len); memcpy(node_id.data(), ...); offset += len; }
```

**encode_spawn / decode_spawn** for SpawnRequest and SpawnResponse — same length-prefixed encoding.

---

## Phase 3: Network Component Changes

### 3.1 `include/hpactor/net/registrar.hpp`
- `StaticRouteConfig::node_id` — changed from `uint32_t` to `std::string`, default `""`
- `NodeEndpoint::node_id` — changed from `uint32_t` to `std::string`, default `""`
- All methods taking `NodeId` updated signatures

### 3.2 `include/hpactor/net/tls_context.hpp`
- `TlsConfig::node_id` — string type
- `TlsContext::node_id_` — string type
- `from_filesystem()` — sanitizes node_id for filename (replaces `:` with `_`)

### 3.3 `include/hpactor/net/tls_connection.hpp`
- `remote_node_id_` — string type

### 3.4 `include/hpactor/net/plain_connection.hpp`
- `remote_node_id_` — string type

### 3.5 `src/net/acceptor.cpp`
- Fixed to convert IP to string with `inet_ntop`:
  ```cpp
  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
  NodeId node_hint = std::string(ip_str) + ":0";
  ```

### 3.6 `src/net/registrar_server.cpp`
- Updated Register message parsing to use length-prefixed string node_id
- Updated `broadcast_node_joined` and `broadcast_node_left` to encode string node_ids

### 3.7 `src/net/registrar.cpp`
- Updated `handle_udp_packet` for ResolveQuery and ResolveResponse to use length-prefixed strings

### 3.8 `src/net/registrar_client.cpp`
- Updated `send_registration`, `resolve_node`, `failover` to use length-prefixed string node_ids
- Fixed `probe_id` to use hash: `uint64_t probe_id = std::hash<std::string>{}(local_node_id_);`

---

## Phase 4: Actor/Spawn Changes

### 4.1 `include/hpactor/spawn.hpp`
- `AsyncActor::node_id_` — string type

### 4.2 `src/actor/actor_system.cpp`
- Fixed: `NodeId remote_node_id = node_name;` (was `stoul`)

---

## Phase 5: Test Updates

All test files updated to use string node_ids instead of integer:

| Test File | Changes |
|-----------|---------|
| `tests/core/test_serializer.cpp` | `ActorAddress addr("localhost:12345", ...)` |
| `tests/ref/test_actor_address.cpp` | `ActorAddress addr{"remotehost:12345", ...}` |
| `tests/ref/test_actor_proxy.cpp` | String node_ids throughout |
| `tests/actor/test_actor_context.cpp` | String node_ids in ActorAddress |
| `tests/net/test_frame.cpp` | String node_ids for Frame tests |
| `tests/net/test_tls_context.cpp` | `config.node_id = "node42:12345"` |
| `tests/net/test_tls_connection.cpp` | All `create_client(n, ...)` → `create_client("nodeN:12345", ...)` |
| `tests/net/test_registrar.cpp` | String node_ids for NodeEndpoint/StaticRouteConfig |
| `tests/net/test_tls_integration.cpp` | String node_id in TlsConfig |
| `tests/spawn/test_async_actor.cpp` | `AsyncActor handle("node42:12345", ...)` |
| `tests/spawn/test_spawn_serialization.cpp` | String node_ids for ActorAddress |
| `tests/spawn/test_spawn_integration.cpp` | Length-prefixed string encoding for manual encode/decode test |

---

## Verification

```bash
# Build
cmake -S . -B build -GNinja
ninja -C build

# Run tests
ctest --output-on-failure
```

**Result:** 36/36 targets compiled, 50/50 tests passed.
