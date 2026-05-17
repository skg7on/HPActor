# HTTP Gateway Integration for Order Platform Example — Design

**Date:** 2026-05-17
**Status:** Draft
**Issue:** [#98](https://github.com/skg7on/HPActor/issues/98)
**Dependency on:** #97 (ActorContext::schedule) — not required for gateway itself, but the example's payment-timeout path needs it

---

## Overview

Wire the existing `HTTPGatewayActor` into the order platform example so orders can be submitted via HTTP POST and queried via HTTP GET. This replaces the `--gateway` and `--query` mode stubs with working HTTP ingress/egress, demonstrating the framework's HTTP surface.

## Current State

The order platform example has:
- `--gateway` mode: calls `run_long_role("GATEWAY")` — spawns an `ActorSystem` with network enabled, prints the endpoint, then sits in a sleep loop until Ctrl-C. No actors are spawned, no routes are registered.
- `--query` mode: stub that prints "QUERY would submit demo-order to HTTP port N" and exits.
- `HTTPGatewayActor` fully implemented and tested (`test_http_gateway.cpp`) but never used in any example.
- `HttpClient` implemented but never used in any example.
- `HttpSerializer` available for content negotiation between HTTP bodies and `TypedMessage`.

## Goals

- **`--gateway` mode**: spawn `HTTPGatewayActor`, register `POST /orders` and `GET /orders/{id}` routes, spawn the full order platform actor tree (coordinator + inventory + payment + fulfillment + log), and accept HTTP traffic
- **`--query` mode**: use `HttpClient` to `POST` a demo order to the gateway's HTTP port and print the response
- **Order submission via HTTP**: `POST /orders` with a JSON body containing `order_id`, `customer_id`, `lines`, and optional `scenario` → coordinator processes the order → reply is serialized and sent as HTTP response
- **Order query via HTTP**: `GET /orders/{id}` → coordinator looks up the order → replies with current `OrderStatusPayload` as JSON
- No new external dependencies beyond existing llhttp
- Demonstrate the complete `HTTPGatewayActor` route registration and reply correlation pattern

## Non-Goals

- Full REST API with proper HTTP semantics (PATCH, DELETE, Link headers) — out of scope for the example
- Authentication, rate limiting, or CORS — not relevant for a demo
- HTTPS/TLS on the HTTP gateway — out of scope
- TOML-driven route configuration — routes are registered programmatically for clarity

---

## Architecture

### Component Diagram (--gateway mode)

```
┌─────────────────────────────────────────────────────────┐
│ ActorSystem (gateway node)                               │
│                                                          │
│  ┌──────────────────────┐   ┌─────────────────────────┐ │
│  │ HTTPGatewayActor     │   │ OrderCoordinatorActor   │ │
│  │ (DaemonActor)        │   │ (StatefulActor)         │ │
│  │                       │   │                          │ │
│  │ routes:               │   │ handles:                │ │
│  │  POST /orders ────────┼──▶  SubmitOrderTag          │ │
│  │  GET /orders/:id ─────┼──▶  QueryOrderTag (new)    │ │
│  │                       │   │                          │ │
│  │ port: 18130           │   │ children:               │ │
│  └──────────────────────┘   │  ├─ InventoryActor       │ │
│                              │  ├─ PaymentActor         │ │
│  ┌──────────────────────┐   │  ├─ FulfillmentWorker    │ │
│  │ OrderLogActor        │   │  └─ OrderLogActor        │ │
│  └──────────────────────┘   └─────────────────────────┘ │
│                                                          │
│  [Other roles run on separate ActorSystems/processes]    │
└─────────────────────────────────────────────────────────┘
```

### HTTP Request Flow

```
HTTP Client (curl or --query mode)
    │
    │  POST /orders HTTP/1.1
    │  Content-Type: application/json
    │  {"order_id":"demo-1","customer_id":"cust-1","lines":[...]}
    │
    ▼
HTTPGatewayActor::on_request()
    │
    ├─ routes_.match(POST, "/orders", req) → builder callback
    ├─ builder(req):
    │   ├─ deserialize JSON body → SubmitOrderPayload
    │   ├─ encode SubmitOrderPayload → TypedMessage(SubmitOrderTag)
    │   └─ return {coordinator_address, typed_message}
    ├─ embed 8-byte request_id prefix into message payload
    ├─ deliver_local(coordinator_id, msg)
    └─ register PendingReply{connection, request_id, timeout}

    ... coordinator processes order, replies ...

ReplyAdapter (receives reply TypedMessage)
    │
    ├─ extract 8-byte request_id from payload
    ├─ lookup PendingReply in pending_replies_
    ├─ HttpSerializer::serialize_response(reply, accept_header)
    ├─ gateway_.send_response(conn, 200 OK, json_body)
    └─ return
```

---

## Detailed Design

### 1. Gateway Mode Actor Setup

In `run_gateway(const Options& opts)`:

```cpp
int run_gateway(const Options& opts) {
    Config config = make_base_config(opts, opts.actor_port);
    config.enable_http_gateway = false;  // we spawn manually, not via Config flag
    ActorSystem system(config);

    // Spawn the order platform actor tree (same as all-in-one).
    auto log = system.spawn<OrderLogActor>();
    auto inventory = system.spawn<InventoryActor>();
    auto payment = system.spawn<PaymentActor>();
    auto fulfillment = system.spawn<FulfillmentWorkerActor>();

    // Coordinator with no completion promise — long-running, many orders.
    auto coordinator = system.spawn<OrderCoordinatorActor>(
        inventory.address(), payment.address(),
        fulfillment.address(), log.address(),
        nullptr);  // no completion promise in gateway mode

    // Spawn HTTP gateway and register routes.
    auto gw = system.spawn<net::HTTPGatewayActor>(
        opts.host, opts.http_port);

    // Route: POST /orders → SubmitOrder
    gw->route(HttpMethod::POST, "/orders",
              [coordinator_addr = coordinator.address()]
              (const HttpRequest& req) -> std::pair<ActorAddress, TypedMessage> {
                  SubmitOrderPayload submit = deserialize_submit_order_from_json(req.body);
                  return {coordinator_addr,
                          TypedMessage(SubmitOrderTag, encode_submit_order(submit))};
              });

    // Route: GET /orders/:id → QueryOrder
    gw->route(HttpMethod::GET, "/orders/:id",
              [coordinator_addr = coordinator.address()]
              (const HttpRequest& req) -> std::pair<ActorAddress, TypedMessage> {
                  auto it = req.path_params.find("id");
                  std::string order_id = (it != req.path_params.end()) ? it->second : "";
                  QueryOrderPayload query{order_id};
                  return {coordinator_addr,
                          TypedMessage(QueryOrderTag, encode_query_order(query))};
              });

    std::cout << "GATEWAY listening on http://" << opts.host << ":"
              << opts.http_port << "\n";
    run_until_signal("GATEWAY");
    return 0;
}
```

### 2. OrderCoordinator QueryOrder Handling

Add `QueryOrderTag` handling to `OrderCoordinatorActor::make_behavior()`:

```cpp
} else if (msg.type_id() == QueryOrderTag) {
    QueryOrderPayload query;
    if (!decode_query_order(msg.payload(), query)) return;
    auto it = state().orders.find(query.order_id);
    if (it == state().orders.end()) {
        OrderStatusPayload not_found{query.order_id, OrderStatus::Cancelled,
                                     "not found", 0};
        context()->reply(TypedMessage(OrderStatusTag,
                                      encode_order_status(not_found)));
    } else {
        const auto& record = it->second;
        OrderStatusPayload status{record.order_id, record.status,
                                  record.detail, record.total_cents};
        context()->reply(TypedMessage(OrderStatusTag,
                                      encode_order_status(status)));
    }
}
```

### 3. JSON Serialization (Inline, No Library)

To avoid pulling in a JSON library, use hand-rolled minimal JSON encoding/decoding for the order platform's narrow schema:

```cpp
// Simple JSON helpers for the order platform schema only.
// Not a general-purpose JSON library.

std::string submit_order_to_json(const SubmitOrderPayload& submit) {
    std::string json = "{";
    json += "\"order_id\":\"" + json_escape(submit.order_id) + "\",";
    json += "\"customer_id\":\"" + json_escape(submit.customer_id) + "\",";
    json += "\"scenario\":\"" + std::string(to_string(submit.scenario)) + "\",";
    json += "\"lines\":[";
    for (size_t i = 0; i < submit.lines.size(); ++i) {
        if (i > 0) json += ",";
        json += "{\"sku\":\"" + json_escape(submit.lines[i].sku) + "\",";
        json += "\"quantity\":" + std::to_string(submit.lines[i].quantity) + ",";
        json += "\"unit_cents\":" + std::to_string(submit.lines[i].unit_cents) + "}";
    }
    json += "]}";
    return json;
}
```

### 4. Query Mode Implementation

```cpp
int run_query(const Options& opts) {
    if (!opts.submit_demo_order) {
        std::cerr << "QUERY requires --submit demo-order\n";
        return 1;
    }

    // Build a demo order.
    SubmitOrderPayload submit;
    submit.order_id = "demo-1";
    submit.customer_id = "customer-1";
    submit.scenario = opts.scenario;

    std::string json_body = submit_order_to_json(submit);
    StreamBuffer body(json_body.begin(), json_body.end());

    // Send HTTP POST via HttpClient.
    EventLoop loop;
    HttpClient client(&loop);

    std::string url = "http://" + opts.host + ":" +
                      std::to_string(opts.gateway_port) + "/orders";

    RpcFuture<StreamBuffer> future = client.post(url, body,
        {{"Content-Type", "application/json"}});

    auto result = future.get(std::chrono::seconds(5));
    if (!result) {
        std::cerr << "QUERY failed: " << result.error().message() << "\n";
        return 1;
    }

    std::string response(result->begin(), result->end());
    std::cout << "QUERY response: " << response << "\n";
    return 0;
}
```

Note: `HttpClient` currently does blocking socket I/O. For a CLI tool like `--query`, this is acceptable. The client is short-lived and blocks only for the request duration.

### 5. TypeTag and Payload Additions

New payload types in `messages.hpp`:

```cpp
struct QueryOrderPayload {
    std::string order_id;
};

inline StreamBuffer encode_query_order(const QueryOrderPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    return writer.finish();
}

inline bool decode_query_order(const StreamBuffer& buffer, QueryOrderPayload& out) {
    BufferReader reader(buffer);
    return reader.str(out.order_id) && reader.done();
}
```

`QueryOrderTag` (`0x00020002`) is already defined in `messages.hpp` — no new TypeTag needed.

### 6. HTTP Reply Correlation

The existing `HTTPGatewayActor` already handles reply correlation via an 8-byte `request_id` prefix embedded in the outbound message payload. When the `ReplyAdapter` receives a reply, it:
1. Extracts the first 8 bytes (big-endian `request_id`)
2. Looks up the `PendingReply` in `pending_replies_`
3. Strips the prefix, serializes the remainder via `HttpSerializer`
4. Sends the HTTP response

For the order platform, the reply message's payload must start with the 8-byte prefix. This means the coordinator's `context()->reply()` path will automatically carry the correlation through. No changes needed in the coordinator — `EventBasedActor`'s `receive()` handles preserving the `current_sender_` which includes the reply routing information.

**Important**: `context()->reply()` uses `current_sender_` to route replies. The `HTTPGatewayActor` sets up `PendingReply` with the connection reference, and the 8-byte request_id in the message body is what links the reply back. The hardcoded dispatch in `EventBasedActor::receive()` for system messages (TypeTag < 0x1000) handles the reply routing.

---

## Changes Summary

### Files Modified

| File | Change |
|------|--------|
| `examples/13_order_platform.cpp` | Replace `run_long_role("GATEWAY")` and `run_query()` stubs with real implementations |
| `examples/order_platform/messages.hpp` | Add `QueryOrderPayload` struct + `encode_query_order()`/`decode_query_order()` |
| `tests/examples/test_order_platform_messages.cpp` | Add `test_query_order_round_trip()` |

### Files Referenced (no changes)

| File | Role |
|------|------|
| `include/hpactor/actor/http_gateway_actor.hpp` | `HTTPGatewayActor::route()`, `MessageBuilder` type |
| `include/hpactor/net/http_client.hpp` | `HttpClient::post()` |
| `include/hpactor/net/http_types.hpp` | `HttpRequest`, `HttpResponse`, `HttpMethod` |

---

## Acceptance Criteria

1. `./13_order_platform --gateway --http-port 18130` starts and prints "GATEWAY listening on http://..."
2. `curl -X POST http://127.0.0.1:18130/orders -H "Content-Type: application/json" -d '{"order_id":"test-1","customer_id":"cust-1","lines":[{"sku":"sku-book","quantity":1,"unit_cents":1599}]}'` returns a JSON response with the order status
3. `curl http://127.0.0.1:18130/orders/test-1` returns the current order status as JSON
4. `./13_order_platform --query --gateway-port 18130 --submit demo-order` submits an order via HTTP and prints the response
5. Happy-path order submitted via HTTP completes with status `completed`
6. Existing `test_order_platform_messages` tests continue to pass
7. No new external dependencies

## Test Plan

### Manual integration tests (interactive)

1. Terminal 1: `./13_order_platform --gateway --http-port 18130`
2. Terminal 2: `curl -X POST http://127.0.0.1:18130/orders -d '...'` — verify 200 OK with status JSON
3. Terminal 2: `curl http://127.0.0.1:18130/orders/test-1` — verify status returned
4. Terminal 2: `./13_order_platform --query --gateway-port 18130 --submit demo-order` — verify client output

### Unit test (automated)

- `test_query_order_round_trip()` — serialize/deserialize `QueryOrderPayload`
