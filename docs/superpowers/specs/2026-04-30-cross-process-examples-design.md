# Cross-Process Actor Communication Examples — Design Spec

## Overview

Two new examples that demonstrate HPActor's cross-process communication capabilities,
building from basic message exchange to remote spawn with RPC-style request/response.

Both examples use a single binary that takes `--server` or `--client` on the command
line. The server listens on a known TCP port; the client connects and communicates.

## Prerequisites: Framework Fixes Required

These examples depend on two framework-level fixes that must be implemented first
or as part of the same work:

### P1. `ActorContext::resolve()` gates proxy creation on `is_local()`

**File**: `src/actor/actor_context.cpp`, line 49

`resolve()` uses `ActorAddress::is_local()` (which checks for loopback IP)
to decide whether to create an `ActorProxy` for remote delivery:

```cpp
// actor_context.cpp:49 — only creates ActorProxy when is_local() is false
if (!target.is_local()) {
    ActorProxy proxy(target, system);
    ...
}
```

When two processes run on the same host, both use loopback addresses
(e.g., `127.0.0.1:7000` server, `127.0.0.1:54321` client). `resolve()`
on the server finds no local actor with the client's ID, then skips
`ActorProxy` creation because `is_local()` returns `true` for any
`127.x.x.x` address. **Replies are silently dropped.**

**Required fix**: In `ActorContext::resolve()` at line 49, replace:
```cpp
if (!target.is_local()) {
```
with:
```cpp
if (!(target.endpoint == system->endpoint())) {
```
This compares the target's endpoint against the owning system's identity
rather than guessing based on loopback. The local registry check (step 2
in `resolve()`) already handles same-process actors correctly. This fix
leaves `ActorAddress::is_local()` unchanged (it remains useful as a
network topology query) but removes it from the routing decision.

### P2. No public API to query actor count

**File**: `include/hpactor/core/actor_system.hpp`

`ActorSystem` has no public method returning the number of live actors.
The `actors_` map is private. `actor_registry` only tracks name-to-address
mappings, not the full actor set.

**Required fix**: Add `size_t ActorSystem::actor_count() const` that returns
the number of entries in `actors_` (under `actors_mutex_`).

### P3. `parse_endpoint` double-converts byte order

**File**: `src/net/endpoint.cpp`, line 86

```cpp
return Ipv4Endpoint{htonl(addr.s_addr), htons(static_cast<uint16_t>(port))};
```

`inet_pton` already stores the result in network byte order. The extra
`htonl()` double-converts on little-endian hosts, producing a
host-byte-order value where `Ipv4Endpoint::addr` is documented to hold
network byte order. This makes `is_loopback()` return false for addresses
parsed via `parse_endpoint("127.0.0.1")`.

**Required fix**: Remove the `htonl()` call — use `addr.s_addr` directly.
The `htons()` on the port is correct (that value comes from `stoi`, which
returns host byte order).

---

## Example 09: Cross-Process Echo

**File**: `examples/09_cross_process_echo.cpp`

**Goal**: Minimal cross-process actor communication — a client actor sends
messages to a server actor and prints the replies.

**Architecture**:

```
┌──────────────────────────────┐      ┌──────────────────────────────┐
│  Process A (--server)        │      │  Process B (--client)        │
│                              │      │                              │
│  ActorSystem                 │      │  ActorSystem                 │
│  endpoint: 127.0.0.1:7000    │      │  endpoint: 127.0.0.1:0      │
│  tcp_port: 7000              │      │  tcp_port: 0                 │
│  enable_network: true        │      │  enable_network: true        │
│                              │      │  static_route → 127.0.0.1:7k │
│  EchoActor                   │      │  ClientActor                 │
│    receives EchoMsgTag       │◄─────│    sends 3 EchoMsgTag msgs   │
│    replies "echo: <text>"    │──────►    receives replies, prints  │
│                              │      │                              │
│  waits for messages          │      │  exits after 3 replies       │
└──────────────────────────────┘      └──────────────────────────────┘
```

**Server flow**:

1. Parse `--server` flag, port (default 7000)
2. Create `Config`:
   - `enable_network = true`
   - `endpoint = endpoint_ops::parse_endpoint("127.0.0.1:<port>")`
   - `tcp_port = <port>`
3. Create `ActorSystem`, spawn an `EchoActor` (reuses the same EchoActor from
   Example 01 — receives `EchoMsgTag`, replies with `"echo: <text>"`)
4. Print PID and endpoint, then enter a sleep loop until SIGINT
5. On SIGINT (or after a configurable idle timeout), exit gracefully

**Client flow**:

1. Parse `--client` flag, server port
2. Create `Config`:
   - `enable_network = true`
   - `endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0")`
   - `tcp_port = 0`
3. Configure a static route to the server:
   ```cpp
   config.registrar.static_routes.push_back(
       net::StaticRouteConfig{Ipv4Endpoint{}, "127.0.0.1", port});
   ```
4. Create `ActorSystem`
5. Spawn a local `ClientActor` that:
   - Constructs the remote EchoActor's address using the server endpoint and
     known actor identity. The server spawns EchoActor as its first user actor,
     so `ActorId{1}` and `ActorType{0}` are deterministic:
     ```cpp
     auto server_ep = endpoint_ops::parse_endpoint("127.0.0.1:<port>");
     ActorAddress echo_addr{server_ep, ActorType{0}, ActorId{1}, 0};
     ```
   - Sends 3 `EchoMsgTag` messages via `context()->send(echo_addr, msg)` (the
     framework resolves the address, creates an `ActorProxy`, and delivers via TCP)
   - Counts replies received, prints each one
   - After 3 replies, signals `main()` to exit
6. `main()` waits for the client actor to finish (via `std::atomic<bool>` or
   `std::promise<void>`), then exits

**Why a ClientActor instead of bare transport calls**: From `main()`, there is
no actor context, so `context()->send()` is unavailable. Direct
`transport()->send()` requires manual `WireFrame` construction and does not
automatically route replies. Using a local `ClientActor` keeps the example
consistent with the framework's actor model and lets `context()->reply()` work
transparently (once P1 is fixed).

**Message types**:
- `EchoMsgTag` (TypeTag 100) — string payload, same format as Example 01

**Key APIs demonstrated**:
- `Config::enable_network`, `Config::tcp_port`, `Config::endpoint`
- `RegistrarConfig::static_routes` for deterministic node discovery
- `context()->send()` to a remote `ActorAddress` (automatic `ActorProxy` resolution)
- `context()->reply()` working transparently across process boundaries (after P1)
- Local actor used as both sender and reply receiver (standard actor model)

**Success criteria**:
- Client actor sends "hello", "world", "cross-process"
- Server EchoActor replies "echo: hello", "echo: world", "echo: cross-process"
- Client actor prints all 3 replies and signals exit
- Both processes exit with code 0

---

## Example 10: Remote PID Query (RPC-Style)

**File**: `examples/10_remote_pid_query.cpp`

**Goal**: Remote spawn + structured request/response queries. Client spawns a
`ProcessInfoActor` in the server process via `spawn_remote()`, then sends it
typed queries and prints the responses.

**Architecture**:

```
┌──────────────────────────────┐      ┌──────────────────────────────┐
│  Process A (--server)        │      │  Process B (--client)        │
│                              │      │                              │
│  ActorSystem                 │      │  ActorSystem                 │
│  endpoint: 127.0.0.1:7001    │      │  endpoint: 127.0.0.1:0      │
│  tcp_port: 7001              │      │  tcp_port: 0                 │
│  enable_network: true        │      │  enable_network: true        │
│                              │      │  static_route → 127.0.0.1:7k │
│  ActorTypeRegistry:          │      │                              │
│    "process_info" →          │      │  r = spawn_remote(           │
│      ProcessInfoActorFactory │      │    "127.0.0.1:7001",        │
│                              │      │    "process_info", {})       │
│  ProcessInfoActor            │◄─────│  if (r) {                    │
│  (remote spawned by client)  │      │    ActorRef ref = *r;        │
│    handles:                  │      │    send query messages       │
│      QueryPidMsg             │──►   │    print responses           │
│      QueryActorCountMsg      │──►   │  }                           │
│      ShutdownMsg             │      │                              │
└──────────────────────────────┘      └──────────────────────────────┘
```

**Server flow**:

1. Parse `--server`, port (default 7001)
2. Create `Config`: `enable_network=true`, `endpoint=127.0.0.1:<port>`,
   `tcp_port=<port>`
3. Create `ActorSystem`
4. Register the `ProcessInfoActor` type for remote spawning:
   ```cpp
   system.actor_type_registry().register_type<ProcessInfoActor>("process_info");
   ```
5. Print PID and endpoint, enter sleep loop until SIGINT
6. On SIGINT, exit (the SpawnReceiver handles remote spawn requests
   automatically while the system is running)

**Client flow**:

1. Parse `--client`, server port
2. Create `Config`: `enable_network=true`, `endpoint=127.0.0.1:0`,
   `tcp_port=0`, static route to server
3. Create `ActorSystem`
4. Call `spawn_remote()` (blocking — waits up to `spawn_timeout` for
   the server to respond):
   ```cpp
   auto result = system.spawn_remote("127.0.0.1:<port>", "process_info", bytes{});
   if (!result.has_value()) {
       std::cerr << "spawn_remote failed: " << result.error().message() << std::endl;
       return 1;
   }
   ActorRef ref = result.value();
   ```
5. The returned `ActorRef` wraps an `ActorProxy` pointing to the remote actor.
   Send queries through it (via `ActorContext::send()`, requiring a local
   `QueryActor` that owns the context):
   - Send `QueryPidMsg` → remote actor replies with `PidResponse{pid, hostname}`
   - Send `QueryActorCountMsg` → remote actor replies with
     `ActorCountResponse{actor_count}` (after P2 is implemented)
   - Send `ShutdownMsg` → remote actor sets exit reason and stops
6. Print all responses, exit

**Why a local QueryActor**: Same reasoning as Example 09 — replies are routed
to the sender address set in the `WireFrame`. A local actor provides the sender
identity and `receive()` callback for handling responses. The client spawns a
`QueryActor` that takes the `ActorRef` to the remote actor and runs the
query sequence in its `on_activate()` or first `receive()`.

**Message types** (plain C++ structs, serialized to/from `bytes`):

```cpp
// Messages from client to server
struct QueryPidMsg {};
struct QueryActorCountMsg {};
struct ShutdownMsg {};

// Responses from server to client
struct PidResponse {
    int pid;
    std::string hostname;
};
struct ActorCountResponse {
    int actor_count;
};
```

**Serialization helpers** (defined alongside the message types):

```cpp
// Serialize PidResponse to bytes: [4 bytes pid BE][hostname bytes]
bytes serialize(const PidResponse& r) {
    bytes b;
    b.push_back((r.pid >> 24) & 0xFF);
    b.push_back((r.pid >> 16) & 0xFF);
    b.push_back((r.pid >> 8) & 0xFF);
    b.push_back(r.pid & 0xFF);
    b.insert(b.end(), r.hostname.begin(), r.hostname.end());
    return b;
}
PidResponse deserialize_pid_response(const bytes& b) {
    int pid = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return {pid, std::string(b.begin() + 4, b.end())};
}

// Serialize ActorCountResponse: [4 bytes count BE]
bytes serialize(const ActorCountResponse& r) {
    return {(uint8_t)(r.actor_count >> 24), (uint8_t)(r.actor_count >> 16),
            (uint8_t)(r.actor_count >> 8), (uint8_t)r.actor_count};
}
ActorCountResponse deserialize_actor_count_response(const bytes& b) {
    int count = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    return {count};
}
```

Empty structs (`QueryPidMsg`, `QueryActorCountMsg`, `ShutdownMsg`) are
sent as `TypedMessage(tag, bytes{})` — zero-length payload; the tag alone
identifies the message type.

Custom `TypeTag` values for each message:
```cpp
static const TypeTag QueryPidTag{200};
static const TypeTag PidResponseTag{201};
static const TypeTag QueryActorCountTag{202};
static const TypeTag ActorCountResponseTag{203};
static const TypeTag ShutdownMsgTag{204};
```

**ProcessInfoActor implementation**:

Overrides `make_behavior()` returning a `Behavior` that dispatches on type tag:
- `QueryPidTag` → calls `getpid()`, `gethostname()`, serializes a
  `PidResponse`, calls `context()->reply(TypedMessage(PidResponseTag, serialized_bytes))`
- `QueryActorCountTag` → calls `system().actor_count()` (from P2), serializes
  an `ActorCountResponse`, calls `context()->reply(TypedMessage(ActorCountResponseTag, serialized_bytes))`
- `ShutdownMsgTag` → sets `exit_reason_`, stops processing

**Key APIs demonstrated**:
- `system.spawn_remote()` — spawn an actor in another process (blocking)
- `ActorTypeRegistry::register_type<T>()` — register spawnable actor types
- `SpawnReceiver` — system actor handling spawn requests (auto-wired)
- RPC-style request/response: typed query → typed response via `context()->reply()`
- `ActorRef` as a location-transparent handle (wraps `ActorProxy` for remote)
- Custom serialization for plain C++ structs
- `result<ActorRef>` error handling pattern

**Success criteria**:
- Client successfully spawns `ProcessInfoActor` on the server
- Client receives correct PID and hostname from the remote process
- Client receives correct actor count from `ActorSystem::actor_count()`
- Client sends shutdown; remote actor terminates gracefully
- Both processes exit with code 0

---

## Shared Infrastructure

### Single Binary, Dual Mode

Both examples use the same pattern:

```cpp
int main(int argc, char* argv[]) {
    std::string mode = argc > 1 ? argv[1] : "--help";
    uint16_t port = argc > 2
        ? static_cast<uint16_t>(std::stoi(argv[2]))
        : 0;

    if (mode == "--server") {
        run_server(port);
    } else if (mode == "--client") {
        run_client(port);
    } else {
        std::cout << "Usage: " << argv[0]
                  << " --server [port] | --client <port>" << std::endl;
    }
}
```

### Signal Handling

Servers use `std::atomic<bool> shutdown_requested{false}` set by a SIGINT
handler (`signal(SIGINT, ...)`). The main loop polls this flag:

```cpp
while (!shutdown_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```

### Static Routes vs Auto-Discovery

Both examples use `RegistrarConfig::static_routes` instead of UDP auto-discovery:
- Deterministic — no flakiness from network conditions
- Simpler — no need to explain mDNS/UDP discovery in the example
- Works reliably on the same host

### Connection Establishment

TCP connections are lazily established by `TcpTransport`. When
`context()->send()` resolves a remote address and creates an `ActorProxy`,
the first `send()` triggers `TcpTransport::connect()` which creates a
`ConnectionPool` and establishes a TCP connection to the target endpoint.
No explicit `connect()` call is needed in the example code.

---

## Build Configuration

Both examples are compiled unconditionally (no `#if` guards) since networking
is always available in the library. The `enable_network` flag is a runtime
toggle.

Add to `examples/CMakeLists.txt`:

```cmake
add_example(09_cross_process_echo 09_cross_process_echo.cpp)
add_example(10_remote_pid_query 10_remote_pid_query.cpp)
```

---

## Testing Strategy

### Manual test script

A shell script `test_cross_process.sh` that covers both examples:

```bash
#!/bin/bash
set -e

echo "=== Test 09: Cross-Process Echo ==="
./09_cross_process_echo --server 7000 &
S09=$!
sleep 1
./09_cross_process_echo --client 7000
kill $S09 2>/dev/null; wait $S09 2>/dev/null

echo "=== Test 10: Remote PID Query ==="
./10_remote_pid_query --server 7001 &
S10=$!
sleep 1
./10_remote_pid_query --client 7001
kill $S10 2>/dev/null; wait $S10 2>/dev/null

echo "=== All cross-process tests passed ==="
```

### Integration test approach (future)

Once the framework has a test harness for multi-process tests, these examples
can be converted into automated integration tests. For now, the shell script
is sufficient.

---

## What These Examples Do Not Cover

- **Link/monitor across processes** — death detection depends on connection
  lifecycle management; deferred to a future example
- **HTTP server bridging** — exposing actors via REST endpoints; deferred to a
  future example
- **UDP registrar auto-discovery** — using static routes instead for determinism
- **TLS encryption** — plain TCP for simplicity; TLS config is a separate concern
- **Coroutine dispatch** — behavior-based dispatch only for clarity; the coroutine
  path works identically for networking
- **`spawn_remote_async()`** — non-blocking remote spawn; Example 10 uses the
  simpler blocking `spawn_remote()` to keep the flow easy to follow
- **Orphan cleanup** — if the client crashes before sending `ShutdownMsg`, the
  remotely spawned actor lingers; a future example on cross-process link/monitor
  would address this
