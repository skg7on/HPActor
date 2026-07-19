# Name Resolution Mesh Demo App — Design Spec

## 1. Executive Summary

PR #453 added a distributed name resolution layer (`NameResolver`, `NameDirectory`,
`NameResolveCache`, `ConsistentHashRing`, wire protocol on TypeTags 0x80–0x84)
that enables `ActorSystem::resolve_actor("billing-service")` to return a valid
`ActorProxy` when the named actor lives on a remote node.

This spec defines a comprehensive demo application `apps/name_resolution_mesh/`
that simulates three actor system processes as three cluster nodes and exercises
every path in the distributed name resolution feature: registration, three-tier
resolution cascade (local → cache → home-node query), cache warming, proxy
messaging, duplicate detection, and node departure / ring rebalance.

### 1.1 Non-Goals

- Replacing the existing `cluster_control_plane` app (it simulates failure model;
  this app demonstrates name resolution).
- Production deployment patterns (single binary, localhost mesh, shell orchestration).
- Stress/performance benchmarking (correctness demo, not throughput test).
- Modifying the core name resolution implementation.

## 2. Architecture

### 2.1 Deployment Model

Three separate OS processes, each an `ActorSystem` bound to a distinct localhost
TCP port, communicating via localhost TCP. Static discovery seeds provide a
consistent membership view so all three nodes compute the same
`ConsistentHashRing`.

```
┌─ Node 1 (127.0.0.1:10001) ─┐   ┌─ Node 2 (127.0.0.1:10002) ─┐   ┌─ Node 3 (127.0.0.1:10003) ─┐
│  Role: gateway               │   │  Role: payment              │   │  Role: inventory            │
│                              │   │                             │   │                             │
│  ┌────────────────────────┐  │   │  ┌───────────────────────┐  │   │  ┌───────────────────────┐  │
│  │ AuthService            │  │   │  │ PaymentService        │  │   │  │ InventoryService      │  │
│  │ registered: "auth"     │  │   │  │ registered: "payment" │  │   │  │ registered:"inventory"│  │
│  └────────────────────────┘  │   │  └───────────────────────┘  │   │  └───────────────────────┘  │
│                              │   │                             │   │                             │
│  NameResolver ───────────────┼───┼── NameResolver ─────────────┼───┼── NameResolver             │
│  ConsistentHashRing          │   │  ConsistentHashRing         │   │  ConsistentHashRing         │
│  StaticDiscovery[ep1,ep2,ep3]│   │  StaticDiscovery[ep1,ep2,ep3]│   │  StaticDiscovery[ep1,ep2,ep3]│
└──────────────────────────────┘   └─────────────────────────────┘   └─────────────────────────────┘
           │                                  │                                  │
           └──────────────────────────────────┼──────────────────────────────────┘
                                     TCP localhost mesh
                              (register/resolve/unregister on 0x80-0x84)
```

### 2.2 Name Resolution Flow (per scenario)

Each node has the same static discovery seed list, so all three compute an
identical `ConsistentHashRing`. When a node registers a name, it hashes the name
to find the home node and sends a `NameRegisterRequest` (tag 0x80) there.
Resolution follows the three-tier cascade: local `NameDirectory` → cache → home-
node query (tag 0x82).

### 2.3 Inter-Process Coordination

No external dependencies (no Redis, no shared filesystem). Coordination happens
through two mechanisms:

1. **Shell script orchestration** — `run_mesh.sh` launches all three processes,
   monitors their stdout for `READY <scenario>` markers, and advances through
   scenarios sequentially with 2s sleep gaps for network quiescence.

2. **Timer-based advancement** — each process uses `context()->schedule()` to
   poll a local `current_scenario_` counter every 500ms. When the counter
   changes (incremented by an internal `advance_scenario()` call after the
   node completes its scenario duties), the timer handler runs the next
   scenario. A 30s per-scenario timeout prevents indefinite hangs. No signals,
   no shared filesystem — each process advances independently after the shell
   script's sleep gaps allow other nodes to reach readiness.

## 3. Scenario Plan

Seven scenarios executed sequentially:

| # | Scenario | What It Demonstrates |
|---|----------|---------------------|
| 1 | **Startup & Discovery** | All three nodes start, `StaticDiscovery` seeds converge, each builds an identical `ConsistentHashRing` |
| 2 | **Local Registration** | Each node spawns its service actor and registers its name. The home node (per the ring) stores the entry. Print home-node assignments so ring behavior is visible |
| 3 | **Tier-3 Remote Resolve** | Node-1 resolves `"payment"` (homed on node-2) and `"inventory"` via full network round-trip. Cross-resolves between nodes 2 and 3 |
| 4 | **Tier-2 Cache Hit** | Second resolve of `"payment"` on node-1 returns from `NameResolveCache` (zero network). Verify latency difference between first and second resolve |
| 5 | **Message Through Resolved Proxy** | Node-1 sends `PingRequest` to the `ActorProxy` returned by `resolve_actor("payment")`. Payment actor on node-2 receives and replies with `PongResponse`. Proves the resolved proxy is functional end-to-end |
| 6 | **Duplicate Name Detection** | Node-3 tries to register `"payment"` — the home node rejects with `DuplicateName` |
| 7 | **Node Departure & Ring Rebalance** | Node-2 is killed. Nodes 1 and 3 detect departure. Ring rebuilds. Cache entries pointing to node-2 are evicted. Resolving `"payment"` returns empty. Resolving `"inventory"` still works (homed on node-3 all along) |

### 3.1 Output Format

Each node prints structured, human-readable output to stdout:

```
=== Node 1 (gateway) | Scenario 3: Tier-3 Remote Resolve ===
  [resolve] 'payment' -> home=node-2 (127.0.0.1:10002)
  [resolve] Result: ActorId(42) @ 127.0.0.1:10002  [Tier-3: network RTT]
  [resolve] 'inventory' -> home=node-3 (127.0.0.1:10003)
  [resolve] Result: ActorId(43) @ 127.0.0.1:10003  [Tier-3: network RTT]
  PASS

=== Node 1 (gateway) | Scenario 4: Tier-2 Cache Hit ===
  [resolve] 'payment' -> cache hit, no network
  [timing] First resolve: 1.2ms, Cached resolve: 0.003ms
  PASS
```

## 4. Actor Design & Message Types

### 4.1 Service Actors

One per node, each extending `EventBasedActor`:

| Actor | Node | Registered Name | Behavior |
|-------|------|-----------------|----------|
| `AuthServiceActor` | 1 (gateway) | `"auth"` | Ping-pong request-response |
| `PaymentServiceActor` | 2 (payment) | `"payment"` | Ping-pong request-response |
| `InventoryServiceActor` | 3 (inventory) | `"inventory"` | Ping-pong request-response |

Each actor:
- Calls `context()->register_name(name)` during `on_start()`
- Handles `Ping` (fire-and-forget, logs receipt)
- Handles `PingRequest` → replies with `PongResponse{node_id, service_name, timestamp_ns}`

The `PongResponse` payload carries enough metadata to verify the message arrived
at the correct remote actor.

### 4.2 Scenario Driver

Procedural code in `main()` / `run_scenario()` that runs after `ActorSystem` is
fully started:

1. Spawns the service actor
2. Waits for the scenario signal (timer poll or stdout barrier)
3. Executes the node's role in the current scenario
4. Prints results
5. Signals readiness for next scenario

### 4.3 Message Types

Two app-level TypeTags in `messages.hpp`:

```cpp
// App-level tags — do not conflict with name protocol tags (0x80-0x84)
inline constexpr TypeTag AppPingTag        = TypeTag{200};
inline constexpr TypeTag AppPingRequestTag = TypeTag{201};
```

Payload encoding uses `StreamBuffer` with manual big-endian encode/decode
functions (matching the pattern in `order_platform/messages.hpp` and
`edgeops_telemetry/messages.hpp`):

- `encode_ping(node_id, service_name)`
- `encode_ping_request()`
- `encode_pong_response(node_id, service_name, timestamp_ns)`
- `decode_pong_response(buffer)` → struct `PongResponse`

## 5. File Layout & Build Integration

### 5.1 New Files

```
apps/name_resolution_mesh/
├── CMakeLists.txt              # Build: links hpactor_lib + hpactor_cluster
├── main.cpp                    # Entry point: CLI parsing, scenario dispatch
├── scenario.hpp                # ScenarioRunConfig, Scenario enum, run_scenario()
├── scenario.cpp                # Scenario runner, coordination, output formatting
├── actors.hpp                  # AuthServiceActor, PaymentServiceActor, InventoryServiceActor
├── messages.hpp                # App TypeTags, Ping/Pong payloads, encode/decode
├── run_mesh.sh                 # Shell orchestrator: launch/kill 3 processes, collect logs
└── README.md                   # How to build and run
```

### 5.2 CMakeLists.txt

```cmake
add_executable(19_name_resolution_mesh
    main.cpp
    scenario.cpp
)
target_link_libraries(19_name_resolution_mesh PRIVATE
    hpactor_lib
    hpactor_cluster
)
target_include_directories(19_name_resolution_mesh PRIVATE ${CMAKE_SOURCE_DIR})
```

### 5.3 Parent CMakeLists.txt

Add to `apps/CMakeLists.txt`:
```cmake
add_subdirectory(name_resolution_mesh)
```

### 5.4 CLI Interface

```
Usage: 19_name_resolution_mesh --role <gateway|payment|inventory> [options]
Options:
  --role ROLE           Node role: gateway, payment, inventory (required)
  --base-port PORT      Starting port (default: 10001)
  --scenario SCENARIO   Run a single scenario (1-7). Default: run all
  --help
```

Port assignment: gateway→10001, payment→10002, inventory→10003.

### 5.5 Orchestrator Script

`run_mesh.sh`:
1. Build the binary if needed (`ninja -C build 19_name_resolution_mesh`)
2. Create `build/mesh-logs/` directory
3. Launch all three processes backgrounded with `--role` flags
4. Wait for all three to print `READY 1` (scenario 1: startup)
5. For scenarios 2-7: signal advance (via timer), wait for READY markers, print summary
6. On exit or SIGINT: kill all three processes, print log file locations

Logs: `build/mesh-logs/node-gateway.log`, `node-payment.log`, `node-inventory.log`

## 6. Runtime Configuration

Each process configures its `ActorSystem` with:

```cpp
Config make_node_config(NodeRole role, uint16_t port) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.enable_network = true;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:" + std::to_string(port));
    cfg.tcp_port = port;
    cfg.cli.enabled = false;
    // Static discovery: all three endpoints known upfront
    cfg.registrar.static_routes = {
        {endpoint_ops::parse_endpoint("127.0.0.1:10001"), "gateway",   "127.0.0.1:10001"},
        {endpoint_ops::parse_endpoint("127.0.0.1:10002"), "payment",   "127.0.0.1:10002"},
        {endpoint_ops::parse_endpoint("127.0.0.1:10003"), "inventory", "127.0.0.1:10003"},
    };
    // Name resolution cluster wiring.
    // IMPLEMENTATION NOTE: verify the exact Config struct field names —
    // name_resolution config may be set via Config struct or TOML only.
    // The cluster_system_bridge wiring requires ActorSystem::enable_cluster().
    cfg.name_resolution.enabled = true;
    cfg.name_resolution.resolve_timeout_ms = 2000;
    cfg.name_resolution.register_timeout_ms = 5000;
    cfg.name_resolution.cache_ttl_seconds = 30;
    cfg.name_resolution.virtual_nodes = 100;
    return cfg;
}

// After ActorSystem construction:
//   system.enable_cluster(role_name);  // wires cluster_system_bridge
```

## 7. Error Handling & Edge Cases

| Scenario | Error Case | Behavior |
|----------|-----------|----------|
| Startup | Port already in use | Process exits with error message |
| Startup | Not all nodes ready within 10s | Timeout, remaining nodes exit |
| Resolve | Name not registered anywhere | `resolve_actor()` returns empty `ActorRef` |
| Resolve | Home node unreachable | Timeout after `resolve_timeout_ms`, returns empty |
| Register | Duplicate name | Home node returns `DuplicateName`, caller prints warning |
| Register | Home node unreachable | Timeout after `register_timeout_ms`, registration fails |
| Departure | Node killed mid-scenario | Remaining nodes detect via discovery, ring rebuilds |
| Orchestrator | SIGINT/Ctrl-C | Kill all three processes, clean up |

## 8. Test Plan

No automated CTest tests are needed for the demo app — it is a manual
demonstration tool. Verification gates:

1. **Build:** `ninja -C build 19_name_resolution_mesh` compiles cleanly
2. **Smoke:** `./run_mesh.sh` completes all 7 scenarios with PASS verdicts
3. **Manual inspection:** Log output shows correct home-node assignments,
   cache-hit latency improvement, duplicate rejection, and post-departure
   eviction

## 9. Build & Run Quick Reference

```bash
# Build
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build 19_name_resolution_mesh

# Run full demo
cd apps/name_resolution_mesh && ./run_mesh.sh

# Run individual node manually (for debugging)
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role gateway
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role payment
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role inventory

# Run single scenario
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role gateway --scenario 3
```
