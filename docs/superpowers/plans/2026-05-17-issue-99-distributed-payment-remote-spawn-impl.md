# Distributed Payment with Remote Spawn Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a distributed payment path to the order platform example demonstrating remote actor spawn and cross-process message delivery over TCP. The gateway remote-spawns a `PaymentActor` on a separate process and routes `AuthorizePaymentTag` messages over TCP.

**Architecture:** `--payment` mode spawns an `ActorSystem` with network enabled, registers `PaymentActor` in `ActorTypeRegistry`, and listens for remote spawn requests. `--gateway --payment <endpoint>` uses `spawn_remote_async()` to create a `PaymentActor` on the remote node, and the coordinator's payment address is set to the remote proxy. Cross-process message delivery uses existing `ActorProxy::try_send()` → `TcpTransport` → `ConnectionPool` → TCP → remote `deliver_local()`.

**Tech Stack:** C++20, existing `spawn_remote_async()`, `SpawnReceiver`, `ActorTypeRegistry`, `TcpTransport`, `ConnectionPool`, `StaticDiscovery`.

**Spec:** `docs/superpowers/specs/2026-05-17-issue-99-distributed-payment-remote-spawn-design.md`

---

## File Structure

| File | Purpose |
|------|---------|
| `examples/13_order_platform.cpp` | **Modified** — replace `run_long_role("PAYMENT")` stub; add remote spawn logic to gateway mode; wire remote payment address into coordinator |

No new files. All remote spawn infrastructure already exists.

---

### Task 1: Implement run_payment() with ActorTypeRegistry Registration

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Add run_payment() function**

```cpp
int run_payment(const Options& opts) {
    Config config = make_base_config(opts, opts.actor_port);
    config.enable_network = true;
    config.tcp_port = opts.actor_port;
    ActorSystem system(config);

    // Register PaymentActor so remote spawn requests can create instances.
    system.actor_type_registry().register_type<PaymentActor>("PaymentActor");

    std::cout << "PAYMENT node listening on "
              << endpoint_ops::to_string(system.endpoint())
              << " (tcp=" << opts.actor_port << ")\n";
    std::cout << "  registered types: PaymentActor\n";

    run_until_signal("PAYMENT");
    return 0;
}
```

- [ ] **Step 2: Hook into main() dispatch**

In `main()`, change:

```cpp
if (opts->mode == "--payment")
    return run_payment(*opts);  // was: run_long_role(*opts, "PAYMENT");
```

This requires that `run_payment()` is declared before `main()` or forward-declared.

---

### Task 2: Implement Remote Spawn in Gateway Mode

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Add `run_gateway()` function with remote payment support**

This replaces the `--gateway` dispatch in `main()` (building on the #98 gateway implementation):

```cpp
int run_gateway(const Options& opts) {
    Config config = make_base_config(opts, opts.actor_port);
    config.enable_network = true;
    config.tcp_port = opts.actor_port;

    // If a payment endpoint is provided, add it as a static route
    // so the transport can resolve it without a registrar.
    ActorRef payment_ref;
    bool payment_is_remote = false;

    ActorSystem system(config);

    // Spawn local actors first.
    auto log = system.spawn<OrderLogActor>();
    auto inventory = system.spawn<InventoryActor>();
    auto fulfillment_worker = system.spawn<FulfillmentWorkerActor>();

    // Resolve payment actor.
    if (!opts.payment_endpoint.empty()) {
        // Add static route for the remote payment node.
        auto remote_ep = endpoint_ops::parse_endpoint(opts.payment_endpoint);
        config.registrar.static_routes.push_back(
            StaticRouteConfig{remote_ep,
                endpoint_ops::host_str(remote_ep),
                endpoint_ops::port(remote_ep)});

        // Remote-spawn PaymentActor on the payment node.
        StreamBuffer args;  // PaymentActor takes no constructor args beyond ctx+sys
        AsyncActor async = system.spawn_remote_async(
            opts.payment_endpoint, "PaymentActor", args);

        std::cout << "Remote-spawning PaymentActor on "
                  << opts.payment_endpoint << "...\n";

        auto result = async.get(std::chrono::seconds(5));
        if (!result) {
            std::cerr << "Failed to remote-spawn PaymentActor: "
                      << result.error().message() << "\n";
            std::cerr << "Is the payment node running?\n";
            return 1;
        }
        payment_ref = *result;
        payment_is_remote = true;
        std::cout << "Remote PaymentActor spawned: id="
                  << payment_ref.address().id << "\n";
    } else {
        // Fall back to local PaymentActor.
        auto payment = system.spawn<PaymentActor>();
        payment_ref = ActorRef{payment};
        std::cout << "Local PaymentActor spawned\n";
    }

    // Coordinator uses the resolved payment address.
    auto coordinator = system.spawn<OrderCoordinatorActor>(
        inventory.address(),
        payment_ref.address(),
        fulfillment_worker.address(),
        log.address(),
        nullptr);  // long-running — no completion promise

    // HTTP gateway setup (see issue #98).
    // [HTTPGatewayActor route registration...]

    std::cout << "GATEWAY running"
              << (payment_is_remote ? " (remote payment)" : " (local payment)")
              << "\n";
    run_until_signal("GATEWAY");
    return 0;
}
```

- [ ] **Step 2: Add static route config**

The static route must be added to the config BEFORE the ActorSystem is constructed, since `ActorSystem` constructor reads `config.registrar.static_routes` and registers them. Move the static route addition before `ActorSystem system(config)`:

```cpp
Config config = make_base_config(opts, opts.actor_port);
config.enable_network = true;
config.tcp_port = opts.actor_port;

if (!opts.payment_endpoint.empty()) {
    auto remote_ep = endpoint_ops::parse_endpoint(opts.payment_endpoint);
    config.registrar.static_routes.push_back(
        StaticRouteConfig{remote_ep,
            endpoint_ops::host_str(remote_ep),
            endpoint_ops::port(remote_ep)});
}

ActorSystem system(config);
// ... rest
```

Note: `endpoint_ops::host_str()` and `endpoint_ops::port()` may not exist as named functions. Use `endpoint_ops::to_string()` on just the IP portion, or use `inet_ntop` directly. Check the existing API surface. If needed, parse the host string from `opts.payment_endpoint` directly (split on `:`).

- [ ] **Step 3: Add needed includes**

Ensure `examples/13_order_platform.cpp` includes:
```cpp
#include <hpactor/actor_type_registry.hpp>  // for register_type<>
#include <hpactor/spawn.hpp>                // for AsyncActor
```

---

### Task 3: Wire Remote Payment Address into Coordinator

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Verify OrderCoordinatorActor constructor accepts ActorAddress**

The current constructor signature (line 336-341):

```cpp
OrderCoordinatorActor(ActorContext* ctx, ActorSystem& sys,
                      ActorAddress inventory,
                      ActorAddress payment,
                      ActorAddress fulfillment,
                      ActorAddress log,
                      std::promise<OrderStatusPayload>* done);
```

It already takes `ActorAddress` by value — no change needed. The `payment_ref.address()` returns the remote actor's `ActorAddress`, which is valid.

- [ ] **Step 2: Verify context()->send() works with remote address**

`ActorContext::send()` delegates to `ActorRef::try_send()` which dispatches to `ActorProxy::try_send()` for remote addresses. The `ActorProxy` created during `AsyncActor::get()` has the transport wired through `ActorSystem`. Verify the path works — no source changes needed.

---

### Task 4: Build and Manual Integration Testing

- [ ] **Step 1: Build the example**

```bash
ninja -C build 13_order_platform
```

- [ ] **Step 2: Two-process happy path**

Terminal 1:
```bash
./13_order_platform --payment --actor-port 17132
```

Terminal 2:
```bash
./13_order_platform --gateway --actor-port 17130 \
    --payment 127.0.0.1:17132 --http-port 18130
```

Then submit an order via curl:
```bash
curl -X POST http://127.0.0.1:18130/orders \
  -H "Content-Type: application/json" \
  -d '{"order_id":"remote-test","customer_id":"cust-1","lines":[{"sku":"sku-book","quantity":1,"unit_cents":1599}]}'
```

Check the gateway terminal for the order completion result.

- [ ] **Step 3: Failure test — payment node not running**

```bash
./13_order_platform --gateway --actor-port 17130 --payment 127.0.0.1:17199
```

Expected: "Failed to remote-spawn PaymentActor: ..." within 5 seconds.

- [ ] **Step 4: Run full test suite**

```bash
ctest --output-on-failure --parallel 8
```

Expected: all 140 existing tests pass (no test changes in this issue).
