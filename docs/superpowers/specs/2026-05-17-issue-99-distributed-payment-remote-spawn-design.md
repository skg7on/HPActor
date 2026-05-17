# Distributed Payment Role with Remote Spawn — Design

**Date:** 2026-05-17
**Status:** Draft
**Issue:** [#99](https://github.com/skg7on/HPActor/issues/99)

---

## Overview

Add a distributed payment path to the order platform example that demonstrates remote actor spawn and cross-process message delivery over TCP transport. The gateway remote-spawns a `PaymentActor` on a separate process and routes `AuthorizePaymentTag` messages over TCP instead of delivering locally.

## Current State

- `--payment` mode: calls `run_long_role("PAYMENT")` — spawns `ActorSystem` with network enabled, prints the endpoint, then sits in a sleep loop. No `PaymentActor` is spawned, no remote spawn listener is active.
- `--gateway` mode: stub (same as above).
- Remote spawn infrastructure is fully implemented: `spawn_remote_async()`, `SpawnReceiver`, `ActorTypeRegistry`, `ConnectionPool` routing, `WireFrame` encoding — all wired and tested (5 test files in `tests/spawn/`).
- `ActorLocationCache` resolves ActorId → EndPoint with TTL caching.
- Example 10 (`10_remote_pid_query.cpp`) demonstrates two-process remote communication — the pattern works.

## Goals

- **`--payment` mode**: spawn a local `PaymentActor`, register `PaymentActor` in `ActorTypeRegistry`, listen on TCP for remote spawn requests, keep running until Ctrl-C
- **`--gateway` mode with `--payment <endpoint>`**: connect to the remote payment node, remote-spawn a `PaymentActor` on it, route payment messages to the remote actor instead of spawning locally
- **Full two-process happy path**: orders submitted to the gateway flow through local inventory, remote payment, local fulfillment, and complete successfully
- Demonstrate `spawn_remote_async()`, `ActorTypeRegistry::register_type<>()`, cross-process `context()->send()`, and `SpawnReceiver` request handling
- No new external dependencies

## Non-Goals

- Registrar-based service discovery for the remote payment node — use static routes (explicit `--payment <ip:port>` flag)
- Two-process `--scenario payment-timeout` — timer delivery needs #97 but timeout is tested locally in all-in-one mode
- Multi-payment-node load balancing — single remote payment node
- Remote spawn of inventory, fulfillment, or coordinator actors — payment only
- TLS on the cross-process link — plain TCP for demo clarity

---

## Architecture

### Two-Process Layout

```
┌──────────────────────────────────┐   ┌──────────────────────────────────┐
│ Process 1: GATEWAY               │   │ Process 2: PAYMENT               │
│ (actor_port: 17130)              │   │ (actor_port: 17132)              │
│                                  │   │                                  │
│  ActorSystem                     │   │  ActorSystem                     │
│  ├─ HTTPGatewayActor :18130      │   │  ├─ SpawnReceiver (system)       │
│  ├─ OrderCoordinatorActor        │   │  ├─ PaymentActor (local spawn)   │
│  │   payment address ────────────┼───┼─▶│   handles AuthorizePaymentTag │
│  │   = remote proxy              │   │  │                                │
│  ├─ InventoryActor (local)       │   │  └─ ActorLocationCache            │
│  ├─ FulfillmentWorker (local)    │   │                                  │
│  ├─ OrderLogActor (local)        │   │  [TCP transport on 17132]        │
│  └─ TcpTransport                 │   │                                  │
│                                  │   │                                  │
│  [TCP transport on 17130]        │   │                                  │
└──────────────────────────────────┘   └──────────────────────────────────┘
          │                                       │
          └─────────── TCP connection ────────────┘
                spawn_remote_async("127.0.0.1:17132", "PaymentActor")
                context()->send(remote_payment_addr, AuthorizePayment)
```

### Remote Spawn Flow

```
Gateway Process                            Payment Process
===============                            ===============

spawn_remote_async("127.0.0.1:17132", 
                   "PaymentActor", args)
  │
  ├─ Build WireFrame (RpcRequest,
  │   target=SpawnReceiverId@remote)
  ├─ Register AsyncActor in pending_spawns_
  └─ transport_->send()
        │
        └─── TCP ─────────────────────▶  ConnectionPool::on_frame_received()
                                              │
                                              ├─ Frame is NOT RpcResponse
                                              ├─ actor_message_handler_(frame)
                                              └─ ActorSystem::deliver_remote(frame)
                                                    │
                                                    └─ deliver_local(SpawnReceiverId, msg)
                                                          │
                                                          └─ SpawnReceiver::make_behavior()
                                                                │
                                                                ├─ type_id == SpawnRequestTag
                                                                ├─ registry_.spawn("PaymentActor", ...)
                                                                │    └─ Factory: sys.spawn<PaymentActor>()
                                                                └─ transport_->send(SpawnResponse)
                                                                      │
  ◀─── TCP ───────────────────────────┘
  │
ConnectionPool::on_frame_received()
  ├─ Frame IS RpcResponse + SpawnResponseTag
  ├─ spawn_handler_(message_id, response)
  └─ pending_spawns_[msg_id]->set_response(response)
        │
        └─ AsyncActor::get() → ActorRef(remote_proxy)

  ... coordinator sends payment ...

context()->send(remote_payment_addr, AuthorizePayment)
  │
  ├─ ActorRef::try_send(addr, msg)
  ├─ ActorProxy::try_send(addr, msg)
  │    ├─ ActorLocationCache.get(actor_id)
  │    ├─ Build WireFrame (sender=coordinator, target=payment_actor)
  │    └─ transport_->try_send(addr, frame.encode())
  └───── TCP ────────────────────────▶  deliver_local(payment_actor_id, msg)
                                              │
                                              └─ PaymentActor handles AuthorizePaymentTag
```

---

## Detailed Design

### 1. Payment Mode (`--payment`)

```cpp
int run_payment(const Options& opts) {
    Config config = make_base_config(opts, opts.actor_port);
    config.enable_network = true;
    config.tcp_port = opts.actor_port;
    ActorSystem system(config);

    // Register PaymentActor for remote spawning.
    system.actor_type_registry().register_type<PaymentActor>("PaymentActor");

    // Spawn a local PaymentActor (the one remote spawns will create on demand).
    // Actually, remote spawn creates NEW instances per spawn_remote call.
    // We just need the type registered; the SpawnReceiver handles creation.

    std::cout << "PAYMENT node listening on "
              << endpoint_ops::to_string(system.endpoint()) << "\n";
    run_until_signal("PAYMENT");
    return 0;
}
```

Key insight: `ActorTypeRegistry::register_type<PaymentActor>("PaymentActor")` registers a factory lambda that calls `system.spawn<PaymentActor>()`. When a remote spawn request arrives, `SpawnReceiver` calls `registry.spawn(...)` which invokes this factory, creating a new `PaymentActor` instance. The local `ActorSystem` handles the rest — mailbox creation, scheduling, etc.

The payment node does NOT need to pre-spawn a `PaymentActor`. Each remote spawn request creates a new instance via the registered factory.

### 2. Gateway Mode with Remote Payment (`--gateway --payment <endpoint>`)

```cpp
int run_gateway(const Options& opts) {
    Config config = make_base_config(opts, opts.actor_port);
    config.enable_network = true;
    config.tcp_port = opts.actor_port;

    // Add static route to payment endpoint so transport can resolve it.
    if (!opts.payment_endpoint.empty()) {
        auto ep = endpoint_ops::parse_endpoint(opts.payment_endpoint);
        config.registrar.static_routes.push_back(StaticRouteConfig{
            ep, 
            endpoint_ops::host(ep),
            endpoint_ops::port(ep)
        });
    }

    ActorSystem system(config);

    // Spawn local actors.
    auto log = system.spawn<OrderLogActor>();
    auto inventory = system.spawn<InventoryActor>();
    auto fulfillment_worker = system.spawn<FulfillmentWorkerActor>();

    // Resolve payment address.
    ActorRef payment_ref;
    if (!opts.payment_endpoint.empty()) {
        // Remote spawn on the payment node.
        StreamBuffer args;  // no constructor args for PaymentActor
        AsyncActor async = system.spawn_remote_async(
            opts.payment_endpoint, "PaymentActor", args);

        auto result = async.get(std::chrono::seconds(5));
        if (!result) {
            std::cerr << "Failed to remote-spawn PaymentActor: "
                      << result.error().message() << "\n";
            return 1;
        }
        payment_ref = *result;
        std::cout << "Remote PaymentActor spawned: "
                  << endpoint_ops::to_string(payment_ref.address().endpoint)
                  << " id=" << payment_ref.address().id << "\n";
    } else {
        // Fall back to local PaymentActor.
        auto payment = system.spawn<PaymentActor>();
        payment_ref = ActorRef{payment};
    }

    // Coordinator uses the resolved payment_ref.
    auto coordinator = system.spawn<OrderCoordinatorActor>(
        inventory.address(),
        payment_ref.address(),   // may be remote
        fulfillment_worker.address(),
        log.address(),
        nullptr);

    // HTTP gateway setup (see #98).
    // ...

    run_until_signal("GATEWAY");
    return 0;
}
```

### 3. Static Route for Remote Endpoint Resolution

`TcpTransport::try_send()` calls `get_or_create_pool(target.endpoint)` which creates a `ConnectionPool` for the remote node. The first send triggers a TCP `connect()`. Existing `HostResolver` / `NodeRegistry` infrastructure handles DNS/static resolution.

The `StaticDiscovery` backend already supports `config.registrar.static_routes` — entries are added to `NodeRegistry` during transport construction. This means the transport can resolve `127.0.0.1:17132` without a registrar server.

### 4. Cross-Process Message Routing

When the coordinator calls `context()->send(payment_addr, msg)` where `payment_addr` is a remote address:

1. `ActorContext::send()` → `ActorRef::try_send()` → `ActorProxy::try_send()`
2. `ActorProxy` checks `ActorLocationCache` for the ActorId
3. If not cached, uses `ServiceDiscovery::discover()` → returns the endpoint from the static route
4. Builds `WireFrame` with sender (coordinator), receiver (remote payment actor), type_tag, payload
5. `transport_->try_send(addr, frame.encode())` → `ConnectionPool::try_send()` → TCP write

On the receiving side:
1. `ConnectionPool::on_frame_received()` → `actor_message_handler_(frame)` 
2. `ActorSystem::deliver_remote(frame)` → `try_deliver_local(target_id, msg)`
3. `MPSCActorMailbox::try_push()` → scheduled on a worker thread
4. `PaymentActor::receive()` handles `AuthorizePaymentTag`

### 5. Reply Routing

When the remote `PaymentActor` calls `context()->reply(msg)`:
1. `ActorContext::reply()` reads `current_sender_` (set during `receive()` from the `WireFrame`'s sender field)
2. `current_sender_` contains the coordinator's `ActorAddress` (endpoint + ActorId)
3. `ActorRef::try_send(current_sender_, msg)` → `ActorProxy::try_send()` → `transport_->try_send()` back to the gateway
4. Reply arrives at the gateway's `ConnectionPool` → `deliver_remote()` → `deliver_local(coordinator_id, msg)`

Reply routing is already fully implemented and tested in the remote spawn infrastructure.

---

## Changes Summary

### Files Modified

| File | Change |
|------|--------|
| `examples/13_order_platform.cpp` | Replace `run_long_role("PAYMENT")` and `run_long_role("GATEWAY")` stubs with remote spawn implementations |
| `tests/examples/test_order_platform_messages.cpp` | Tests unchanged (no new payload types) |

### New Files

None. All infrastructure already exists.

### Files Referenced (no changes)

| File | Role |
|------|------|
| `include/hpactor/core/actor_system.hpp:373-379` | `spawn_remote_async()` declaration |
| `src/actor/actor_system.cpp:516-566` | `spawn_remote_async()` implementation |
| `include/hpactor/actor_type_registry.hpp:43` | `register_type<T>(name)` |
| `include/hpactor/spawn.hpp:68-111` | `AsyncActor` handle |
| `include/hpactor/ref/actor_proxy.hpp` | `ActorProxy::try_send()` — remote message routing |
| `include/hpactor/net/actor_location_cache.hpp` | TTL cache for ActorId → EndPoint |
| `src/actor/spawn_receiver.cpp` | `SpawnReceiver::handle_spawn_request()` |

---

## Acceptance Criteria

1. `./13_order_platform --payment --actor-port 17132` starts and prints "PAYMENT node listening on 127.0.0.1:17132"
2. `./13_order_platform --gateway --actor-port 17130 --payment 127.0.0.1:17132` connects to the payment node and remote-spawns a `PaymentActor`
3. A demo order submitted to the gateway completes with `status=completed` — the payment authorization flows over TCP
4. If the payment node is not running, `spawn_remote_async().get()` returns an error after timeout with a clear message
5. Ctrl-C on either process triggers clean shutdown
6. No new external dependencies

## Test Plan

### Manual integration test (interactive, two terminals)

1. Terminal 1: `./13_order_platform --payment --actor-port 17132`
2. Terminal 2: `./13_order_platform --gateway --actor-port 17130 --payment 127.0.0.1:17132 --scenario happy-path`
3. Verify Terminal 2 prints `status=completed`
4. Terminal 2: Ctrl-C. Terminal 1: Ctrl-C.

### Failure test (interactive)

1. Terminal 2: `./13_order_platform --gateway --actor-port 17130 --payment 127.0.0.1:17199`
2. Verify: prints "Failed to remote-spawn PaymentActor: timeout" or similar
