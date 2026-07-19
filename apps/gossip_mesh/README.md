# Gossip Mesh Demo

Demonstrates HPActor's SWIM gossip protocol (`GossipMembership`) by simulating
a 3-node cluster on localhost. Each node discovers peers dynamically through
the SWIM protocol — no static discovery, no hardcoded membership.

## What It Demonstrates

| # | Scenario | What's Exercised |
|---|----------|-----------------|
| 1 | Solo Bootstrap | Seedless node starts as solo cluster |
| 2 | Seed Join & SyncRsp | Node joins via seed, receives full membership sync |
| 3 | Transitive Membership | Third node discovers all peers via piggyback dissemination |
| 4 | Metadata Announce | `announce()` with `actor_types` metadata propagates via piggyback |
| 5 | Failure Detection | Node crashes (no Leave sent) → Suspicious → Dead |
| 6 | Graceful Leave | Node calls `stop()` → sends Leave → other nodes see Left status |
| 7 | Tombstone Purging | Dead/Left tombstones purged after `dead_timeout`; incarnation monotonicity |

## Architecture

```
Node Alpha (:15354)          Node Beta (:15355)          Node Gamma (:15356)
┌──────────────────────┐    ┌──────────────────────┐    ┌──────────────────────┐
│ EventLoop (bg thread)│    │ EventLoop (bg thread)│    │ EventLoop (bg thread)│
│ GossipMembership     │    │ GossipMembership     │    │ GossipMembership     │
│  seeds: (none)       │    │  seeds: [Alpha]      │    │  seeds: [Beta]       │
│ ActorSystem (local)  │    │ ActorSystem (local)  │    │ ActorSystem (local)  │
└──────────────────────┘    └──────────────────────┘    └──────────────────────┘
         │                            │                            │
         └────────────────────────────┼────────────────────────────┘
                           SWIM over UDP
              (Ping/Ack, PingReq/IndirectAck, Join/SyncRsp, Leave)
```

Each node runs its own `EventLoop` on a background thread for UDP gossip I/O.
The `ActorSystem` is local-only (`enable_network=false`) — gossip handles
all membership discovery independently.

## Build

```bash
cd /path/to/HPActor
cmake -S . -B build -GNinja
ninja -C build 20_gossip_mesh
```

## Run

```bash
# Full demo (all scenarios, all nodes)
./apps/gossip_mesh/run_mesh.sh

# Run individual nodes manually (in separate terminals)
./build/apps/gossip_mesh/20_gossip_mesh --role alpha
./build/apps/gossip_mesh/20_gossip_mesh --role beta
./build/apps/gossip_mesh/20_gossip_mesh --role gamma

# Run a single scenario on one node
./build/apps/gossip_mesh/20_gossip_mesh --role alpha --scenario 1

# Adjust ports if defaults conflict
./build/apps/gossip_mesh/20_gossip_mesh --role alpha --base-port 20000
```

**Important**: When running nodes manually, start Alpha first, wait ~1 second,
then Beta, wait ~2 seconds, then Gamma. The seed join order matters.

## Gossip Protocol Details

- **Ports**: UDP 15354 (Alpha), 15355 (Beta), 15356 (Gamma)
- **Timeouts**: protocol_period=500ms, ping_timeout=200ms, suspicion_timeout=2s, dead_timeout=10s
- **Wire format**: Protobuf-encoded SWIM messages (magic `HPGC`, version `0x01`, max 1400 bytes)
- **Failure detection**: 2-node clusters skip indirect probes and go straight to Suspicious
- **Dissemination**: Suspicious/Dead states and Metadata piggyback on Ping/Ack messages

## Logs

Per-node logs: `build/gossip-logs/node-{alpha,beta,gamma}.log`
