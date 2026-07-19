# Name Resolution Mesh Demo

Demonstrates HPActor's distributed name resolution (PR #453) by simulating
a 3-node microservice cluster on localhost.

## What It Demonstrates

| # | Scenario | What's Exercised |
|---|----------|-----------------|
| 1 | Startup & Discovery | `StaticDiscovery`, `ConsistentHashRing` build |
| 2 | Registration | `register_actor()`, `NameRegisterRequest` (tag 0x80) |
| 3 | Tier-3 Remote Resolve | `resolve_actor()` → home-node query (tag 0x82) |
| 4 | Tier-2 Cache Hit | `NameResolveCache` TTL read (zero network) |
| 5 | Proxy Messaging | `ActorProxy` ping-pong across nodes |
| 6 | Duplicate Detection | `DuplicateName` rejection by home node |
| 7 | Node Departure | Ring rebalance, cache eviction |

## Architecture

```
Node 1 (gateway, :10001)     Node 2 (payment, :10002)     Node 3 (inventory, :10003)
┌─────────────────────────┐  ┌─────────────────────────┐  ┌─────────────────────────┐
│ AuthService ("auth")    │  │ PaymentService("payment")│  │ InventoryService("inv") │
│ NameResolver            │  │ NameResolver            │  │ NameResolver            │
│ ConsistentHashRing      │  │ ConsistentHashRing      │  │ ConsistentHashRing      │
│ StaticDiscovery[1,2,3]  │  │ StaticDiscovery[1,2,3]  │  │ StaticDiscovery[1,2,3]  │
└─────────────────────────┘  └─────────────────────────┘  └─────────────────────────┘
         │                            │                            │
         └────────────────────────────┼────────────────────────────┘
                                TCP localhost
                    (name protocol: register/resolve/unregister)
```

## Build

```bash
cd /path/to/HPActor
cmake -S . -B build -GNinja
ninja -C build 19_name_resolution_mesh
```

## Run

```bash
# Full demo (all scenarios, all nodes)
./apps/name_resolution_mesh/run_mesh.sh

# Run a single node manually (for debugging)
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role gateway
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role payment
./build/apps/name_resolution_mesh/19_name_resolution_mesh --role inventory

# Run a single scenario on one node
./build/apps/name_resolution_mesh/19_name_resolution_mesh \
    --role gateway --scenario 3

# Adjust ports if defaults conflict
./build/apps/name_resolution_mesh/19_name_resolution_mesh \
    --role gateway --base-port 20001
```

## Logs

Per-node logs: `build/mesh-logs/node-{gateway,payment,inventory}.log`
