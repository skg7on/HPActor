# HTTP Gateway Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the existing `HTTPGatewayActor` into the order platform example so orders can be submitted via HTTP POST and queried via HTTP GET, replacing the `--gateway` and `--query` mode stubs.

**Architecture:** `--gateway` mode spawns the full order platform actor tree plus an `HTTPGatewayActor` with `POST /orders` and `GET /orders/:id` routes registered via `MessageBuilder` lambdas. `--query` mode uses `HttpClient` to POST a demo order. `OrderCoordinator` gains `QueryOrderTag` handling to look up orders by ID and reply with their status.

**Tech Stack:** C++20, existing `HTTPGatewayActor`, `HttpClient`, `HttpSerializer`, hand-rolled minimal JSON for payload bodies, no new dependencies.

**Spec:** `docs/superpowers/specs/2026-05-17-issue-98-http-gateway-integration-design.md`

---

## File Structure

| File | Purpose |
|------|---------|
| `examples/order_platform/messages.hpp` | **Modified** — add `QueryOrderPayload` + encode/decode |
| `examples/13_order_platform.cpp` | **Modified** — replace `run_long_role("GATEWAY")` and `run_query()` stubs; add `QueryOrderTag` handling to `OrderCoordinatorActor`; add minimal JSON helpers |
| `tests/examples/test_order_platform_messages.cpp` | **Modified** — add `test_query_order_round_trip()` |

---

### Task 1: Add QueryOrderPayload Serialization

**File:** `examples/order_platform/messages.hpp`

- [ ] **Step 1: Add payload struct and encode/decode**

Add after `QueryOrderTag` (already defined at line 19):

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

---

### Task 2: Add QueryOrder Round-Trip Test

**File:** `tests/examples/test_order_platform_messages.cpp`

- [ ] **Step 1: Add test function**

```cpp
static void test_query_order_round_trip() {
    QueryOrderPayload in;
    in.order_id = "ord-500";

    auto encoded = encode_query_order(in);
    QueryOrderPayload out;
    assert(decode_query_order(encoded, out));
    assert(out.order_id == "ord-500");

    // Malformed rejected.
    hpactor::StreamBuffer truncated{0x00, 0x00, 0x00, 0x02, 'x'};
    assert(!decode_query_order(truncated, out));
}
```

- [ ] **Step 2: Add to main()**

```cpp
test_query_order_round_trip();
```

---

### Task 3: Add QueryOrderTag Handling to OrderCoordinatorActor

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Add handler in make_behavior()**

In `OrderCoordinatorActor::make_behavior()`, add before the final `return` in the Behavior lambda:

```cpp
} else if (msg.type_id() == order::QueryOrderTag) {
    order::QueryOrderPayload query;
    if (!order::decode_query_order(msg.payload(), query)) return;
    auto it = state().orders.find(query.order_id);
    order::OrderStatusPayload status;
    status.order_id = query.order_id;
    if (it == state().orders.end()) {
        status.status = order::OrderStatus::Cancelled;
        status.detail = "not found";
    } else {
        const auto& record = it->second;
        status.status = record.status;
        status.detail = record.detail;
        status.total_cents = record.total_cents;
    }
    context()->reply(hpactor::TypedMessage(
        order::OrderStatusTag, order::encode_order_status(status)));
```

---

### Task 4: Add Minimal JSON Helpers

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Add JSON serialization for the two payloads**

Add in the anonymous namespace, before the actor classes:

```cpp
// Minimal JSON helpers — only for the order platform's narrow schema.

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string submit_order_to_json(const order::SubmitOrderPayload& submit) {
    std::ostringstream json;
    json << "{\"order_id\":\"" << json_escape(submit.order_id) << "\""
         << ",\"customer_id\":\"" << json_escape(submit.customer_id) << "\""
         << ",\"scenario\":\"" << order::to_string(submit.scenario) << "\""
         << ",\"lines\":[";
    for (size_t i = 0; i < submit.lines.size(); ++i) {
        if (i > 0) json << ",";
        json << "{\"sku\":\"" << json_escape(submit.lines[i].sku) << "\""
             << ",\"quantity\":" << submit.lines[i].quantity
             << ",\"unit_cents\":" << submit.lines[i].unit_cents << "}";
    }
    json << "]}";
    return json.str();
}

std::string order_status_to_json(const order::OrderStatusPayload& status) {
    std::ostringstream json;
    json << "{\"order_id\":\"" << json_escape(status.order_id) << "\""
         << ",\"status\":\"" << order::to_string(status.status) << "\""
         << ",\"detail\":\"" << json_escape(status.detail) << "\""
         << ",\"total_cents\":" << status.total_cents << "}";
    return json.str();
}
```

Note: `<sstream>` is already included in the file.

---

### Task 5: Implement run_gateway() with HTTPGatewayActor

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Replace the `run_long_role("GATEWAY")` call**

Replace the gateway-specific logic with a full implementation (currently `run_long_role` is called for gateway, inventory, payment, fulfillment, ops — we replace only the gateway and add `run_gateway`):

```cpp
int run_gateway(const Options& opts) {
    Config config = make_base_config(opts, opts.actor_port);
    config.enable_network = (opts.actor_port != 0);
    // We spawn the gateway manually, not via the Config flag.
    ActorSystem system(config);

    // Spawn the order platform actor tree.
    auto log = system.spawn<OrderLogActor>();
    auto inventory = system.spawn<InventoryActor>();
    auto payment = system.spawn<PaymentActor>();
    auto fulfillment_worker = system.spawn<FulfillmentWorkerActor>();
    auto coordinator = system.spawn<OrderCoordinatorActor>(
        inventory.address(), payment.address(),
        fulfillment_worker.address(), log.address(),
        nullptr);  // no completion promise — long-running

    // Spawn HTTP gateway.
    auto gw = system.spawn<net::HTTPGatewayActor>(opts.host, opts.http_port);

    auto coordinator_addr = coordinator.address();

    // POST /orders — submit a new order.
    gw->route(HttpMethod::POST, "/orders",
        [coordinator_addr](const HttpRequest& req)
        -> std::pair<ActorAddress, TypedMessage> {
            // Deserialize JSON body into SubmitOrderPayload.
            // For simplicity, parse from the raw body string.
            std::string body_str(req.body.begin(), req.body.end());
            // Minimal JSON parse — extract fields.
            SubmitOrderPayload submit;
            // ... (see note below) ...
            return {coordinator_addr,
                    TypedMessage(SubmitOrderTag, encode_submit_order(submit))};
        });

    // GET /orders/:id — query order status.
    gw->route(HttpMethod::GET, "/orders/:id",
        [coordinator_addr](const HttpRequest& req)
        -> std::pair<ActorAddress, TypedMessage> {
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

- [ ] **Step 2: JSON body parsing in the POST builder**

The `HttpSerializer` handles Content-Type negotiation. For the minimal JSON approach, parse the body manually:

```cpp
// Simple key-value extraction for the known JSON schema.
auto extract_json_string = [](const std::string& json, const std::string& key)
    -> std::string {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
};
```

Full JSON parsing for lines array can be kept minimal since the example schema is known.

---

### Task 6: Implement run_query() with HttpClient

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Replace the run_query() stub**

```cpp
int run_query(const Options& opts) {
    if (!opts.submit_demo_order) {
        std::cerr << "QUERY requires --submit demo-order\n";
        return 1;
    }

    // Build a demo order as JSON.
    SubmitOrderPayload submit;
    submit.order_id = "demo-1";
    submit.customer_id = "customer-1";
    submit.scenario = opts.scenario;
    submit.lines.push_back(OrderLine{"sku-book", 2, 1599});

    std::string json_body = submit_order_to_json(submit);
    StreamBuffer body(json_body.begin(), json_body.end());

    // Create an EventLoop for the HTTP client.
    EventLoop loop;
    HttpClient client(&loop);

    std::string url = "http://" + opts.host + ":" +
                      std::to_string(opts.gateway_port) + "/orders";

    auto future = client.post(url, body,
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

- [ ] **Step 2: Update main() dispatch to call run_gateway() instead of run_long_role**

In `main()`, change:

```cpp
if (opts->mode == "--gateway")
    return run_gateway(*opts);  // was: run_long_role(*opts, "GATEWAY");
```

---

### Task 7: Build, Test, and Manual Verification

- [ ] **Step 1: Build the example**

```bash
ninja -C build 13_order_platform
```

- [ ] **Step 2: Run unit tests**

```bash
./build/tests/test_order_platform_messages
```

Expected: 8 assertions pass (7 existing + 1 new query round-trip).

- [ ] **Step 3: Manual integration test — gateway + curl**

Terminal 1:
```bash
./13_order_platform --gateway --http-port 18130 --scenario happy-path
```

Terminal 2:
```bash
curl -X POST http://127.0.0.1:18130/orders \
  -H "Content-Type: application/json" \
  -d '{"order_id":"test-1","customer_id":"cust-1","lines":[{"sku":"sku-book","quantity":1,"unit_cents":1599}]}'
```

Expected: HTTP 200 with JSON response containing order status.

Terminal 2:
```bash
curl http://127.0.0.1:18130/orders/test-1
```

Expected: JSON with current status of "test-1".

- [ ] **Step 4: Run full test suite**

```bash
ctest --output-on-failure --parallel 8
```

Expected: all 140 existing tests pass.
