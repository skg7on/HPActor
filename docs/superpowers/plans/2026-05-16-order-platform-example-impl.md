# Order Platform Example Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `13_order_platform`, a multi-role end-to-end HPActor example that demonstrates the framework's actor, networking, scheduling, mailbox, DLQ, CLI, metrics, tracing, logging, topology, remote spawn, and RPC surfaces through an order-processing app.

**Architecture:** Add a focused message/payload helper header, one advanced example executable, role-specific TOML configs, and a small serializer unit test. The executable starts the same actor graph in all-in-one mode, or selected role subsets in multi-process mode; the first distributed path remote-spawns payment workers and keeps inventory/fulfillment local for the initial smoke path.

**Tech Stack:** C++20, HPActor public APIs, CMake/Ninja, CTest, existing HTTP gateway, existing TCP transport/static routes, existing remote spawn/RPC, existing TOML topology config, no new external dependencies.

---

## File Structure

- Create: `examples/order_platform/messages.hpp`
  - Owns application `TypeTag` constants, payload structs, string/binary serialization helpers, order status/scenario enums, and helper conversions. This file is tested directly.
- Create: `examples/13_order_platform.cpp`
  - Owns command-line parsing, role runners, business actors, HTTP route registration, distributed payment setup, scenario execution, and transcript output.
- Create: `examples/config/order_platform_all_in_one.toml`
  - Enables scheduler, metrics, CLI, logging, DLQ, tracing config, HTTP gateway, mailbox defaults, and all-in-one actor topology metadata.
- Create: `examples/config/order_gateway.toml`
  - Gateway/coordinator role config.
- Create: `examples/config/order_inventory.toml`
  - Inventory role config.
- Create: `examples/config/order_payment.toml`
  - Payment role config and network port.
- Create: `examples/config/order_fulfillment.toml`
  - Fulfillment role config.
- Create: `examples/config/order_ops.toml`
  - Ops role config with CLI, metrics, logging, and DLQ.
- Create: `tests/examples/test_order_platform_messages.cpp`
  - Covers payload round trips, enum conversion, malformed decode rejection, and status formatting.
- Modify: `examples/CMakeLists.txt`
  - Add `13_order_platform`.
- Modify: `tests/CMakeLists.txt`
  - Add `test_order_platform_messages`.

Keep actors in `examples/13_order_platform.cpp` unless the implementation becomes difficult to navigate. The only planned helper header is `messages.hpp` because serialization needs focused tests.

## Task 1: Message Tags, Payload Helpers, And Tests

**Files:**
- Create: `examples/order_platform/messages.hpp`
- Create: `tests/examples/test_order_platform_messages.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add the failing serializer test**

Create `tests/examples/test_order_platform_messages.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <examples/order_platform/messages.hpp>

#include <cassert>
#include <string>
#include <vector>

using namespace hpactor::examples::order_platform;

static void test_submit_order_round_trip() {
    SubmitOrderPayload in;
    in.order_id = "ord-100";
    in.customer_id = "cust-7";
    in.scenario = ScenarioKind::HappyPath;
    in.lines.push_back(OrderLine{"sku-book", 2, 1599});
    in.lines.push_back(OrderLine{"sku-pen", 3, 250});

    auto encoded = encode_submit_order(in);
    SubmitOrderPayload out;
    assert(decode_submit_order(encoded, out));
    assert(out.order_id == "ord-100");
    assert(out.customer_id == "cust-7");
    assert(out.scenario == ScenarioKind::HappyPath);
    assert(out.lines.size() == 2);
    assert(out.lines[0].sku == "sku-book");
    assert(out.lines[0].quantity == 2);
    assert(out.lines[0].unit_cents == 1599);
    assert(out.lines[1].sku == "sku-pen");
    assert(out.lines[1].quantity == 3);
    assert(out.lines[1].unit_cents == 250);
}

static void test_status_round_trip() {
    OrderStatusPayload in;
    in.order_id = "ord-200";
    in.status = OrderStatus::PaymentFailed;
    in.detail = "card declined";
    in.total_cents = 4242;

    auto encoded = encode_order_status(in);
    OrderStatusPayload out;
    assert(decode_order_status(encoded, out));
    assert(out.order_id == "ord-200");
    assert(out.status == OrderStatus::PaymentFailed);
    assert(out.detail == "card declined");
    assert(out.total_cents == 4242);
    assert(to_string(out.status) == std::string("payment_failed"));
}

static void test_inventory_round_trip() {
    InventoryReservePayload in;
    in.order_id = "ord-300";
    in.lines.push_back(OrderLine{"sku-lamp", 1, 3200});

    auto encoded = encode_inventory_reserve(in);
    InventoryReservePayload out;
    assert(decode_inventory_reserve(encoded, out));
    assert(out.order_id == "ord-300");
    assert(out.lines.size() == 1);
    assert(out.lines[0].sku == "sku-lamp");
}

static void test_payment_round_trip() {
    PaymentAuthorizePayload in;
    in.order_id = "ord-400";
    in.customer_id = "cust-9";
    in.amount_cents = 9900;
    in.scenario = ScenarioKind::PaymentDecline;

    auto encoded = encode_payment_authorize(in);
    PaymentAuthorizePayload out;
    assert(decode_payment_authorize(encoded, out));
    assert(out.order_id == "ord-400");
    assert(out.customer_id == "cust-9");
    assert(out.amount_cents == 9900);
    assert(out.scenario == ScenarioKind::PaymentDecline);
}

static void test_malformed_decode_rejected() {
    hpactor::StreamBuffer truncated{0x00, 0x00, 0x00, 0x04, 'o'};
    SubmitOrderPayload out;
    assert(!decode_submit_order(truncated, out));
}

static void test_scenario_from_string() {
    assert(scenario_from_string("happy-path") == ScenarioKind::HappyPath);
    assert(scenario_from_string("insufficient-stock") ==
           ScenarioKind::InsufficientStock);
    assert(scenario_from_string("payment-decline") ==
           ScenarioKind::PaymentDecline);
    assert(scenario_from_string("payment-timeout") ==
           ScenarioKind::PaymentTimeout);
    assert(scenario_from_string("worker-crash") == ScenarioKind::WorkerCrash);
    assert(scenario_from_string("overload") == ScenarioKind::Overload);
    assert(scenario_from_string("missing-route") == ScenarioKind::MissingRoute);
    assert(scenario_from_string("unknown") == ScenarioKind::HappyPath);
}

int main() {
    test_submit_order_round_trip();
    test_status_round_trip();
    test_inventory_round_trip();
    test_payment_round_trip();
    test_malformed_decode_rejected();
    test_scenario_from_string();
    return 0;
}
```

- [ ] **Step 2: Register the failing test target**

Append this block to `tests/CMakeLists.txt` near the other subsystem sections:

```cmake
# =============================================================================
# Example support tests
# =============================================================================
add_executable(test_order_platform_messages examples/test_order_platform_messages.cpp)
target_link_libraries(test_order_platform_messages hpactor)
target_include_directories(test_order_platform_messages PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME test_order_platform_messages COMMAND test_order_platform_messages)
```

- [ ] **Step 3: Run the test build and verify it fails for the missing header**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build test_order_platform_messages
```

Expected: `ninja` fails with an error containing:

```text
examples/order_platform/messages.hpp: No such file or directory
```

- [ ] **Step 4: Add the message helper header**

Create `examples/order_platform/messages.hpp` with this structure and exact public names:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor::examples::order_platform {

inline constexpr TypeTag SubmitOrderTag{0x00020000};
inline constexpr TypeTag OrderAcceptedTag{0x00020001};
inline constexpr TypeTag QueryOrderTag{0x00020002};
inline constexpr TypeTag OrderStatusTag{0x00020003};
inline constexpr TypeTag ReserveInventoryTag{0x00020004};
inline constexpr TypeTag InventoryReservedTag{0x00020005};
inline constexpr TypeTag InventoryRejectedTag{0x00020006};
inline constexpr TypeTag ReleaseInventoryTag{0x00020007};
inline constexpr TypeTag AuthorizePaymentTag{0x00020008};
inline constexpr TypeTag PaymentAuthorizedTag{0x00020009};
inline constexpr TypeTag PaymentDeclinedTag{0x0002000A};
inline constexpr TypeTag PaymentTimedOutTag{0x0002000B};
inline constexpr TypeTag QueueFulfillmentTag{0x0002000C};
inline constexpr TypeTag FulfillmentQueuedTag{0x0002000D};
inline constexpr TypeTag FulfillmentFailedTag{0x0002000E};
inline constexpr TypeTag LogOrderEventTag{0x0002000F};
inline constexpr TypeTag ScenarioKickTag{0x00020010};
inline constexpr TypeTag OpsProbeTickTag{0x00020011};
inline constexpr TypeTag PricingRequestTag{0x00020012};
inline constexpr TypeTag PricingReplyTag{0x00020013};

enum class ScenarioKind : uint8_t {
    HappyPath = 0,
    InsufficientStock,
    PaymentDecline,
    PaymentTimeout,
    WorkerCrash,
    Overload,
    MissingRoute,
};

enum class OrderStatus : uint8_t {
    Received = 0,
    Priced,
    InventoryReserved,
    PaymentAuthorized,
    FulfillmentQueued,
    Completed,
    Rejected,
    Cancelled,
    InventoryFailed,
    PaymentFailed,
    PaymentTimedOut,
    FulfillmentFailed,
    Overloaded,
};

struct OrderLine {
    std::string sku;
    uint32_t quantity = 0;
    uint64_t unit_cents = 0;
};

struct SubmitOrderPayload {
    std::string order_id;
    std::string customer_id;
    std::vector<OrderLine> lines;
    ScenarioKind scenario = ScenarioKind::HappyPath;
};

struct OrderStatusPayload {
    std::string order_id;
    OrderStatus status = OrderStatus::Received;
    std::string detail;
    uint64_t total_cents = 0;
};

struct InventoryReservePayload {
    std::string order_id;
    std::vector<OrderLine> lines;
};

struct InventoryReplyPayload {
    std::string order_id;
    bool ok = false;
    uint64_t reservation_id = 0;
    std::string detail;
};

struct PaymentAuthorizePayload {
    std::string order_id;
    std::string customer_id;
    uint64_t amount_cents = 0;
    ScenarioKind scenario = ScenarioKind::HappyPath;
};

struct PaymentReplyPayload {
    std::string order_id;
    bool ok = false;
    std::string authorization_id;
    std::string detail;
};

struct FulfillmentPayload {
    std::string order_id;
    uint64_t reservation_id = 0;
    ScenarioKind scenario = ScenarioKind::HappyPath;
};

struct PricingRequest {
    std::string order_id;
    std::vector<OrderLine> lines;
};

struct PricingReply {
    std::string order_id;
    uint64_t subtotal_cents = 0;
    uint64_t discount_cents = 0;
    uint64_t tax_cents = 0;
    uint64_t total_cents = 0;
};

class BufferWriter {
  public:
    void u8(uint8_t value) { buffer_.push_back(value); }

    void u32(uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            buffer_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
        }
    }

    void u64(uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
        }
    }

    void str(const std::string& value) {
        u32(static_cast<uint32_t>(value.size()));
        buffer_.insert(buffer_.end(), value.begin(), value.end());
    }

    StreamBuffer finish() { return std::move(buffer_); }

  private:
    StreamBuffer buffer_;
};

class BufferReader {
  public:
    explicit BufferReader(const StreamBuffer& buffer) : buffer_(buffer) {}

    bool u8(uint8_t& value) {
        if (offset_ + 1 > buffer_.size()) return false;
        value = buffer_[offset_++];
        return true;
    }

    bool u32(uint32_t& value) {
        if (offset_ + 4 > buffer_.size()) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            value = (value << 8) | buffer_[offset_++];
        }
        return true;
    }

    bool u64(uint64_t& value) {
        if (offset_ + 8 > buffer_.size()) return false;
        value = 0;
        for (int i = 0; i < 8; ++i) {
            value = (value << 8) | buffer_[offset_++];
        }
        return true;
    }

    bool str(std::string& value) {
        uint32_t size = 0;
        if (!u32(size)) return false;
        if (offset_ + size > buffer_.size()) return false;
        value.assign(reinterpret_cast<const char*>(buffer_.data() + offset_), size);
        offset_ += size;
        return true;
    }

    bool done() const { return offset_ == buffer_.size(); }

  private:
    const StreamBuffer& buffer_;
    size_t offset_ = 0;
};

inline const char* to_string(ScenarioKind value) {
    switch (value) {
        case ScenarioKind::HappyPath: return "happy-path";
        case ScenarioKind::InsufficientStock: return "insufficient-stock";
        case ScenarioKind::PaymentDecline: return "payment-decline";
        case ScenarioKind::PaymentTimeout: return "payment-timeout";
        case ScenarioKind::WorkerCrash: return "worker-crash";
        case ScenarioKind::Overload: return "overload";
        case ScenarioKind::MissingRoute: return "missing-route";
    }
    return "happy-path";
}

inline ScenarioKind scenario_from_string(std::string_view value) {
    if (value == "insufficient-stock") return ScenarioKind::InsufficientStock;
    if (value == "payment-decline") return ScenarioKind::PaymentDecline;
    if (value == "payment-timeout") return ScenarioKind::PaymentTimeout;
    if (value == "worker-crash") return ScenarioKind::WorkerCrash;
    if (value == "overload") return ScenarioKind::Overload;
    if (value == "missing-route") return ScenarioKind::MissingRoute;
    return ScenarioKind::HappyPath;
}

inline const char* to_string(OrderStatus value) {
    switch (value) {
        case OrderStatus::Received: return "received";
        case OrderStatus::Priced: return "priced";
        case OrderStatus::InventoryReserved: return "inventory_reserved";
        case OrderStatus::PaymentAuthorized: return "payment_authorized";
        case OrderStatus::FulfillmentQueued: return "fulfillment_queued";
        case OrderStatus::Completed: return "completed";
        case OrderStatus::Rejected: return "rejected";
        case OrderStatus::Cancelled: return "cancelled";
        case OrderStatus::InventoryFailed: return "inventory_failed";
        case OrderStatus::PaymentFailed: return "payment_failed";
        case OrderStatus::PaymentTimedOut: return "payment_timed_out";
        case OrderStatus::FulfillmentFailed: return "fulfillment_failed";
        case OrderStatus::Overloaded: return "overloaded";
    }
    return "received";
}

inline void encode_lines(BufferWriter& writer, const std::vector<OrderLine>& lines) {
    writer.u32(static_cast<uint32_t>(lines.size()));
    for (const auto& line : lines) {
        writer.str(line.sku);
        writer.u32(line.quantity);
        writer.u64(line.unit_cents);
    }
}

inline bool decode_lines(BufferReader& reader, std::vector<OrderLine>& lines) {
    uint32_t count = 0;
    if (!reader.u32(count)) return false;
    lines.clear();
    lines.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        OrderLine line;
        if (!reader.str(line.sku)) return false;
        if (!reader.u32(line.quantity)) return false;
        if (!reader.u64(line.unit_cents)) return false;
        lines.push_back(std::move(line));
    }
    return true;
}

inline StreamBuffer encode_submit_order(const SubmitOrderPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.str(payload.customer_id);
    writer.u8(static_cast<uint8_t>(payload.scenario));
    encode_lines(writer, payload.lines);
    return writer.finish();
}

inline bool decode_submit_order(const StreamBuffer& buffer, SubmitOrderPayload& out) {
    BufferReader reader(buffer);
    uint8_t scenario = 0;
    if (!reader.str(out.order_id)) return false;
    if (!reader.str(out.customer_id)) return false;
    if (!reader.u8(scenario)) return false;
    out.scenario = static_cast<ScenarioKind>(scenario);
    if (!decode_lines(reader, out.lines)) return false;
    return reader.done();
}

inline StreamBuffer encode_order_status(const OrderStatusPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.u8(static_cast<uint8_t>(payload.status));
    writer.str(payload.detail);
    writer.u64(payload.total_cents);
    return writer.finish();
}

inline bool decode_order_status(const StreamBuffer& buffer, OrderStatusPayload& out) {
    BufferReader reader(buffer);
    uint8_t status = 0;
    if (!reader.str(out.order_id)) return false;
    if (!reader.u8(status)) return false;
    out.status = static_cast<OrderStatus>(status);
    if (!reader.str(out.detail)) return false;
    if (!reader.u64(out.total_cents)) return false;
    return reader.done();
}

inline StreamBuffer encode_inventory_reserve(const InventoryReservePayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    encode_lines(writer, payload.lines);
    return writer.finish();
}

inline bool decode_inventory_reserve(const StreamBuffer& buffer,
                                     InventoryReservePayload& out) {
    BufferReader reader(buffer);
    if (!reader.str(out.order_id)) return false;
    if (!decode_lines(reader, out.lines)) return false;
    return reader.done();
}

inline StreamBuffer encode_inventory_reply(const InventoryReplyPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.u8(payload.ok ? 1 : 0);
    writer.u64(payload.reservation_id);
    writer.str(payload.detail);
    return writer.finish();
}

inline bool decode_inventory_reply(const StreamBuffer& buffer, InventoryReplyPayload& out) {
    BufferReader reader(buffer);
    uint8_t ok = 0;
    if (!reader.str(out.order_id)) return false;
    if (!reader.u8(ok)) return false;
    out.ok = ok != 0;
    if (!reader.u64(out.reservation_id)) return false;
    if (!reader.str(out.detail)) return false;
    return reader.done();
}

inline StreamBuffer encode_payment_authorize(const PaymentAuthorizePayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.str(payload.customer_id);
    writer.u64(payload.amount_cents);
    writer.u8(static_cast<uint8_t>(payload.scenario));
    return writer.finish();
}

inline bool decode_payment_authorize(const StreamBuffer& buffer,
                                     PaymentAuthorizePayload& out) {
    BufferReader reader(buffer);
    uint8_t scenario = 0;
    if (!reader.str(out.order_id)) return false;
    if (!reader.str(out.customer_id)) return false;
    if (!reader.u64(out.amount_cents)) return false;
    if (!reader.u8(scenario)) return false;
    out.scenario = static_cast<ScenarioKind>(scenario);
    return reader.done();
}

inline StreamBuffer encode_payment_reply(const PaymentReplyPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.u8(payload.ok ? 1 : 0);
    writer.str(payload.authorization_id);
    writer.str(payload.detail);
    return writer.finish();
}

inline bool decode_payment_reply(const StreamBuffer& buffer, PaymentReplyPayload& out) {
    BufferReader reader(buffer);
    uint8_t ok = 0;
    if (!reader.str(out.order_id)) return false;
    if (!reader.u8(ok)) return false;
    out.ok = ok != 0;
    if (!reader.str(out.authorization_id)) return false;
    if (!reader.str(out.detail)) return false;
    return reader.done();
}

inline StreamBuffer encode_fulfillment(const FulfillmentPayload& payload) {
    BufferWriter writer;
    writer.str(payload.order_id);
    writer.u64(payload.reservation_id);
    writer.u8(static_cast<uint8_t>(payload.scenario));
    return writer.finish();
}

inline bool decode_fulfillment(const StreamBuffer& buffer, FulfillmentPayload& out) {
    BufferReader reader(buffer);
    uint8_t scenario = 0;
    if (!reader.str(out.order_id)) return false;
    if (!reader.u64(out.reservation_id)) return false;
    if (!reader.u8(scenario)) return false;
    out.scenario = static_cast<ScenarioKind>(scenario);
    return reader.done();
}

inline uint64_t calculate_subtotal(const std::vector<OrderLine>& lines) {
    uint64_t total = 0;
    for (const auto& line : lines) {
        total += static_cast<uint64_t>(line.quantity) * line.unit_cents;
    }
    return total;
}

} // namespace hpactor::examples::order_platform
```

- [ ] **Step 5: Run the serializer test**

Run:

```bash
ninja -C build test_order_platform_messages
ctest --test-dir build -R test_order_platform_messages --output-on-failure
```

Expected:

```text
100% tests passed
```

- [ ] **Step 6: Commit**

Run:

```bash
git add examples/order_platform/messages.hpp tests/examples/test_order_platform_messages.cpp tests/CMakeLists.txt
git commit -m "test: add order platform message helpers"
```

## Task 2: Executable Scaffold, CLI Options, And CMake Target

**Files:**
- Create: `examples/13_order_platform.cpp`
- Modify: `examples/CMakeLists.txt`

- [ ] **Step 1: Add the executable target before the source exists**

Append this block to `examples/CMakeLists.txt`:

```cmake
add_executable(13_order_platform 13_order_platform.cpp)
target_link_libraries(13_order_platform PRIVATE hpactor_lib)
target_include_directories(13_order_platform PRIVATE ${CMAKE_SOURCE_DIR})
```

- [ ] **Step 2: Run build and verify the source-file failure**

Run:

```bash
ninja -C build 13_order_platform
```

Expected: build fails with an error containing:

```text
13_order_platform.cpp
```

- [ ] **Step 3: Create the scaffold source**

Create `examples/13_order_platform.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <examples/order_platform/messages.hpp>

#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/registrar.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace order = hpactor::examples::order_platform;

namespace {

struct Options {
    std::string mode = "--help";
    order::ScenarioKind scenario = order::ScenarioKind::HappyPath;
    std::string host = "127.0.0.1";
    std::string registrar_host = "127.0.0.1";
    std::string payment_endpoint;
    uint16_t actor_port = 17130;
    uint16_t http_port = 18130;
    uint16_t registrar_port = 19153;
    uint16_t gateway_port = 18130;
    bool submit_demo_order = false;
};

std::atomic<bool> shutdown_requested{false};

void sigint_handler(int) {
    shutdown_requested.store(true, std::memory_order_release);
}

bool parse_port(const std::string& value, uint16_t& port) {
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 || parsed > 65535) {
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return true;
}

void print_usage(const char* argv0) {
    std::cout
        << "HPActor Example 13: Multi-Role Order Platform\n\n"
        << "Quickstart:\n"
        << "  " << argv0 << " --all-in-one --scenario happy-path\n\n"
        << "Distributed:\n"
        << "  " << argv0 << " --payment --actor-port 17132\n"
        << "  " << argv0 << " --gateway --actor-port 17130 --http-port 18130 "
        << "--payment 127.0.0.1:17132\n"
        << "  " << argv0 << " --query --gateway-port 18130 --submit demo-order\n\n"
        << "Failure scenarios:\n"
        << "  " << argv0 << " --all-in-one --scenario overload\n"
        << "  " << argv0 << " --all-in-one --scenario payment-decline\n";
}

std::optional<Options> parse_args(int argc, char* argv[]) {
    Options opts;
    if (argc <= 1) return opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << flag << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--payment" && opts.mode == "--gateway" && i + 1 < argc &&
            argv[i + 1][0] != '-') {
            opts.payment_endpoint = argv[++i];
        } else if (arg == "--all-in-one" || arg == "--gateway" ||
                   arg == "--inventory" || arg == "--payment" ||
                   arg == "--fulfillment" || arg == "--ops" ||
                   arg == "--query" || arg == "--help") {
            opts.mode = arg;
        } else if (arg == "--scenario") {
            const char* value = need_value("--scenario");
            if (value == nullptr) return std::nullopt;
            opts.scenario = order::scenario_from_string(value);
        } else if (arg == "--actor-port") {
            const char* value = need_value("--actor-port");
            if (value == nullptr || !parse_port(value, opts.actor_port)) {
                std::cerr << "invalid --actor-port\n";
                return std::nullopt;
            }
        } else if (arg == "--http-port") {
            const char* value = need_value("--http-port");
            if (value == nullptr || !parse_port(value, opts.http_port)) {
                std::cerr << "invalid --http-port\n";
                return std::nullopt;
            }
        } else if (arg == "--gateway-port") {
            const char* value = need_value("--gateway-port");
            if (value == nullptr || !parse_port(value, opts.gateway_port)) {
                std::cerr << "invalid --gateway-port\n";
                return std::nullopt;
            }
        } else if (arg == "--registrar-port") {
            const char* value = need_value("--registrar-port");
            if (value == nullptr || !parse_port(value, opts.registrar_port)) {
                std::cerr << "invalid --registrar-port\n";
                return std::nullopt;
            }
        } else if (arg == "--registrar-host") {
            const char* value = need_value("--registrar-host");
            if (value == nullptr) return std::nullopt;
            opts.registrar_host = value;
        } else if (arg == "--submit") {
            const char* value = need_value("--submit");
            if (value == nullptr) return std::nullopt;
            opts.submit_demo_order = std::string(value) == "demo-order";
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return std::nullopt;
        }
    }

    return opts;
}

hpactor::Config make_base_config(const Options& opts, uint16_t actor_port) {
    hpactor::Config config;
    config.scheduler_threads = 4;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint(
        opts.host + ":" + std::to_string(actor_port));
    config.tcp_port = actor_port;
    config.enable_network = actor_port != 0;
    config.registrar.tcp_port = opts.registrar_port;
    config.registrar.udp_port = opts.registrar_port;
    config.mailbox.default_capacity =
        opts.scenario == order::ScenarioKind::Overload ? 2 : 1024;
    config.dead_letters.enabled = true;
    config.dead_letters.capacity = 128;
    config.tracing.enabled = true;
    config.tracing.exporter = hpactor::tracing::TraceExporterKind::kJsonFile;
    config.tracing.json_file_path = "build/order-platform-traces.jsonl";
    config.cli.enabled = opts.mode == "--all-in-one" || opts.mode == "--ops";
    return config;
}

void run_until_signal(const char* role) {
    std::signal(SIGINT, sigint_handler);
    std::cout << role << " running. Press Ctrl-C to stop.\n";
    while (!shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int run_all_in_one(const Options& opts) {
    hpactor::ActorSystem system(make_base_config(opts, 0));
    std::cout << "ALL-IN-ONE scenario=" << order::to_string(opts.scenario) << "\n";
    std::cout << "actor_count=" << system.actor_count() << "\n";
    return 0;
}

int run_long_role(const Options& opts, const char* role) {
    hpactor::ActorSystem system(make_base_config(opts, opts.actor_port));
    std::cout << role << " endpoint="
              << hpactor::endpoint_ops::to_string(system.endpoint()) << "\n";
    run_until_signal(role);
    return 0;
}

int run_query(const Options& opts) {
    if (!opts.submit_demo_order) {
        std::cout << "QUERY requires --submit demo-order\n";
        return 1;
    }
    std::cout << "QUERY would submit demo-order to HTTP port "
              << opts.gateway_port << "\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);
    if (!opts.has_value() || opts->mode == "--help") {
        print_usage(argv[0]);
        return opts.has_value() ? 0 : 1;
    }

    if (opts->mode == "--all-in-one") return run_all_in_one(*opts);
    if (opts->mode == "--query") return run_query(*opts);
    if (opts->mode == "--gateway") return run_long_role(*opts, "GATEWAY");
    if (opts->mode == "--inventory") return run_long_role(*opts, "INVENTORY");
    if (opts->mode == "--payment") return run_long_role(*opts, "PAYMENT");
    if (opts->mode == "--fulfillment") return run_long_role(*opts, "FULFILLMENT");
    if (opts->mode == "--ops") return run_long_role(*opts, "OPS");

    print_usage(argv[0]);
    return 1;
}
```

- [ ] **Step 4: Build and run the scaffold**

Run:

```bash
ninja -C build 13_order_platform
./build/examples/13_order_platform --help
./build/examples/13_order_platform --all-in-one --scenario happy-path
```

Expected:

```text
HPActor Example 13: Multi-Role Order Platform
ALL-IN-ONE scenario=happy-path
```

- [ ] **Step 5: Commit**

Run:

```bash
git add examples/13_order_platform.cpp examples/CMakeLists.txt
git commit -m "feat: scaffold order platform example"
```

## Task 3: Core Actor State And All-In-One Happy Path

**Files:**
- Modify: `examples/13_order_platform.cpp`

- [ ] **Step 1: Add the all-in-one smoke expectation before actors exist**

Run:

```bash
./build/examples/13_order_platform --all-in-one --scenario happy-path
```

Expected current output contains:

```text
ALL-IN-ONE scenario=happy-path
```

Expected missing output that this task must add:

```text
SCENARIO RESULT order_id=demo-1 status=completed
```

- [ ] **Step 2: Add shared state structs near the top of `13_order_platform.cpp`**

Insert after `namespace {`:

```cpp
struct OrderRecord {
    std::string order_id;
    std::string customer_id;
    order::ScenarioKind scenario = order::ScenarioKind::HappyPath;
    order::OrderStatus status = order::OrderStatus::Received;
    std::vector<order::OrderLine> lines;
    uint64_t subtotal_cents = 0;
    uint64_t discount_cents = 0;
    uint64_t tax_cents = 0;
    uint64_t total_cents = 0;
    uint64_t reservation_id = 0;
    std::string detail;
};

struct OrderCoordinatorState {
    std::unordered_map<std::string, OrderRecord> orders;
    uint64_t processed = 0;
};

struct InventoryState {
    std::unordered_map<std::string, uint32_t> stock;
    std::unordered_map<uint64_t, order::InventoryReservePayload> reservations;
    uint64_t next_reservation_id = 1;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t released = 0;
};

struct OrderLogState {
    static constexpr size_t kCapacity = 64;
    std::deque<std::string> entries;
    void append(std::string value) {
        if (entries.size() == kCapacity) entries.pop_front();
        entries.push_back(std::move(value));
    }
};
```

Add includes:

```cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/stateful_actor.hpp>
#include <hpactor/actor/typed_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/cli/cli_types.hpp>

#include <chrono>
#include <deque>
#include <future>
#include <sstream>
#include <unordered_map>
```

- [ ] **Step 3: Add `OrderLogActor`**

Insert before the runner functions:

```cpp
class OrderLogActor : public hpactor::StatefulActor<OrderLogState> {
  public:
    static constexpr const char* kActorTypeName = "OrderLogActor";

    OrderLogActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::StatefulActor<OrderLogState>(ctx, sys) {
        become(make_behavior());
    }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta meta;
        meta.actor_id = id().value();
        meta.actor_type = kActorTypeName;
        meta.state = "active";
        meta.messages_processed = processed_;
        meta.behavior_name = "ring-log";
        return meta;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream out;
        out << "entries=" << state().entries.size();
        for (const auto& entry : state().entries) {
            out << "\n" << entry;
        }
        auto text = out.str();
        return {text.begin(), text.end()};
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == order::LogOrderEventTag) {
                state().append(std::string(msg.payload().begin(), msg.payload().end()));
                ++processed_;
            }
        }};
    }

  private:
    uint64_t processed_ = 0;
};
```

- [ ] **Step 4: Add `InventoryActor`**

Insert after `OrderLogActor`:

```cpp
class InventoryActor : public hpactor::StatefulActor<InventoryState> {
  public:
    static constexpr const char* kActorTypeName = "InventoryActor";

    InventoryActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::StatefulActor<InventoryState>(ctx, sys) {
        state().stock.emplace("sku-book", 100);
        state().stock.emplace("sku-pen", 200);
        state().stock.emplace("sku-lamp", 5);
        become(make_behavior());
    }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta meta;
        meta.actor_id = id().value();
        meta.actor_type = kActorTypeName;
        meta.state = "active";
        meta.messages_processed = state().accepted + state().rejected + state().released;
        meta.behavior_name = "inventory";
        return meta;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream out;
        out << "accepted=" << state().accepted
            << " rejected=" << state().rejected
            << " released=" << state().released;
        for (const auto& [sku, qty] : state().stock) {
            out << "\n" << sku << "=" << qty;
        }
        auto text = out.str();
        return {text.begin(), text.end()};
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == order::ReserveInventoryTag) {
                order::InventoryReservePayload payload;
                if (!order::decode_inventory_reserve(msg.payload(), payload)) return;

                bool enough = true;
                for (const auto& line : payload.lines) {
                    auto it = state().stock.find(line.sku);
                    if (it == state().stock.end() || it->second < line.quantity) {
                        enough = false;
                        break;
                    }
                }

                order::InventoryReplyPayload reply;
                reply.order_id = payload.order_id;
                if (enough) {
                    for (const auto& line : payload.lines) {
                        state().stock[line.sku] -= line.quantity;
                    }
                    reply.ok = true;
                    reply.reservation_id = state().next_reservation_id++;
                    reply.detail = "reserved";
                    state().reservations.emplace(reply.reservation_id, payload);
                    ++state().accepted;
                    context()->reply(hpactor::TypedMessage(
                        order::InventoryReservedTag, order::encode_inventory_reply(reply)));
                } else {
                    reply.ok = false;
                    reply.detail = "insufficient stock";
                    ++state().rejected;
                    context()->reply(hpactor::TypedMessage(
                        order::InventoryRejectedTag, order::encode_inventory_reply(reply)));
                }
            }
        }};
    }
};
```

- [ ] **Step 5: Add `PricingActor` typed actor**

Insert after `InventoryActor`:

```cpp
class PricingActor
    : public hpactor::TypedEventBasedActor<
          hpactor::result<order::PricingReply>(order::PricingRequest)> {
  public:
    static constexpr const char* kActorTypeName = "PricingActor";

    PricingActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::TypedEventBasedActor<
              hpactor::result<order::PricingReply>(order::PricingRequest)>(ctx, sys) {
        become(make_behavior());
    }

  protected:
    behavior_type make_behavior() override {
        behavior_type behavior;
        behavior.on([](order::PricingRequest req) -> order::PricingReply {
            order::PricingReply reply;
            reply.order_id = req.order_id;
            reply.subtotal_cents = order::calculate_subtotal(req.lines);
            reply.discount_cents = reply.subtotal_cents >= 5000 ? 500 : 0;
            reply.tax_cents = (reply.subtotal_cents - reply.discount_cents) / 10;
            reply.total_cents =
                reply.subtotal_cents - reply.discount_cents + reply.tax_cents;
            return reply;
        });
        return behavior;
    }
};
```

- [ ] **Step 6: Add `PaymentActor`, `FulfillmentWorkerActor`, and `FulfillmentRouterActor`**

Insert after `PricingActor`:

```cpp
class PaymentActor : public hpactor::EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "PaymentActor";

    PaymentActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() != order::AuthorizePaymentTag) return;
            order::PaymentAuthorizePayload payload;
            if (!order::decode_payment_authorize(msg.payload(), payload)) return;

            if (payload.scenario == order::ScenarioKind::PaymentDecline) {
                order::PaymentReplyPayload reply{payload.order_id, false, "", "card declined"};
                context()->reply(hpactor::TypedMessage(
                    order::PaymentDeclinedTag, order::encode_payment_reply(reply)));
                return;
            }
            if (payload.scenario == order::ScenarioKind::PaymentTimeout) {
                return;
            }

            order::PaymentReplyPayload reply;
            reply.order_id = payload.order_id;
            reply.ok = true;
            reply.authorization_id = "auth-" + payload.order_id;
            reply.detail = "authorized";
            context()->reply(hpactor::TypedMessage(
                order::PaymentAuthorizedTag, order::encode_payment_reply(reply)));
        }};
    }
};

class FulfillmentWorkerActor : public hpactor::EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "FulfillmentWorkerActor";

    FulfillmentWorkerActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() != order::QueueFulfillmentTag) return;
            order::FulfillmentPayload payload;
            if (!order::decode_fulfillment(msg.payload(), payload)) return;
            if (payload.scenario == order::ScenarioKind::WorkerCrash) {
                set_exit_reason(1);
                return;
            }
            context()->reply(hpactor::TypedMessage(
                order::FulfillmentQueuedTag, order::encode_fulfillment(payload)));
        }};
    }
};

class FulfillmentRouterActor : public hpactor::EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "FulfillmentRouterActor";

    FulfillmentRouterActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    void add_worker(hpactor::ActorAddress addr) { workers_.push_back(addr); }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() != order::QueueFulfillmentTag || workers_.empty()) return;
            const auto target = workers_[next_worker_++ % workers_.size()];
            context()->send_with_priority(target, hpactor::TypedMessage(
                order::QueueFulfillmentTag, hpactor::StreamBuffer(msg.payload())),
                1, INT64_MAX);
        }};
    }

  private:
    std::vector<hpactor::ActorAddress> workers_;
    size_t next_worker_ = 0;
};
```

- [ ] **Step 7: Add `OrderCoordinatorActor` happy path**

Insert after `FulfillmentRouterActor`. Use a sequential saga: on submit, calculate price synchronously through the typed actor instance, then proceed through actor messages for inventory, payment, and fulfillment.

```cpp
class OrderCoordinatorActor : public hpactor::StatefulActor<OrderCoordinatorState>,
                              public hpactor::LifecycleActor {
  public:
    static constexpr const char* kActorTypeName = "OrderCoordinatorActor";

    OrderCoordinatorActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                          hpactor::ActorAddress inventory,
                          hpactor::ActorAddress payment,
                          hpactor::ActorAddress fulfillment,
                          hpactor::ActorAddress log,
                          PricingActor* pricing,
                          std::promise<order::OrderStatusPayload>* done)
        : hpactor::StatefulActor<OrderCoordinatorState>(ctx, sys),
          inventory_(inventory), payment_(payment), fulfillment_(fulfillment),
          log_(log), pricing_(pricing), done_(done) {
        become(make_behavior());
    }

    hpactor::LifecycleActor* as_lifecycle() override { return this; }
    const hpactor::LifecycleActor* as_lifecycle() const override { return this; }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta meta;
        meta.actor_id = id().value();
        meta.actor_type = kActorTypeName;
        meta.state = state_string();
        meta.incarnation = incarnation();
        meta.messages_processed = state().processed;
        meta.behavior_name = "order-saga";
        return meta;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream out;
        out << "orders=" << state().orders.size();
        for (const auto& [id, record] : state().orders) {
            out << "\n" << id << "=" << order::to_string(record.status)
                << " total_cents=" << record.total_cents
                << " detail=" << record.detail;
        }
        auto text = out.str();
        return {text.begin(), text.end()};
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            ++state().processed;
            if (msg.type_id() == order::SubmitOrderTag) {
                on_submit(msg);
            } else if (msg.type_id() == order::InventoryReservedTag ||
                       msg.type_id() == order::InventoryRejectedTag) {
                on_inventory(msg);
            } else if (msg.type_id() == order::PaymentAuthorizedTag ||
                       msg.type_id() == order::PaymentDeclinedTag) {
                on_payment(msg);
            } else if (msg.type_id() == order::FulfillmentQueuedTag ||
                       msg.type_id() == order::FulfillmentFailedTag) {
                on_fulfillment(msg);
            } else if (msg.type_id() == order::PaymentTimedOutTag) {
                on_payment_timeout(msg);
            }
        }};
    }

  private:
    void log_event(const std::string& text) {
        context()->send(log_, hpactor::TypedMessage(
            order::LogOrderEventTag,
            hpactor::StreamBuffer(text.begin(), text.end())));
    }

    void complete(const OrderRecord& record) {
        order::OrderStatusPayload result;
        result.order_id = record.order_id;
        result.status = record.status;
        result.detail = record.detail;
        result.total_cents = record.total_cents;
        if (done_ != nullptr) done_->set_value(result);
    }

    void on_submit(hpactor::TypedMessage& msg) {
        order::SubmitOrderPayload payload;
        if (!order::decode_submit_order(msg.payload(), payload)) return;

        OrderRecord record;
        record.order_id = payload.order_id;
        record.customer_id = payload.customer_id;
        record.scenario = payload.scenario;
        record.lines = payload.lines;
        record.status = order::OrderStatus::Received;

        order::PricingRequest pricing_req{record.order_id, record.lines};
        auto pricing_result = (*pricing_)(std::move(pricing_req));
        if (!pricing_result.has_value()) {
            record.status = order::OrderStatus::Rejected;
            record.detail = "pricing failed";
            state().orders[record.order_id] = record;
            complete(record);
            return;
        }
        auto pricing = pricing_result.value();
        record.subtotal_cents = pricing.subtotal_cents;
        record.discount_cents = pricing.discount_cents;
        record.tax_cents = pricing.tax_cents;
        record.total_cents = pricing.total_cents;
        record.status = order::OrderStatus::Priced;
        state().orders[record.order_id] = record;
        log_event(record.order_id + " priced");

        if (context()->current_sender().id.value() != 0) {
            order::OrderStatusPayload accepted;
            accepted.order_id = record.order_id;
            accepted.status = order::OrderStatus::Priced;
            accepted.detail = "accepted";
            accepted.total_cents = record.total_cents;
            context()->reply(hpactor::TypedMessage(
                order::OrderAcceptedTag, order::encode_order_status(accepted)));
        }

        order::InventoryReservePayload reserve{record.order_id, record.lines};
        context()->try_send(inventory_, hpactor::TypedMessage(
            order::ReserveInventoryTag, order::encode_inventory_reserve(reserve)));
    }

    void on_inventory(hpactor::TypedMessage& msg) {
        order::InventoryReplyPayload reply;
        if (!order::decode_inventory_reply(msg.payload(), reply)) return;
        auto& record = state().orders[reply.order_id];
        if (!reply.ok) {
            record.status = order::OrderStatus::InventoryFailed;
            record.detail = reply.detail;
            log_event(record.order_id + " inventory_failed");
            complete(record);
            return;
        }
        record.status = order::OrderStatus::InventoryReserved;
        record.reservation_id = reply.reservation_id;
        log_event(record.order_id + " inventory_reserved");

        order::PaymentAuthorizePayload payment;
        payment.order_id = record.order_id;
        payment.customer_id = record.customer_id;
        payment.amount_cents = record.total_cents;
        payment.scenario = record.scenario;
        context()->try_send(payment_, hpactor::TypedMessage(
            order::AuthorizePaymentTag, order::encode_payment_authorize(payment)));
        if (record.scenario == order::ScenarioKind::PaymentTimeout) {
            context()->schedule(std::chrono::milliseconds(200),
                hpactor::TypedMessage(order::PaymentTimedOutTag,
                                      order::encode_order_status({record.order_id})));
        }
    }

    void on_payment(hpactor::TypedMessage& msg) {
        order::PaymentReplyPayload reply;
        if (!order::decode_payment_reply(msg.payload(), reply)) return;
        auto& record = state().orders[reply.order_id];
        if (!reply.ok) {
            record.status = order::OrderStatus::PaymentFailed;
            record.detail = reply.detail;
            log_event(record.order_id + " payment_failed");
            complete(record);
            return;
        }
        record.status = order::OrderStatus::PaymentAuthorized;
        log_event(record.order_id + " payment_authorized");
        order::FulfillmentPayload fulfill;
        fulfill.order_id = record.order_id;
        fulfill.reservation_id = record.reservation_id;
        fulfill.scenario = record.scenario;
        context()->try_send_with_priority(
            fulfillment_,
            hpactor::TypedMessage(order::QueueFulfillmentTag,
                                  order::encode_fulfillment(fulfill)),
            2, INT64_MAX);
    }

    void on_payment_timeout(hpactor::TypedMessage& msg) {
        order::OrderStatusPayload timeout;
        if (!order::decode_order_status(msg.payload(), timeout)) return;
        auto& record = state().orders[timeout.order_id];
        if (record.status == order::OrderStatus::InventoryReserved) {
            record.status = order::OrderStatus::PaymentTimedOut;
            record.detail = "payment timed out";
            log_event(record.order_id + " payment_timed_out");
            complete(record);
        }
    }

    void on_fulfillment(hpactor::TypedMessage& msg) {
        order::FulfillmentPayload reply;
        if (!order::decode_fulfillment(msg.payload(), reply)) return;
        auto& record = state().orders[reply.order_id];
        record.status = order::OrderStatus::Completed;
        record.detail = "completed";
        log_event(record.order_id + " completed");
        complete(record);
    }

    hpactor::ActorAddress inventory_;
    hpactor::ActorAddress payment_;
    hpactor::ActorAddress fulfillment_;
    hpactor::ActorAddress log_;
    PricingActor* pricing_ = nullptr;
    std::promise<order::OrderStatusPayload>* done_ = nullptr;
};
```

- [ ] **Step 8: Wire the all-in-one graph**

Replace `run_all_in_one()` with:

```cpp
int run_all_in_one(const Options& opts) {
    hpactor::ActorSystem system(make_base_config(opts, 0));

    auto log = system.spawn<OrderLogActor>();
    auto inventory = system.spawn<InventoryActor>();
    auto payment = system.spawn<PaymentActor>();
    auto fulfillment_worker = system.spawn<FulfillmentWorkerActor>();
    auto fulfillment_router = system.spawn<FulfillmentRouterActor>();
    auto pricing = system.spawn<PricingActor>();

    auto* router =
        static_cast<FulfillmentRouterActor*>(fulfillment_router.get().get());
    router->add_worker(fulfillment_worker.address());

    std::promise<order::OrderStatusPayload> done;
    auto result = done.get_future();
    auto* pricing_actor = static_cast<PricingActor*>(pricing.get().get());
    auto coordinator = system.spawn<OrderCoordinatorActor>(
        inventory.address(), payment.address(), fulfillment_router.address(),
        log.address(), pricing_actor, &done);

    order::SubmitOrderPayload submit;
    submit.order_id = "demo-1";
    submit.customer_id = "customer-1";
    submit.scenario = opts.scenario;
    submit.lines.push_back(order::OrderLine{"sku-book", 2, 1599});
    submit.lines.push_back(order::OrderLine{"sku-pen", 3, 250});
    if (opts.scenario == order::ScenarioKind::InsufficientStock) {
        submit.lines.push_back(order::OrderLine{"sku-lamp", 99, 3200});
    }

    system.deliver_local(coordinator.id(), hpactor::TypedMessage(
        order::SubmitOrderTag, order::encode_submit_order(submit)));

    auto status = result.wait_for(std::chrono::seconds(3));
    if (status == std::future_status::timeout) {
        std::cout << "SCENARIO RESULT order_id=demo-1 status=timeout\n";
        return 2;
    }
    auto final_status = result.get();
    std::cout << "SCENARIO RESULT order_id=" << final_status.order_id
              << " status=" << order::to_string(final_status.status)
              << " detail=" << final_status.detail
              << " total_cents=" << final_status.total_cents << "\n";

    auto dlq = system.dead_letter_snapshot();
    std::cout << "DLQ depth=" << dlq.depth
              << " total_pushed=" << dlq.total_pushed
              << " total_lost=" << dlq.total_lost << "\n";
    return final_status.status == order::OrderStatus::Completed ? 0 : 0;
}
```

- [ ] **Step 9: Build and run happy path**

Run:

```bash
ninja -C build 13_order_platform
./build/examples/13_order_platform --all-in-one --scenario happy-path
```

Expected:

```text
SCENARIO RESULT order_id=demo-1 status=completed
```

- [ ] **Step 10: Commit**

Run:

```bash
git add examples/13_order_platform.cpp
git commit -m "feat: add order platform happy path actors"
```

## Task 4: Failure Scenarios And Bounded Admission Output

**Files:**
- Modify: `examples/13_order_platform.cpp`

- [ ] **Step 1: Run failure commands and record current gaps**

Run:

```bash
./build/examples/13_order_platform --all-in-one --scenario insufficient-stock
./build/examples/13_order_platform --all-in-one --scenario payment-decline
./build/examples/13_order_platform --all-in-one --scenario payment-timeout
./build/examples/13_order_platform --all-in-one --scenario overload
./build/examples/13_order_platform --all-in-one --scenario missing-route
```

Expected before this task:

```text
insufficient-stock reports inventory_failed
payment-decline reports payment_failed
payment-timeout reports payment_timed_out
overload does not yet print admission details
missing-route does not yet print actor-not-found or DLQ details
```

- [ ] **Step 2: Add enqueue-result formatting**

Insert:

```cpp
const char* enqueue_code_name(hpactor::mailbox::EnqueueResultCode code) {
    using Code = hpactor::mailbox::EnqueueResultCode;
    switch (code) {
        case Code::Accepted: return "accepted";
        case Code::AcceptedWithSoftPressure: return "accepted_with_soft_pressure";
        case Code::Rejected: return "rejected";
        case Code::DroppedNewest: return "dropped_newest";
        case Code::DroppedExisting: return "dropped_existing";
        case Code::ReroutedToDeadLetter: return "rerouted_to_dead_letter";
        case Code::ReroutedToOverflow: return "rerouted_to_overflow";
        case Code::MailboxClosed: return "mailbox_closed";
        case Code::ActorNotFound: return "actor_not_found";
    }
    return "rejected";
}

void print_enqueue_result(const char* label,
                          const hpactor::mailbox::EnqueueResult& result) {
    std::cout << label
              << " code=" << enqueue_code_name(result.code)
              << " depth=" << result.depth
              << " capacity=" << result.capacity
              << " pressure=" << result.pressure_ratio
              << " retryable=" << (result.retryable() ? "true" : "false")
              << "\n";
}
```

Add include:

```cpp
#include <hpactor/mailbox/mailbox_policy.hpp>
```

- [ ] **Step 3: Add overload and missing-route paths to `run_all_in_one()`**

Before sending the normal submit, add:

```cpp
if (opts.scenario == order::ScenarioKind::Overload) {
    for (int i = 0; i < 8; ++i) {
        order::SubmitOrderPayload burst = submit;
        burst.order_id = "burst-" + std::to_string(i);
        auto result = system.try_deliver_local(
            coordinator.id(),
            hpactor::TypedMessage(order::SubmitOrderTag,
                                  order::encode_submit_order(burst)));
        print_enqueue_result("OVERLOAD enqueue", result);
    }
    auto dlq = system.dead_letter_snapshot();
    std::cout << "OVERLOAD dlq_depth=" << dlq.depth
              << " total_pushed=" << dlq.total_pushed << "\n";
    return 0;
}

if (opts.scenario == order::ScenarioKind::MissingRoute) {
    auto result = system.try_deliver_local(
        hpactor::ActorId{999999},
        hpactor::TypedMessage(order::SubmitOrderTag,
                              order::encode_submit_order(submit)));
    print_enqueue_result("MISSING_ROUTE enqueue", result);
    auto dlq = system.dead_letter_snapshot();
    std::cout << "MISSING_ROUTE dlq_depth=" << dlq.depth
              << " total_pushed=" << dlq.total_pushed << "\n";
    return result.code == hpactor::mailbox::EnqueueResultCode::ActorNotFound ? 0 : 3;
}
```

- [ ] **Step 4: Route worker-crash to fulfillment failure**

In `FulfillmentWorkerActor`, replace the worker-crash branch with a reply rather than only setting `exit_reason`, so the smoke command completes deterministically:

```cpp
if (payload.scenario == order::ScenarioKind::WorkerCrash) {
    context()->reply(hpactor::TypedMessage(
        order::FulfillmentFailedTag, order::encode_fulfillment(payload)));
    set_exit_reason(1);
    return;
}
```

In `OrderCoordinatorActor::on_fulfillment()`, add:

```cpp
if (msg.type_id() == order::FulfillmentFailedTag) {
    record.status = order::OrderStatus::FulfillmentFailed;
    record.detail = "worker failed";
    log_event(record.order_id + " fulfillment_failed");
    complete(record);
    return;
}
```

- [ ] **Step 5: Run scenario smoke commands**

Run:

```bash
ninja -C build 13_order_platform
./build/examples/13_order_platform --all-in-one --scenario insufficient-stock
./build/examples/13_order_platform --all-in-one --scenario payment-decline
./build/examples/13_order_platform --all-in-one --scenario payment-timeout
./build/examples/13_order_platform --all-in-one --scenario worker-crash
./build/examples/13_order_platform --all-in-one --scenario overload
./build/examples/13_order_platform --all-in-one --scenario missing-route
```

Expected output contains:

```text
status=inventory_failed
status=payment_failed
status=payment_timed_out
status=fulfillment_failed
OVERLOAD enqueue code=
MISSING_ROUTE enqueue code=actor_not_found
```

- [ ] **Step 6: Commit**

Run:

```bash
git add examples/13_order_platform.cpp
git commit -m "feat: add order platform failure scenarios"
```

## Task 5: HTTP Gateway And Query Mode

**Files:**
- Modify: `examples/13_order_platform.cpp`

- [ ] **Step 1: Run current query mode and observe scaffold behavior**

Run:

```bash
./build/examples/13_order_platform --query --gateway-port 18130 --submit demo-order
```

Expected before this task:

```text
QUERY would submit demo-order to HTTP port 18130
```

- [ ] **Step 2: Add HTTP gateway includes and route setup**

Add includes:

```cpp
#include <hpactor/actor/http_gateway_actor.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/http_types.hpp>
```

Add helper:

```cpp
void setup_http_routes(hpactor::net::HTTPGatewayActor& gateway,
                       hpactor::ActorAddress coordinator,
                       order::ScenarioKind scenario) {
    gateway.route(hpactor::net::HttpMethod::POST, "/orders",
        [coordinator, scenario](const hpactor::net::HttpRequest& req)
            -> std::pair<hpactor::ActorAddress, hpactor::TypedMessage> {
            order::SubmitOrderPayload submit;
            submit.order_id = "http-1";
            submit.customer_id = "http-customer";
            submit.scenario = scenario;
            submit.lines.push_back(order::OrderLine{"sku-book", 1, 1599});
            if (!req.body.empty()) {
                submit.order_id = std::string(req.body.begin(), req.body.end());
            }
            return {coordinator, hpactor::TypedMessage(
                order::SubmitOrderTag, order::encode_submit_order(submit))};
        });

    gateway.route(hpactor::net::HttpMethod::GET, "/orders/:id",
        [coordinator](const hpactor::net::HttpRequest& req)
            -> std::pair<hpactor::ActorAddress, hpactor::TypedMessage> {
            auto it = req.path_params.find("id");
            std::string order_id = it == req.path_params.end() ? "http-1" : it->second;
            order::OrderStatusPayload query;
            query.order_id = order_id;
            return {coordinator, hpactor::TypedMessage(
                order::QueryOrderTag, order::encode_order_status(query))};
        });
}
```

- [ ] **Step 3: Teach coordinator query handling**

In `OrderCoordinatorActor::make_behavior()`, add:

```cpp
} else if (msg.type_id() == order::QueryOrderTag) {
    order::OrderStatusPayload query;
    if (!order::decode_order_status(msg.payload(), query)) return;
    auto it = state().orders.find(query.order_id);
    order::OrderStatusPayload reply;
    reply.order_id = query.order_id;
    if (it == state().orders.end()) {
        reply.status = order::OrderStatus::Rejected;
        reply.detail = "not found";
    } else {
        reply.status = it->second.status;
        reply.detail = it->second.detail;
        reply.total_cents = it->second.total_cents;
    }
    context()->reply(hpactor::TypedMessage(
        order::OrderStatusTag, order::encode_order_status(reply)));
```

- [ ] **Step 4: Add gateway role wiring**

Replace `run_long_role()` for `--gateway` with a specific `run_gateway()`:

```cpp
int run_gateway(const Options& opts) {
    hpactor::Config config = make_base_config(opts, opts.actor_port);
    config.enable_http_gateway = false;
    hpactor::ActorSystem system(config);

    auto log = system.spawn<OrderLogActor>();
    auto inventory = system.spawn<InventoryActor>();
    auto payment = system.spawn<PaymentActor>();
    auto fulfillment_worker = system.spawn<FulfillmentWorkerActor>();
    auto fulfillment_router = system.spawn<FulfillmentRouterActor>();
    auto pricing = system.spawn<PricingActor>();
    static_cast<FulfillmentRouterActor*>(fulfillment_router.get().get())
        ->add_worker(fulfillment_worker.address());

    auto coordinator = system.spawn<OrderCoordinatorActor>(
        inventory.address(), payment.address(), fulfillment_router.address(),
        log.address(), static_cast<PricingActor*>(pricing.get().get()),
        nullptr);

    auto gateway_actor =
        system.spawn<hpactor::net::HTTPGatewayActor>("0.0.0.0", opts.http_port);
    auto* gateway =
        static_cast<hpactor::net::HTTPGatewayActor*>(gateway_actor.get().get());
    while (!gateway->is_listening()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    setup_http_routes(*gateway, coordinator.address(), opts.scenario);
    std::cout << "GATEWAY http_port=" << opts.http_port
              << " actor_endpoint="
              << hpactor::endpoint_ops::to_string(system.endpoint()) << "\n";
    run_until_signal("GATEWAY");
    return 0;
}
```

Update `main()`:

```cpp
if (opts->mode == "--gateway") return run_gateway(*opts);
```

- [ ] **Step 5: Replace query mode with HTTP client submission**

Replace `run_query()`:

```cpp
int run_query(const Options& opts) {
    if (!opts.submit_demo_order) {
        std::cout << "QUERY requires --submit demo-order\n";
        return 1;
    }
    hpactor::net::HttpClient client(nullptr);
    std::string url =
        "http://127.0.0.1:" + std::to_string(opts.gateway_port) + "/orders";
    hpactor::StreamBuffer body{'d','e','m','o','-','q','u','e','r','y'};
    auto response = client.post(url, std::move(body)).get();
    if (!response.has_value()) {
        std::cerr << "QUERY failed: " << response.error().message() << "\n";
        return 2;
    }
    std::cout << "QUERY response_bytes=" << response.value().size() << "\n";
    return 0;
}
```

- [ ] **Step 6: Run manual HTTP smoke**

Run terminal A:

```bash
./build/examples/13_order_platform --gateway --actor-port 17130 --http-port 18130
```

Run terminal B:

```bash
./build/examples/13_order_platform --query --gateway-port 18130 --submit demo-order
```

Expected terminal B:

```text
QUERY response_bytes=
```

Stop terminal A with Ctrl-C.

- [ ] **Step 7: Commit**

Run:

```bash
git add examples/13_order_platform.cpp
git commit -m "feat: add order platform HTTP gateway"
```

## Task 6: Distributed Payment Role And Remote Spawn Smoke

**Files:**
- Modify: `examples/13_order_platform.cpp`

- [ ] **Step 1: Register payment actor type**

Add include:

```cpp
#include <hpactor/actor_type_registry.hpp>
```

In every role that may receive remote spawn, register:

```cpp
system.actor_type_registry().register_type<PaymentActor>("order_payment_worker");
```

- [ ] **Step 2: Add static route helper**

Insert:

```cpp
void add_static_route(hpactor::Config& config, const std::string& endpoint) {
    auto pos = endpoint.find(':');
    if (pos == std::string::npos) return;
    std::string host = endpoint.substr(0, pos);
    uint16_t port = 0;
    if (!parse_port(endpoint.substr(pos + 1), port)) return;
    config.registrar.static_routes.push_back(hpactor::net::StaticRouteConfig{
        hpactor::endpoint_ops::parse_endpoint(endpoint), host, port});
}
```

- [ ] **Step 3: Add dedicated payment role**

Replace payment branch with:

```cpp
int run_payment(const Options& opts) {
    hpactor::Config config = make_base_config(opts, opts.actor_port);
    hpactor::ActorSystem system(config);
    system.actor_type_registry().register_type<PaymentActor>("order_payment_worker");
    auto local_payment = system.spawn<PaymentActor>();
    std::cout << "PAYMENT endpoint="
              << hpactor::endpoint_ops::to_string(system.endpoint())
              << " local_actor_id=" << local_payment.id().value()
              << " remote_type=order_payment_worker\n";
    run_until_signal("PAYMENT");
    return 0;
}
```

Update `main()`:

```cpp
if (opts->mode == "--payment") return run_payment(*opts);
```

- [ ] **Step 4: Remote-spawn payment when `--payment host:port` is supplied**

In `run_gateway()`, before creating `ActorSystem`, add:

```cpp
if (!opts.payment_endpoint.empty()) {
    add_static_route(config, opts.payment_endpoint);
}
```

After `ActorSystem system(config);`, add:

```cpp
hpactor::Actor payment;
hpactor::ActorAddress payment_address;
if (!opts.payment_endpoint.empty()) {
    auto remote = system.spawn_remote(
        opts.payment_endpoint, "order_payment_worker", hpactor::StreamBuffer{});
    if (!remote.has_value()) {
        std::cerr << "remote payment spawn failed: "
                  << remote.error().message() << "\n";
        return 2;
    }
    payment_address = remote.value().address();
    std::cout << "remote payment actor_id="
              << payment_address.id.value() << "\n";
} else {
    payment = system.spawn<PaymentActor>();
    payment_address = payment.address();
}
```

Use `payment_address` when spawning `OrderCoordinatorActor`.

- [ ] **Step 5: Run distributed payment smoke**

Run terminal A:

```bash
./build/examples/13_order_platform --payment --actor-port 17132
```

Run terminal B:

```bash
./build/examples/13_order_platform --gateway --actor-port 17130 --http-port 18130 --payment 127.0.0.1:17132
```

Run terminal C:

```bash
./build/examples/13_order_platform --query --gateway-port 18130 --submit demo-order
```

Expected terminal B:

```text
remote payment actor_id=
```

Expected terminal C:

```text
QUERY response_bytes=
```

Stop terminal B and A with Ctrl-C.

- [ ] **Step 6: Commit**

Run:

```bash
git add examples/13_order_platform.cpp
git commit -m "feat: add distributed payment role"
```

## Task 7: TOML Config Files And Topology Registration

**Files:**
- Modify: `examples/13_order_platform.cpp`
- Create: `examples/config/order_platform_all_in_one.toml`
- Create: `examples/config/order_gateway.toml`
- Create: `examples/config/order_inventory.toml`
- Create: `examples/config/order_payment.toml`
- Create: `examples/config/order_fulfillment.toml`
- Create: `examples/config/order_ops.toml`

- [ ] **Step 1: Add actor factory registration**

Add include:

```cpp
#include <hpactor/config/actor_factory_registry.hpp>
```

Add registrations after actor class definitions:

```cpp
HPACTOR_REGISTER_ACTOR("OrderLogActor", OrderLogActor)
HPACTOR_REGISTER_ACTOR("InventoryActor", InventoryActor)
HPACTOR_REGISTER_ACTOR("PaymentActor", PaymentActor)
HPACTOR_REGISTER_ACTOR("FulfillmentWorkerActor", FulfillmentWorkerActor)
HPACTOR_REGISTER_ACTOR("FulfillmentRouterActor", FulfillmentRouterActor)
```

Do not register `OrderCoordinatorActor` because it needs runtime addresses and a
`PricingActor*`.

- [ ] **Step 2: Add all-in-one config**

Create `examples/config/order_platform_all_in_one.toml`:

```toml
[system]
version = "1.0"
scheduler_threads = 4
enable_network = false
enable_http_gateway = false
metrics_enabled = true
metrics_path = "/metrics"

[system.mailbox]
default_capacity = 1024
default_policy = "reject_newest"
high_watermark = 0.80
low_watermark = 0.50
protected_system_messages = 32

[system.dead_letters]
enabled = true
capacity = 128
max_payload_sample_bytes = 256
store_payload = true

[system.cli]
enabled = true
default_format = "pretty"
page_size = 20

[system.tracing]
enabled = true
service_name = "order-platform-all-in-one"
exporter = "json_file"
json_file_path = "build/order-platform-traces.jsonl"
sample_ratio = 1.0

[[actor]]
id = "order_log"
behavior = "OrderLogActor"
mailbox_capacity = 128

[[actor]]
id = "inventory"
behavior = "InventoryActor"
mailbox_capacity = 128

[[actor]]
id = "payment"
behavior = "PaymentActor"
mailbox_capacity = 128

[[actor]]
id = "fulfillment_worker_1"
behavior = "FulfillmentWorkerActor"
mailbox_capacity = 128
```

- [ ] **Step 3: Add role configs**

Create `examples/config/order_gateway.toml`:

```toml
[system]
version = "1.0"
scheduler_threads = 4
enable_network = true
tcp_port = 17130
enable_http_gateway = false
metrics_enabled = true

[system.cli]
enabled = true
default_format = "pretty"
page_size = 20

[system.dead_letters]
enabled = true
capacity = 128
```

Create `examples/config/order_inventory.toml`:

```toml
[system]
version = "1.0"
scheduler_threads = 2
enable_network = true
tcp_port = 17131
metrics_enabled = true

[system.dead_letters]
enabled = true
capacity = 128

[[actor]]
id = "inventory"
behavior = "InventoryActor"
mailbox_capacity = 256
```

Create `examples/config/order_payment.toml`:

```toml
[system]
version = "1.0"
scheduler_threads = 2
enable_network = true
tcp_port = 17132
metrics_enabled = true

[system.dead_letters]
enabled = true
capacity = 128

[[actor]]
id = "payment"
behavior = "PaymentActor"
mailbox_capacity = 256
```

Create `examples/config/order_fulfillment.toml`:

```toml
[system]
version = "1.0"
scheduler_threads = 4
enable_network = true
tcp_port = 17133
metrics_enabled = true

[[dispatcher]]
name = "fulfillment_pool"
threads = 4

[system.dead_letters]
enabled = true
capacity = 128

[[actor]]
id = "fulfillment_worker_1"
behavior = "FulfillmentWorkerActor"
dispatcher = "fulfillment_pool"
mailbox_capacity = 256

[[actor]]
id = "fulfillment_worker_2"
behavior = "FulfillmentWorkerActor"
dispatcher = "fulfillment_pool"
mailbox_capacity = 256
```

Create `examples/config/order_ops.toml`:

```toml
[system]
version = "1.0"
scheduler_threads = 2
enable_network = true
tcp_port = 17134
metrics_enabled = true
metrics_path = "/metrics"

[system.cli]
enabled = true
default_format = "pretty"
page_size = 20

[system.dead_letters]
enabled = true
capacity = 256
alert_on_first_failure = true
```

- [ ] **Step 4: Load topology in all-in-one as a demonstration**

In `run_all_in_one()`, after constructing `ActorSystem`, add:

```cpp
auto topology_result =
    system.load_topology("examples/config/order_platform_all_in_one.toml");
if (!topology_result.has_value()) {
    std::cout << "topology load skipped: "
              << topology_result.error().message() << "\n";
}
```

Keep manual spawning for actors that need runtime wiring. This demonstrates
topology config without forcing the coordinator into the factory registry.

- [ ] **Step 5: Build and smoke config files**

Run:

```bash
ninja -C build 13_order_platform
./build/examples/13_order_platform --all-in-one --scenario happy-path
```

Expected:

```text
SCENARIO RESULT order_id=demo-1 status=completed
```

- [ ] **Step 6: Commit**

Run:

```bash
git add examples/13_order_platform.cpp examples/config/order_platform_all_in_one.toml examples/config/order_gateway.toml examples/config/order_inventory.toml examples/config/order_payment.toml examples/config/order_fulfillment.toml examples/config/order_ops.toml
git commit -m "feat: add order platform topology configs"
```

## Task 8: Ops Probe, DLQ Printing, CLI/Metric Transcript Output

**Files:**
- Modify: `examples/13_order_platform.cpp`

- [ ] **Step 1: Add DLQ record printing**

Insert:

```cpp
const char* dead_letter_reason_name(hpactor::mailbox::DeadLetterReason reason) {
    using Reason = hpactor::mailbox::DeadLetterReason;
    switch (reason) {
        case Reason::MailboxFull: return "mailbox_full";
        case Reason::MailboxClosed: return "mailbox_closed";
        case Reason::ActorNotFound: return "actor_not_found";
        case Reason::ActorTerminated: return "actor_terminated";
        case Reason::MissingRoute: return "missing_route";
        case Reason::RemoteNodeUnreachable: return "remote_node_unreachable";
        case Reason::NetworkPartition: return "network_partition";
        case Reason::TransportSendFailed: return "transport_send_failed";
        case Reason::DecodeFailed: return "decode_failed";
        case Reason::OverflowPolicy: return "overflow_policy";
        case Reason::NoDropRejected: return "no_drop_rejected";
    }
    return "actor_not_found";
}

void print_dead_letters(hpactor::ActorSystem& system) {
    auto snapshot = system.dead_letter_snapshot();
    std::cout << "DLQ depth=" << snapshot.depth
              << " total_pushed=" << snapshot.total_pushed
              << " total_lost=" << snapshot.total_lost << "\n";
    hpactor::mailbox::DeadLetterRecord record;
    while (system.pop_dead_letter(record)) {
        std::cout << "DLQ record reason=" << dead_letter_reason_name(record.reason)
                  << " target=" << record.target.id.value()
                  << " type_tag=" << static_cast<uint32_t>(record.type_tag)
                  << " payload_sample=" << record.payload_sample.size() << "\n";
    }
}
```

- [ ] **Step 2: Add `OpsProbeActor`**

Insert:

```cpp
class OpsProbeActor : public hpactor::EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "OpsProbeActor";

    OpsProbeActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    hpactor::cli::ActorMeta to_metadata() const override {
        hpactor::cli::ActorMeta meta;
        meta.actor_id = id().value();
        meta.actor_type = kActorTypeName;
        meta.state = "active";
        meta.messages_processed = ticks_;
        meta.behavior_name = "ops-probe";
        return meta;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::string text = "ticks=" + std::to_string(ticks_);
        return {text.begin(), text.end()};
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() != order::OpsProbeTickTag) return;
            ++ticks_;
            auto dlq = system().dead_letter_snapshot();
            std::cout << "OPS actor_count=" << system().actor_count()
                      << " dlq_depth=" << dlq.depth
                      << " dlq_total_pushed=" << dlq.total_pushed << "\n";
            context()->schedule(std::chrono::seconds(1),
                hpactor::TypedMessage(order::OpsProbeTickTag, hpactor::StreamBuffer{}));
        }};
    }

  private:
    uint64_t ticks_ = 0;
};
```

- [ ] **Step 3: Spawn ops probe in all-in-one and ops roles**

In `run_all_in_one()`, after spawning the coordinator:

```cpp
auto ops = system.spawn<OpsProbeActor>();
system.deliver_local(ops.id(), hpactor::TypedMessage(
    order::OpsProbeTickTag, hpactor::StreamBuffer{}));
```

Add `run_ops()`:

```cpp
int run_ops(const Options& opts) {
    hpactor::ActorSystem system(make_base_config(opts, opts.actor_port));
    auto ops = system.spawn<OpsProbeActor>();
    system.deliver_local(ops.id(), hpactor::TypedMessage(
        order::OpsProbeTickTag, hpactor::StreamBuffer{}));
    std::cout << "OPS cli_enabled=true metrics_path=/metrics\n";
    run_until_signal("OPS");
    return 0;
}
```

Update `main()`:

```cpp
if (opts->mode == "--ops") return run_ops(*opts);
```

- [ ] **Step 4: Print operations transcript hints**

At the end of successful `run_all_in_one()`, before returning:

```cpp
std::cout << "Inspect with CLI commands: /actor list, /system stats, /system memory\n";
std::cout << "Metrics are collected by the framework metrics ring buffer.\n";
#if HPACTOR_ENABLE_ACTOR_TRACING
std::cout << "Tracing exporter=json_file path=build/order-platform-traces.jsonl\n";
#else
std::cout << "Tracing unavailable in this build.\n";
#endif
print_dead_letters(system);
```

- [ ] **Step 5: Build and run observability smoke**

Run:

```bash
ninja -C build 13_order_platform
./build/examples/13_order_platform --all-in-one --scenario happy-path
./build/examples/13_order_platform --all-in-one --scenario missing-route
```

Expected output contains:

```text
Inspect with CLI commands:
DLQ
```

- [ ] **Step 6: Commit**

Run:

```bash
git add examples/13_order_platform.cpp
git commit -m "feat: add order platform ops visibility"
```

## Task 9: Final Verification, Polish, And Documentation

**Files:**
- Modify: `examples/13_order_platform.cpp`
- Modify: `examples/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `docs/examples/order-platform.md`

- [ ] **Step 1: Add user-facing walkthrough doc**

Create `docs/examples/order-platform.md`:

````markdown
# Order Platform Example

`13_order_platform` is an end-to-end HPActor example that models an order saga
with gateway, coordinator, inventory, payment, fulfillment, order log, and ops
roles.

## Quickstart

```bash
./build/examples/13_order_platform --all-in-one --scenario happy-path
```

Expected final line:

```text
SCENARIO RESULT order_id=demo-1 status=completed
```

## Failure Scenarios

```bash
./build/examples/13_order_platform --all-in-one --scenario insufficient-stock
./build/examples/13_order_platform --all-in-one --scenario payment-decline
./build/examples/13_order_platform --all-in-one --scenario payment-timeout
./build/examples/13_order_platform --all-in-one --scenario worker-crash
./build/examples/13_order_platform --all-in-one --scenario overload
./build/examples/13_order_platform --all-in-one --scenario missing-route
```

## Distributed Payment Smoke

Terminal 1:

```bash
./build/examples/13_order_platform --payment --actor-port 17132
```

Terminal 2:

```bash
./build/examples/13_order_platform --gateway --actor-port 17130 --http-port 18130 --payment 127.0.0.1:17132
```

Terminal 3:

```bash
./build/examples/13_order_platform --query --gateway-port 18130 --submit demo-order
```

## What It Demonstrates

- `EventBasedActor`, `StatefulActor<T>`, and `TypedEventBasedActor`.
- HTTP ingress through `net::HTTPGatewayActor`.
- Bounded mailbox admission through `try_send()` and `try_deliver_local()`.
- Dead-letter queue snapshots and record printing.
- CLI serialization through `to_metadata()` and `serialize_state()`.
- Metrics, logging, and tracing configuration.
- Remote spawn and location-transparent remote payment actor references.
- TOML topology registration for default-constructible actors.

The example names future production-plane extension points in comments and docs
without presenting them as implemented runtime behavior.
````

- [ ] **Step 2: Run formatting-sensitive checks**

Run:

```bash
git diff --check
```

Expected: no output and exit code 0.

- [ ] **Step 3: Build all examples and tests**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

Expected:

```text
100% tests passed
```

- [ ] **Step 4: Run required example acceptance commands**

Run:

```bash
./build/examples/13_order_platform --all-in-one --scenario happy-path
./build/examples/13_order_platform --all-in-one --scenario insufficient-stock
./build/examples/13_order_platform --all-in-one --scenario payment-decline
./build/examples/13_order_platform --all-in-one --scenario payment-timeout
./build/examples/13_order_platform --all-in-one --scenario worker-crash
./build/examples/13_order_platform --all-in-one --scenario overload
./build/examples/13_order_platform --all-in-one --scenario missing-route
```

Expected output contains:

```text
status=completed
status=inventory_failed
status=payment_failed
status=payment_timed_out
status=fulfillment_failed
OVERLOAD enqueue code=
MISSING_ROUTE enqueue code=actor_not_found
```

- [ ] **Step 5: Run distributed smoke**

Run terminal A:

```bash
./build/examples/13_order_platform --payment --actor-port 17132
```

Run terminal B:

```bash
./build/examples/13_order_platform --gateway --actor-port 17130 --http-port 18130 --payment 127.0.0.1:17132
```

Run terminal C:

```bash
./build/examples/13_order_platform --query --gateway-port 18130 --submit demo-order
```

Expected:

```text
remote payment actor_id=
QUERY response_bytes=
```

Stop terminals B and A with Ctrl-C.

- [ ] **Step 6: Commit**

Run:

```bash
git add examples/13_order_platform.cpp examples/CMakeLists.txt tests/CMakeLists.txt examples/order_platform/messages.hpp tests/examples/test_order_platform_messages.cpp examples/config/order_platform_all_in_one.toml examples/config/order_gateway.toml examples/config/order_inventory.toml examples/config/order_payment.toml examples/config/order_fulfillment.toml examples/config/order_ops.toml docs/examples/order-platform.md
git commit -m "docs: add order platform example walkthrough"
```

## Spec Coverage Map

- Purpose and goals: Tasks 1 through 9 build the example, tests, docs, and verification.
- Artifacts: Tasks 1, 2, 7, and 9 create every required artifact.
- Runtime modes: Tasks 2, 5, 6, and 8 implement all modes.
- Actor graph: Tasks 3 and 8 implement the business and ops actors.
- Message tags and payloads: Task 1 implements and tests the application message range.
- Happy path: Task 3 implements and verifies `completed`.
- Distributed path: Task 6 implements remote-spawned payment.
- Failure scenarios: Task 4 implements insufficient stock, decline, timeout, worker crash, overload, and missing route.
- Config design: Task 7 adds TOML configs and topology registration.
- Observability: Task 8 adds CLI serialization, DLQ printing, ops probe output, and tracing/metrics transcript hints.
- Concurrency/resource contracts: Tasks 3 and 4 use actor-owned state, messages, timers, and bounded admission APIs.
- User experience: Tasks 2 and 9 provide banner commands and walkthrough docs.
- Acceptance evidence: Task 9 runs the full build, tests, all-in-one scenarios, and distributed smoke.

## Execution Notes

- Keep the work in `.worktrees/full-featured-example-design` unless the user asks for a new worktree.
- Do not modify unrelated files from the main checkout.
- Do not make remote inventory or remote fulfillment required for the first implementation. The first distributed smoke path is remote payment.
- If an API detail differs from the plan while implementing, inspect the local header and adapt to the checked-in API rather than inventing a new runtime surface.
- Commit after each task so review can happen in small, reversible chunks.
