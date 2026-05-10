# Distributed Tracing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a native distributed tracing subsystem for HPActor that propagates W3C/OpenTelemetry-compatible trace context through actor messages, remote frames, RPC, remote spawn, and HTTP boundaries, then exports sampled spans asynchronously.

**Architecture:** Trace context lives in HPActor message envelopes: `TypedMessage` for in-process delivery and `ActorMsgFrame.trace_context` for wire delivery. `EventBasedActor::receive()` creates actor consumer spans, `ActorContext` propagates the active context on sends/replies/RPC/HTTP, and `TraceManager` owns sampling, ID generation, a bounded `MpscRingBuffer<SpanRecord>`, and exporters. Exporters run outside actor execution and never block actor progress.

**Tech Stack:** C++20, protobuf, existing HPActor `result<T>`, existing `metrics::MpscRingBuffer<T>`, CMake/Ninja, W3C Trace Context, OTLP/HTTP JSON, no exceptions, no RTTI.

**Spec:** `docs/superpowers/specs/2026-05-10-distributed-tracing-design.md`
**Architecture doc:** `docs/architecture/actor/distributed-tracing-design.md`
**OTLP references:** `https://opentelemetry.io/docs/specs/otlp/`, `https://opentelemetry.io/docs/specs/otel/protocol/exporter/`

---

## File Structure

| File | Purpose |
|------|---------|
| `include/hpactor/tracing/trace_config.hpp` | Runtime tracing configuration and enum parsing helpers. |
| `include/hpactor/tracing/trace_context_parser.hpp` | W3C `traceparent`/`tracestate` parser and formatter. |
| `include/hpactor/tracing/span.hpp` | `SpanKind`, `SpanStatus`, `SpanStart`, `SpanHandle`, `SpanRecord`. |
| `include/hpactor/tracing/sampler.hpp` | Sampler interface and built-in sampler classes. |
| `include/hpactor/tracing/trace_exporter.hpp` | Exporter interface. |
| `include/hpactor/tracing/memory_exporter.hpp` | Test exporter with in-memory span storage. |
| `include/hpactor/tracing/json_exporter.hpp` | JSON lines exporter. |
| `include/hpactor/tracing/otlp_exporter.hpp` | OTLP/HTTP JSON exporter. |
| `include/hpactor/tracing/trace_manager.hpp` | TraceManager facade owned by ActorSystem. |
| `src/tracing/trace_context_parser.cpp` | W3C parsing/formatting implementation. |
| `src/tracing/sampler.cpp` | Sampler and ID generator implementation. |
| `src/tracing/trace_manager.cpp` | Span lifecycle, ring buffer, drain thread, force flush. |
| `src/tracing/memory_exporter.cpp` | Memory exporter implementation. |
| `src/tracing/json_exporter.cpp` | JSON file/stdout exporter implementation. |
| `src/tracing/otlp_exporter.cpp` | OTLP/HTTP JSON export implementation. |
| `include/hpactor/types/types.hpp` | Expand `TraceContext`, add `TraceId`, `SpanId`, `TraceFlags`. |
| `include/hpactor/actor/typed_message.hpp` | Add optional trace sidecar and move propagation. |
| `include/hpactor/actor_context.hpp` | Add current trace context and trace scope APIs. |
| `src/actor/actor_context.cpp` | Inject context on send/reply/RPC/HTTP. |
| `src/actor/event_based_actor.cpp` | Actor receive span guard and current trace scope. |
| `include/hpactor/core/actor_system.hpp` | Add `TraceConfig`, `TraceManager`, accessors, apply config. |
| `src/actor/actor_system.cpp` | Initialize tracing, apply TOML tracing config, remote receive propagation. |
| `protos/hpactor/frame.proto` | Add `PbTraceContext trace_context = 7`. |
| `include/hpactor/net/frame.hpp` | Trace protobuf conversion helper declarations. |
| `src/net/frame_protobuf.cpp` | Trace protobuf conversion helper definitions. |
| `src/ref/actor_proxy.cpp` | Serialize trace context into remote actor frames. |
| `include/hpactor/rpc/rpc_channel.hpp` | Store pending RPC trace/span state and response frame type. |
| `src/rpc/rpc_channel.cpp` | Propagate RPC trace context and finish client spans. |
| `include/hpactor/net/connection_pool.hpp` | Update RPC response callback type to carry frame context. |
| `src/net/connection_pool.cpp` | Pass RPC response context to `RpcChannel`. |
| `include/hpactor/actor/spawn_receiver.hpp` | Track spawn receive span helper if needed. |
| `src/actor/spawn_receiver.cpp` | Propagate trace context on spawn request/response. |
| `include/hpactor/actor/http_gateway_actor.hpp` | Add trace state to pending HTTP replies. |
| `src/actor/http_gateway_actor.cpp` | Extract/inject W3C headers and preserve trace through request-id wrapping. |
| `include/hpactor/net/http_client.hpp` | Allow response status and request headers needed by tracing tests. |
| `src/net/http_client.cpp` | Preserve headers and support OTLP/HTTP JSON POST. |
| `include/hpactor/config/topology_model.hpp` | Add `tracing::TraceConfig` to `SystemDef`. |
| `src/config/toml_parser.cpp` | Parse `[system.tracing]`. |
| `include/hpactor/config/binary_format.hpp` | Add binary tracing fields and bump format version. |
| `src/config/binary_serializer.cpp` | Serialize tracing config. |
| `src/config/binary_loader.cpp` | Load tracing config with older-version defaults. |
| `include/hpactor/hpactor_config.hpp.in` | Add `HPACTOR_ENABLE_ACTOR_TRACING`. |
| `CMakeLists.txt` | Add `ENABLE_ACTOR_TRACING` option and tracing sources. |
| `tests/CMakeLists.txt` | Add tracing test executables. |
| `tests/tracing/*.cpp` | Focused tracing tests. |

---

## Global Implementation Rules

- Keep trace context out of user payload bytes.
- Preserve existing fire-and-forget delivery semantics. Tracing failures must not change message delivery.
- Use `result<T>` for parsing/export errors and explicit enum status for non-error state.
- Do not add exceptions or RTTI.
- Do not allocate on actor send/receive hot paths except for existing payload ownership and optional exporter-side formatting.
- Prefer fixed-size IDs and fixed-size span records on hot paths.
- Preserve `TypedMessage::mpsc_next` move behavior exactly as it is today.
- Run the focused test after each task, then run the broader suite at integration checkpoints.

---

### Task 1: Add Tracing Build Skeleton and Test Targets

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `include/hpactor/hpactor_config.hpp.in`
- Modify: `tests/CMakeLists.txt`
- Create: `include/hpactor/tracing/trace_config.hpp`
- Create: `src/tracing/trace_manager.cpp`
- Create: `tests/tracing/test_trace_config_smoke.cpp`

- [ ] **Step 1: Add the CMake option and config macro**

In `CMakeLists.txt`, add the option near the existing observability options:

```cmake
option(ENABLE_ACTOR_TRACING "Enable distributed tracing subsystem" ON)
```

After CLI source wiring, add:

```cmake
if(ENABLE_ACTOR_TRACING)
    set(HPACTOR_ENABLE_ACTOR_TRACING 1)
    target_sources(hpactor_lib PRIVATE
        src/tracing/trace_manager.cpp
    )
else()
    set(HPACTOR_ENABLE_ACTOR_TRACING 0)
endif()
```

In `include/hpactor/hpactor_config.hpp.in`, add:

```cpp
#define HPACTOR_ENABLE_ACTOR_TRACING @HPACTOR_ENABLE_ACTOR_TRACING@
```

- [ ] **Step 2: Create the first tracing config header**

Create `include/hpactor/tracing/trace_config.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor::tracing {

enum class TraceExporterKind : uint8_t {
    kNoop,
    kMemory,
    kJsonFile,
    kOtlpHttp,
};

enum class SamplerKind : uint8_t {
    kAlwaysOff,
    kAlwaysOn,
    kTraceIdRatio,
    kParentBasedTraceIdRatio,
};

struct TraceConfig {
    bool enabled{false};
    bool propagate_unsampled{true};
    uint32_t ring_buffer_capacity{65536};
    std::string service_name{"hpactor"};
    SamplerKind sampler{SamplerKind::kParentBasedTraceIdRatio};
    double sample_ratio{0.01};
    TraceExporterKind exporter{TraceExporterKind::kOtlpHttp};
    std::string otlp_endpoint{"http://127.0.0.1:4318/v1/traces"};
    std::string json_file_path;
    std::chrono::milliseconds export_interval{500};
    uint32_t max_export_batch_size{512};
    uint16_t max_tracestate_len{256};
    bool record_actor_receive_spans{true};
    bool record_remote_producer_spans{true};
    bool record_local_producer_spans{false};
    bool record_payload_size{true};
    bool create_roots_for_actor_context_sends{false};
    bool create_roots_for_rpc{true};
    bool create_roots_for_http_ingress{true};
};

} // namespace hpactor::tracing
```

- [ ] **Step 3: Create a temporary TraceManager translation unit**

Create `src/tracing/trace_manager.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/tracing/trace_config.hpp>

namespace hpactor::tracing {
namespace {
[[maybe_unused]] constexpr bool kTracingTranslationUnitPresent = true;
} // namespace
} // namespace hpactor::tracing
```

This keeps the new source path buildable before the real manager exists.

- [ ] **Step 4: Add a smoke test**

Create `tests/tracing/test_trace_config_smoke.cpp`:

```cpp
#include <hpactor/tracing/trace_config.hpp>

#include <cassert>

int main() {
    hpactor::tracing::TraceConfig cfg;
    assert(!cfg.enabled);
    assert(cfg.propagate_unsampled);
    assert(cfg.ring_buffer_capacity == 65536);
    assert(cfg.sampler == hpactor::tracing::SamplerKind::kParentBasedTraceIdRatio);
    assert(cfg.exporter == hpactor::tracing::TraceExporterKind::kOtlpHttp);
    assert(cfg.otlp_endpoint == "http://127.0.0.1:4318/v1/traces");
    return 0;
}
```

In `tests/CMakeLists.txt`, add near the metrics tests:

```cmake
if(ENABLE_ACTOR_TRACING)
    add_executable(test_trace_config_smoke tracing/test_trace_config_smoke.cpp)
    target_link_libraries(test_trace_config_smoke hpactor)
    add_test(NAME test_trace_config_smoke COMMAND test_trace_config_smoke)
endif()
```

- [ ] **Step 5: Build and run the smoke test**

Run:

```bash
cmake -S . -B build -GNinja
ninja -C build test_trace_config_smoke
./build/tests/test_trace_config_smoke
```

Expected: build succeeds and the executable exits with status 0.

- [ ] **Step 6: Verify tracing can compile out**

Run:

```bash
cmake -S . -B build-no-tracing -GNinja -DENABLE_ACTOR_TRACING=OFF
ninja -C build-no-tracing hpactor_lib
```

Expected: `hpactor_lib` builds and `test_trace_config_smoke` is not generated.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt include/hpactor/hpactor_config.hpp.in include/hpactor/tracing/trace_config.hpp src/tracing/trace_manager.cpp tests/CMakeLists.txt tests/tracing/test_trace_config_smoke.cpp
git commit -m "build: add tracing feature skeleton"
```

---

### Task 2: Implement Trace IDs and W3C Trace Context Parsing

**Files:**
- Modify: `include/hpactor/types/types.hpp`
- Create: `include/hpactor/tracing/trace_context_parser.hpp`
- Create: `src/tracing/trace_context_parser.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/tracing/test_trace_context.cpp`
- Create: `tests/tracing/test_w3c_trace_context.cpp`

- [ ] **Step 1: Write the failing TraceContext unit test**

Create `tests/tracing/test_trace_context.cpp`:

```cpp
#include <hpactor/types/types.hpp>

#include <cassert>
#include <cstring>

using namespace hpactor;

static TraceContext make_context() {
    TraceContext ctx;
    for (uint8_t i = 0; i < ctx.trace_id.bytes.size(); ++i) {
        ctx.trace_id.bytes[i] = static_cast<uint8_t>(i + 1);
    }
    for (uint8_t i = 0; i < ctx.span_id.bytes.size(); ++i) {
        ctx.span_id.bytes[i] = static_cast<uint8_t>(0xA0 + i);
    }
    ctx.flags.set_sampled(true);
    const char state[] = "vendor=value";
    std::memcpy(ctx.tracestate.data(), state, sizeof(state) - 1);
    ctx.tracestate_len = sizeof(state) - 1;
    return ctx;
}

int main() {
    TraceId empty_trace;
    SpanId empty_span;
    assert(!empty_trace.valid());
    assert(!empty_span.valid());

    TraceContext empty;
    assert(!empty.valid());
    assert(!empty.sampled());

    TraceContext ctx = make_context();
    assert(ctx.valid());
    assert(ctx.sampled());
    assert(ctx.tracestate_view() == "vendor=value");

    ctx.flags.set_sampled(false);
    assert(!ctx.sampled());

    ctx.clear();
    assert(!ctx.valid());
    assert(ctx.tracestate_len == 0);
    return 0;
}
```

Add to `tests/CMakeLists.txt` inside `if(ENABLE_ACTOR_TRACING)`:

```cmake
add_executable(test_trace_context tracing/test_trace_context.cpp)
target_link_libraries(test_trace_context hpactor)
add_test(NAME test_trace_context COMMAND test_trace_context)
```

- [ ] **Step 2: Run the failing test build**

Run:

```bash
ninja -C build test_trace_context
```

Expected: compile fails because `TraceId`, `SpanId`, and the expanded `TraceContext` members do not exist.

- [ ] **Step 3: Expand TraceContext in `types.hpp`**

Replace the existing `TraceContext` block in `include/hpactor/types/types.hpp` with:

```cpp
// -----------------------------------------------------------------------------
// Trace identifiers - W3C/OpenTelemetry-compatible distributed tracing IDs
// -----------------------------------------------------------------------------
struct TraceId {
    std::array<uint8_t, 16> bytes{};

    bool valid() const noexcept {
        for (uint8_t b : bytes) {
            if (b != 0) {
                return true;
            }
        }
        return false;
    }

    bool operator==(const TraceId& other) const noexcept {
        return bytes == other.bytes;
    }
};

struct SpanId {
    std::array<uint8_t, 8> bytes{};

    bool valid() const noexcept {
        for (uint8_t b : bytes) {
            if (b != 0) {
                return true;
            }
        }
        return false;
    }

    bool operator==(const SpanId& other) const noexcept {
        return bytes == other.bytes;
    }
};

struct TraceFlags {
    static constexpr uint8_t kSampled = 0x01;

    uint8_t value = 0;

    bool sampled() const noexcept {
        return (value & kSampled) != 0;
    }

    void set_sampled(bool enabled) noexcept {
        if (enabled) {
            value = static_cast<uint8_t>(value | kSampled);
        } else {
            value = static_cast<uint8_t>(value & ~kSampled);
        }
    }
};

struct TraceContext {
    TraceId trace_id;
    SpanId span_id;
    TraceFlags flags;
    uint8_t version = 0;
    uint16_t tracestate_len = 0;
    std::array<char, 256> tracestate{};

    bool valid() const noexcept {
        return trace_id.valid() && span_id.valid();
    }

    bool sampled() const noexcept {
        return flags.sampled();
    }

    std::string_view tracestate_view() const noexcept {
        return {tracestate.data(), tracestate_len};
    }

    void clear() noexcept {
        trace_id = TraceId{};
        span_id = SpanId{};
        flags = TraceFlags{};
        version = 0;
        tracestate_len = 0;
        tracestate.fill('\0');
    }
};
```

Also add these includes near the top of `types.hpp` if missing:

```cpp
#include <array>
#include <string_view>
```

- [ ] **Step 4: Run the TraceContext test**

Run:

```bash
ninja -C build test_trace_context
./build/tests/test_trace_context
```

Expected: build succeeds and test exits with status 0.

- [ ] **Step 5: Write the failing W3C parser test**

Create `tests/tracing/test_w3c_trace_context.cpp`:

```cpp
#include <hpactor/tracing/trace_context_parser.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::tracing;

int main() {
    auto ok = parse_w3c_trace_context(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
        "vendor=value",
        256);
    assert(ok.status == TraceParseStatus::kOk);
    assert(ok.context.valid());
    assert(ok.context.sampled());
    assert(format_traceparent(ok.context) ==
           "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    assert(format_tracestate(ok.context) == "vendor=value");

    auto uppercase = parse_w3c_trace_context(
        "00-4BF92F3577B34DA6A3CE929D0E0E4736-00F067AA0BA902B7-00",
        "",
        256);
    assert(uppercase.status == TraceParseStatus::kOk);
    assert(!uppercase.context.sampled());
    assert(format_traceparent(uppercase.context) ==
           "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00");

    auto missing = parse_w3c_trace_context("", "", 256);
    assert(missing.status == TraceParseStatus::kMissing);

    auto zero_trace = parse_w3c_trace_context(
        "00-00000000000000000000000000000000-00f067aa0ba902b7-01",
        "",
        256);
    assert(zero_trace.status == TraceParseStatus::kInvalidTraceId);

    auto zero_span = parse_w3c_trace_context(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01",
        "",
        256);
    assert(zero_span.status == TraceParseStatus::kInvalidSpanId);

    auto malformed = parse_w3c_trace_context("00-short", "", 256);
    assert(malformed.status == TraceParseStatus::kMalformed);

    auto too_large_state = parse_w3c_trace_context(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
        "abcdefgh",
        4);
    assert(too_large_state.status == TraceParseStatus::kTracestateTooLarge);
    return 0;
}
```

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(test_w3c_trace_context tracing/test_w3c_trace_context.cpp)
target_link_libraries(test_w3c_trace_context hpactor)
add_test(NAME test_w3c_trace_context COMMAND test_w3c_trace_context)
```

- [ ] **Step 6: Run the failing W3C parser test build**

Run:

```bash
ninja -C build test_w3c_trace_context
```

Expected: compile fails because `trace_context_parser.hpp` does not exist.

- [ ] **Step 7: Add the parser header**

Create `include/hpactor/tracing/trace_context_parser.hpp`:

```cpp
#pragma once

#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace hpactor::tracing {

enum class TraceParseStatus : uint8_t {
    kOk,
    kMissing,
    kMalformed,
    kUnsupportedVersion,
    kInvalidTraceId,
    kInvalidSpanId,
    kTracestateTooLarge,
};

struct TraceParseResult {
    TraceParseStatus status{TraceParseStatus::kMissing};
    TraceContext context{};
};

TraceParseResult parse_w3c_trace_context(
    std::string_view traceparent,
    std::string_view tracestate,
    uint16_t max_tracestate_len) noexcept;

std::string format_traceparent(const TraceContext& context);
std::string format_tracestate(const TraceContext& context);

} // namespace hpactor::tracing
```

- [ ] **Step 8: Add the parser implementation**

Create `src/tracing/trace_context_parser.cpp`:

```cpp
#include <hpactor/tracing/trace_context_parser.hpp>

#include <array>
#include <charconv>
#include <cstring>

namespace hpactor::tracing {

namespace {

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

template <size_t N>
bool parse_hex_bytes(std::string_view text, std::array<uint8_t, N>& out) noexcept {
    if (text.size() != N * 2) return false;
    for (size_t i = 0; i < N; ++i) {
        int hi = hex_value(text[i * 2]);
        int lo = hex_value(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

void append_hex_byte(std::string& out, uint8_t b) {
    constexpr char kHex[] = "0123456789abcdef";
    out.push_back(kHex[(b >> 4) & 0x0F]);
    out.push_back(kHex[b & 0x0F]);
}

} // namespace

TraceParseResult parse_w3c_trace_context(
    std::string_view traceparent,
    std::string_view tracestate,
    uint16_t max_tracestate_len) noexcept {
    TraceParseResult result;
    if (traceparent.empty()) {
        result.status = TraceParseStatus::kMissing;
        return result;
    }
    if (traceparent.size() != 55 ||
        traceparent[2] != '-' ||
        traceparent[35] != '-' ||
        traceparent[52] != '-') {
        result.status = TraceParseStatus::kMalformed;
        return result;
    }
    if (traceparent.substr(0, 2) != "00") {
        result.status = TraceParseStatus::kUnsupportedVersion;
        return result;
    }

    TraceContext ctx;
    if (!parse_hex_bytes(traceparent.substr(3, 32), ctx.trace_id.bytes) ||
        !ctx.trace_id.valid()) {
        result.status = TraceParseStatus::kInvalidTraceId;
        return result;
    }
    if (!parse_hex_bytes(traceparent.substr(36, 16), ctx.span_id.bytes) ||
        !ctx.span_id.valid()) {
        result.status = TraceParseStatus::kInvalidSpanId;
        return result;
    }

    std::array<uint8_t, 1> flags{};
    if (!parse_hex_bytes(traceparent.substr(53, 2), flags)) {
        result.status = TraceParseStatus::kMalformed;
        return result;
    }
    ctx.flags.value = flags[0];

    if (tracestate.size() > max_tracestate_len ||
        tracestate.size() > ctx.tracestate.size()) {
        result.status = TraceParseStatus::kTracestateTooLarge;
        return result;
    }
    if (!tracestate.empty()) {
        std::memcpy(ctx.tracestate.data(), tracestate.data(), tracestate.size());
        ctx.tracestate_len = static_cast<uint16_t>(tracestate.size());
    }

    result.status = TraceParseStatus::kOk;
    result.context = ctx;
    return result;
}

std::string format_traceparent(const TraceContext& context) {
    std::string out;
    out.reserve(55);
    out += "00-";
    for (uint8_t b : context.trace_id.bytes) append_hex_byte(out, b);
    out.push_back('-');
    for (uint8_t b : context.span_id.bytes) append_hex_byte(out, b);
    out.push_back('-');
    append_hex_byte(out, context.flags.value);
    return out;
}

std::string format_tracestate(const TraceContext& context) {
    return std::string(context.tracestate_view());
}

} // namespace hpactor::tracing
```

Add `src/tracing/trace_context_parser.cpp` to the `ENABLE_ACTOR_TRACING` `target_sources` block in `CMakeLists.txt`.

- [ ] **Step 9: Run the parser tests**

Run:

```bash
ninja -C build test_trace_context test_w3c_trace_context
./build/tests/test_trace_context
./build/tests/test_w3c_trace_context
```

Expected: both tests pass.

- [ ] **Step 10: Commit**

```bash
git add include/hpactor/types/types.hpp include/hpactor/tracing/trace_context_parser.hpp src/tracing/trace_context_parser.cpp CMakeLists.txt tests/CMakeLists.txt tests/tracing/test_trace_context.cpp tests/tracing/test_w3c_trace_context.cpp
git commit -m "feat(tracing): add W3C trace context model"
```

---

### Task 3: Add Trace Sidecar to TypedMessage

**Files:**
- Modify: `include/hpactor/actor/typed_message.hpp`
- Create: `tests/tracing/test_trace_typed_message.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing TypedMessage sidecar test**

Create `tests/tracing/test_trace_typed_message.cpp`:

```cpp
#include <hpactor/actor/typed_message.hpp>

#include <cassert>

using namespace hpactor;

static TraceContext make_context() {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 1;
    ctx.span_id.bytes[7] = 2;
    ctx.flags.set_sampled(true);
    return ctx;
}

int main() {
    TypedMessage msg(TypeTag::User, StreamBuffer{1, 2, 3});
    assert(!msg.has_trace_context());

    TraceContext ctx = make_context();
    msg.set_trace_context(ctx);
    assert(msg.has_trace_context());
    assert(msg.trace_context().trace_id == ctx.trace_id);
    assert(msg.trace_context().span_id == ctx.span_id);
    assert(msg.trace_context().sampled());

    TypedMessage moved(std::move(msg));
    assert(moved.has_trace_context());
    assert(moved.trace_context().trace_id == ctx.trace_id);

    TypedMessage assigned(TypeTag::User, StreamBuffer{});
    assigned = std::move(moved);
    assert(assigned.has_trace_context());
    assert(assigned.trace_context().span_id == ctx.span_id);

    assigned.clear_trace_context();
    assert(!assigned.has_trace_context());
    assert(!assigned.trace_context().valid());
    return 0;
}
```

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(test_trace_typed_message tracing/test_trace_typed_message.cpp)
target_link_libraries(test_trace_typed_message hpactor)
add_test(NAME test_trace_typed_message COMMAND test_trace_typed_message)
```

- [ ] **Step 2: Run the failing test build**

Run:

```bash
ninja -C build test_trace_typed_message
```

Expected: compile fails because `TypedMessage` has no trace context API.

- [ ] **Step 3: Add the sidecar API to TypedMessage**

In `include/hpactor/actor/typed_message.hpp`, add to the public section after sender address accessors:

```cpp
bool has_trace_context() const noexcept { return has_trace_context_; }

const TraceContext& trace_context() const noexcept {
    return trace_context_;
}

void set_trace_context(const TraceContext& ctx) noexcept {
    trace_context_ = ctx;
    has_trace_context_ = ctx.valid();
}

void clear_trace_context() noexcept {
    trace_context_.clear();
    has_trace_context_ = false;
}
```

Add private members:

```cpp
TraceContext trace_context_;
bool has_trace_context_ = false;
```

Update the move constructor initializer list:

```cpp
TypedMessage(TypedMessage&& other) noexcept
    : tag_(other.tag_),
      payload_(std::move(other.payload_)),
      parsed_(std::move(other.parsed_)),
      sender_address_(other.sender_address_),
      trace_context_(other.trace_context_),
      has_trace_context_(other.has_trace_context_) {}
```

Update move assignment:

```cpp
trace_context_ = other.trace_context_;
has_trace_context_ = other.has_trace_context_;
```

- [ ] **Step 4: Run the TypedMessage test**

Run:

```bash
ninja -C build test_trace_typed_message
./build/tests/test_trace_typed_message
```

Expected: test passes.

- [ ] **Step 5: Run existing actor context tests**

Run:

```bash
ninja -C build test_actor_context test_event_based_actor
./build/tests/test_actor_context
./build/tests/test_event_based_actor
```

Expected: both tests pass; trace sidecar did not break existing moves or mailbox use.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/actor/typed_message.hpp tests/CMakeLists.txt tests/tracing/test_trace_typed_message.cpp
git commit -m "feat(tracing): carry trace context on TypedMessage"
```

---

### Task 4: Implement Samplers, Span Model, Memory Exporter, and TraceManager Core

**Files:**
- Create: `include/hpactor/tracing/span.hpp`
- Create: `include/hpactor/tracing/sampler.hpp`
- Create: `src/tracing/sampler.cpp`
- Create: `include/hpactor/tracing/trace_exporter.hpp`
- Create: `include/hpactor/tracing/memory_exporter.hpp`
- Create: `src/tracing/memory_exporter.cpp`
- Create: `include/hpactor/tracing/trace_manager.hpp`
- Replace: `src/tracing/trace_manager.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/tracing/test_sampler.cpp`
- Create: `tests/tracing/test_trace_manager.cpp`

- [ ] **Step 1: Write the failing sampler test**

Create `tests/tracing/test_sampler.cpp`:

```cpp
#include <hpactor/tracing/sampler.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::tracing;

static TraceId make_trace(uint8_t last) {
    TraceId id;
    id.bytes[15] = last;
    return id;
}

int main() {
    SamplingParameters params;
    params.trace_id = make_trace(1);

    AlwaysOnSampler on;
    assert(on.should_sample(params).sampled);

    AlwaysOffSampler off;
    assert(!off.should_sample(params).sampled);

    TraceIdRatioSampler zero(0.0);
    assert(!zero.should_sample(params).sampled);

    TraceIdRatioSampler one(1.0);
    assert(one.should_sample(params).sampled);

    ParentBasedSampler parent_on(0.0);
    params.has_parent = true;
    params.parent_sampled = true;
    assert(parent_on.should_sample(params).sampled);

    params.parent_sampled = false;
    assert(!parent_on.should_sample(params).sampled);
    return 0;
}
```

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(test_sampler tracing/test_sampler.cpp)
target_link_libraries(test_sampler hpactor)
add_test(NAME test_sampler COMMAND test_sampler)
```

- [ ] **Step 2: Run the failing sampler test build**

Run:

```bash
ninja -C build test_sampler
```

Expected: compile fails because `sampler.hpp` does not exist.

- [ ] **Step 3: Add span model header**

Create `include/hpactor/tracing/span.hpp`:

```cpp
#pragma once

#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string_view>

namespace hpactor::tracing {

enum class SpanKind : uint8_t {
    kInternal,
    kServer,
    kClient,
    kProducer,
    kConsumer,
};

enum class SpanStatus : uint8_t {
    kUnset,
    kOk,
    kError,
};

struct SpanStart {
    std::string_view name;
    SpanKind kind{SpanKind::kInternal};
    TraceContext parent{};
    bool has_parent{false};
    ActorId actor_id{};
    ActorId sender_actor_id{};
    TypeTag type_tag{TypeTag::Invalid};
    MessageId message_id{};
    uint32_t payload_size{0};
};

struct SpanHandle {
    TraceContext context{};
    SpanId parent_span_id{};
    uint64_t start_ns{0};
    SpanKind kind{SpanKind::kInternal};
    ActorId actor_id{};
    ActorId sender_actor_id{};
    TypeTag type_tag{TypeTag::Invalid};
    MessageId message_id{};
    uint32_t payload_size{0};
    bool recording{false};
};

struct SpanRecord {
    TraceId trace_id;
    SpanId span_id;
    SpanId parent_span_id;
    ActorId actor_id;
    ActorId sender_actor_id;
    uint32_t type_tag{0};
    uint64_t message_id{0};
    uint64_t start_ns{0};
    uint64_t end_ns{0};
    uint32_t payload_size{0};
    SpanKind kind{SpanKind::kInternal};
    SpanStatus status{SpanStatus::kUnset};
    uint16_t attribute_mask{0};
};

} // namespace hpactor::tracing
```

- [ ] **Step 4: Add sampler header and implementation**

Create `include/hpactor/tracing/sampler.hpp`:

```cpp
#pragma once

#include <hpactor/types/types.hpp>

#include <cstdint>

namespace hpactor::tracing {

struct SamplingParameters {
    TraceId trace_id;
    bool has_parent{false};
    bool parent_sampled{false};
};

struct SamplingDecision {
    bool sampled{false};
};

class Sampler {
public:
    virtual ~Sampler() = default;
    virtual SamplingDecision should_sample(
        const SamplingParameters& params) const noexcept = 0;
};

class AlwaysOnSampler final : public Sampler {
public:
    SamplingDecision should_sample(
        const SamplingParameters& params) const noexcept override;
};

class AlwaysOffSampler final : public Sampler {
public:
    SamplingDecision should_sample(
        const SamplingParameters& params) const noexcept override;
};

class TraceIdRatioSampler final : public Sampler {
public:
    explicit TraceIdRatioSampler(double ratio);
    SamplingDecision should_sample(
        const SamplingParameters& params) const noexcept override;

private:
    double ratio_{0.0};
};

class ParentBasedSampler final : public Sampler {
public:
    explicit ParentBasedSampler(double root_ratio);
    SamplingDecision should_sample(
        const SamplingParameters& params) const noexcept override;

private:
    TraceIdRatioSampler root_sampler_;
};

class TraceIdGenerator {
public:
    TraceIdGenerator() = default;
    TraceId next_trace_id() noexcept;
    SpanId next_span_id() noexcept;

private:
    std::atomic<uint64_t> counter_{1};
};

} // namespace hpactor::tracing
```

Create `src/tracing/sampler.cpp`:

```cpp
#include <hpactor/tracing/sampler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>

namespace hpactor::tracing {

namespace {

uint64_t splitmix64(uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

uint64_t trace_low64(const TraceId& id) noexcept {
    uint64_t value = 0;
    for (size_t i = 8; i < 16; ++i) {
        value = (value << 8) | id.bytes[i];
    }
    return value;
}

} // namespace

SamplingDecision AlwaysOnSampler::should_sample(
    const SamplingParameters& /*params*/) const noexcept {
    return {true};
}

SamplingDecision AlwaysOffSampler::should_sample(
    const SamplingParameters& /*params*/) const noexcept {
    return {false};
}

TraceIdRatioSampler::TraceIdRatioSampler(double ratio)
    : ratio_(std::clamp(ratio, 0.0, 1.0)) {}

SamplingDecision TraceIdRatioSampler::should_sample(
    const SamplingParameters& params) const noexcept {
    if (ratio_ <= 0.0) return {false};
    if (ratio_ >= 1.0) return {true};
    const uint64_t threshold =
        static_cast<uint64_t>(ratio_ * static_cast<double>(UINT64_MAX));
    return {splitmix64(trace_low64(params.trace_id)) <= threshold};
}

ParentBasedSampler::ParentBasedSampler(double root_ratio)
    : root_sampler_(root_ratio) {}

SamplingDecision ParentBasedSampler::should_sample(
    const SamplingParameters& params) const noexcept {
    if (params.has_parent) {
        return {params.parent_sampled};
    }
    return root_sampler_.should_sample(params);
}

TraceId TraceIdGenerator::next_trace_id() noexcept {
    TraceId id;
    const uint64_t n = counter_.fetch_add(1, std::memory_order_relaxed);
    uint64_t hi = splitmix64(n);
    uint64_t lo = splitmix64(n ^ 0xd1b54a32d192ed03ULL);
    for (int i = 7; i >= 0; --i) {
        id.bytes[static_cast<size_t>(7 - i)] = static_cast<uint8_t>((hi >> (i * 8)) & 0xFF);
        id.bytes[static_cast<size_t>(15 - i)] = static_cast<uint8_t>((lo >> (i * 8)) & 0xFF);
    }
    if (!id.valid()) {
        id.bytes[15] = 1;
    }
    return id;
}

SpanId TraceIdGenerator::next_span_id() noexcept {
    SpanId id;
    const uint64_t n = counter_.fetch_add(1, std::memory_order_relaxed);
    uint64_t value = splitmix64(n ^ 0x94d049bb133111ebULL);
    for (int i = 7; i >= 0; --i) {
        id.bytes[static_cast<size_t>(7 - i)] =
            static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
    if (!id.valid()) {
        id.bytes[7] = 1;
    }
    return id;
}

} // namespace hpactor::tracing
```

Add `src/tracing/sampler.cpp` to the tracing `target_sources` block.

- [ ] **Step 5: Run sampler test**

Run:

```bash
ninja -C build test_sampler
./build/tests/test_sampler
```

Expected: test passes.

- [ ] **Step 6: Write the failing TraceManager test**

Create `tests/tracing/test_trace_manager.cpp`:

```cpp
#include <hpactor/tracing/memory_exporter.hpp>
#include <hpactor/tracing/trace_manager.hpp>

#include <cassert>
#include <memory>

using namespace hpactor;
using namespace hpactor::tracing;

int main() {
    TraceConfig disabled_cfg;
    disabled_cfg.enabled = false;
    auto disabled_exporter = std::make_unique<MemoryExporter>();
    TraceManager disabled(disabled_cfg, nullptr, std::move(disabled_exporter));
    SpanStart disabled_start;
    disabled_start.name = "disabled";
    SpanHandle disabled_span = disabled.start_span(disabled_start);
    assert(!disabled_span.recording);

    TraceConfig cfg;
    cfg.enabled = true;
    cfg.exporter = TraceExporterKind::kMemory;
    cfg.sampler = SamplerKind::kAlwaysOn;
    cfg.export_interval = std::chrono::milliseconds(100000);
    auto* memory = new MemoryExporter();
    TraceManager manager(cfg, nullptr, std::unique_ptr<SpanExporter>(memory));
    manager.start();

    SpanStart start;
    start.name = "hpactor.test";
    start.kind = SpanKind::kInternal;
    start.actor_id = ActorId{42};
    SpanHandle span = manager.start_span(start);
    assert(span.recording);
    assert(span.context.valid());
    manager.finish_span(span, SpanStatus::kOk);
    manager.force_flush();

    auto spans = memory->snapshot();
    assert(spans.size() == 1);
    assert(spans[0].actor_id == ActorId{42});
    assert(spans[0].status == SpanStatus::kOk);
    manager.stop();
    return 0;
}
```

Add:

```cmake
add_executable(test_trace_manager tracing/test_trace_manager.cpp)
target_link_libraries(test_trace_manager hpactor pthread)
add_test(NAME test_trace_manager COMMAND test_trace_manager)
```

- [ ] **Step 7: Run the failing TraceManager test build**

Run:

```bash
ninja -C build test_trace_manager
```

Expected: compile fails because exporter and manager headers do not exist.

- [ ] **Step 8: Add exporter interfaces and memory exporter**

Create `include/hpactor/tracing/trace_exporter.hpp`:

```cpp
#pragma once

#include <hpactor/tracing/span.hpp>
#include <hpactor/types/types.hpp>

#include <span>

namespace hpactor::tracing {

class SpanExporter {
public:
    virtual ~SpanExporter() = default;
    virtual result<void> export_batch(std::span<const SpanRecord> batch) noexcept = 0;
    virtual void shutdown() noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

} // namespace hpactor::tracing
```

Create `include/hpactor/tracing/memory_exporter.hpp`:

```cpp
#pragma once

#include <hpactor/tracing/trace_exporter.hpp>

#include <mutex>
#include <vector>

namespace hpactor::tracing {

class MemoryExporter final : public SpanExporter {
public:
    result<void> export_batch(std::span<const SpanRecord> batch) noexcept override;
    void shutdown() noexcept override {}
    const char* name() const noexcept override { return "memory"; }
    std::vector<SpanRecord> snapshot() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<SpanRecord> spans_;
};

} // namespace hpactor::tracing
```

Create `src/tracing/memory_exporter.cpp`:

```cpp
#include <hpactor/tracing/memory_exporter.hpp>

namespace hpactor::tracing {

result<void> MemoryExporter::export_batch(
    std::span<const SpanRecord> batch) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    spans_.insert(spans_.end(), batch.begin(), batch.end());
    return result<void>::make();
}

std::vector<SpanRecord> MemoryExporter::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return spans_;
}

void MemoryExporter::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    spans_.clear();
}

} // namespace hpactor::tracing
```

- [ ] **Step 9: Add TraceManager header and implementation**

Create `include/hpactor/tracing/trace_manager.hpp`:

```cpp
#pragma once

#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/tracing/sampler.hpp>
#include <hpactor/tracing/span.hpp>
#include <hpactor/tracing/trace_config.hpp>
#include <hpactor/tracing/trace_exporter.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace hpactor {
class ActorContext;
class ActorSystem;
class TypedMessage;
} // namespace hpactor

namespace hpactor::tracing {

class TraceManager {
public:
    TraceManager(TraceConfig config, ActorSystem* system,
                 std::unique_ptr<SpanExporter> exporter = nullptr);
    ~TraceManager();

    TraceManager(const TraceManager&) = delete;
    TraceManager& operator=(const TraceManager&) = delete;

    void start();
    void stop();
    void force_flush();

    bool enabled() const noexcept { return config_.enabled; }
    const TraceConfig& config() const noexcept { return config_; }

    TraceContext create_root_context(std::string_view operation);
    TraceContext child_context(const TraceContext& parent);

    SpanHandle start_span(const SpanStart& start);
    void finish_span(SpanHandle& span, SpanStatus status) noexcept;

    void inject_message_context(TypedMessage& msg,
                                const ActorContext* ctx,
                                bool allow_root);

    uint64_t spans_dropped() const noexcept {
        return spans_dropped_.load(std::memory_order_relaxed);
    }

private:
    std::unique_ptr<Sampler> make_sampler() const;
    void drain_once();
    static uint64_t now_ns() noexcept;

    TraceConfig config_;
    ActorSystem* system_{nullptr};
    TraceIdGenerator ids_;
    std::unique_ptr<Sampler> sampler_;
    std::unique_ptr<SpanExporter> exporter_;
    metrics::MpscRingBuffer<SpanRecord> ring_;
    std::atomic<bool> running_{false};
    std::thread drain_thread_;
    std::atomic<uint64_t> spans_dropped_{0};
};

} // namespace hpactor::tracing
```

Replace `src/tracing/trace_manager.cpp` with:

```cpp
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/tracing/memory_exporter.hpp>
#include <hpactor/tracing/trace_manager.hpp>

#include <chrono>

namespace hpactor::tracing {

TraceManager::TraceManager(TraceConfig config, ActorSystem* system,
                           std::unique_ptr<SpanExporter> exporter)
    : config_(std::move(config)),
      system_(system),
      sampler_(make_sampler()),
      exporter_(std::move(exporter)) {
    if (!exporter_) {
        exporter_ = std::make_unique<MemoryExporter>();
    }
}

TraceManager::~TraceManager() {
    stop();
}

std::unique_ptr<Sampler> TraceManager::make_sampler() const {
    switch (config_.sampler) {
    case SamplerKind::kAlwaysOff:
        return std::make_unique<AlwaysOffSampler>();
    case SamplerKind::kAlwaysOn:
        return std::make_unique<AlwaysOnSampler>();
    case SamplerKind::kTraceIdRatio:
        return std::make_unique<TraceIdRatioSampler>(config_.sample_ratio);
    case SamplerKind::kParentBasedTraceIdRatio:
        return std::make_unique<ParentBasedSampler>(config_.sample_ratio);
    }
    return std::make_unique<AlwaysOffSampler>();
}

uint64_t TraceManager::now_ns() noexcept {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void TraceManager::start() {
    if (!config_.enabled || running_.exchange(true)) {
        return;
    }
    drain_thread_ = std::thread([this]() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(config_.export_interval);
            drain_once();
        }
    });
}

void TraceManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (drain_thread_.joinable()) {
        drain_thread_.join();
    }
    drain_once();
    if (exporter_) {
        exporter_->shutdown();
    }
}

void TraceManager::force_flush() {
    drain_once();
}

TraceContext TraceManager::create_root_context(std::string_view /*operation*/) {
    TraceContext ctx;
    ctx.trace_id = ids_.next_trace_id();
    ctx.span_id = ids_.next_span_id();
    SamplingParameters params;
    params.trace_id = ctx.trace_id;
    auto decision = sampler_->should_sample(params);
    ctx.flags.set_sampled(decision.sampled);
    return ctx;
}

TraceContext TraceManager::child_context(const TraceContext& parent) {
    TraceContext child = parent;
    child.span_id = ids_.next_span_id();
    return child;
}

SpanHandle TraceManager::start_span(const SpanStart& start) {
    SpanHandle handle;
    if (!config_.enabled) {
        return handle;
    }

    TraceContext ctx;
    if (start.has_parent && start.parent.valid()) {
        ctx = child_context(start.parent);
        SamplingParameters params;
        params.trace_id = ctx.trace_id;
        params.has_parent = true;
        params.parent_sampled = start.parent.sampled();
        ctx.flags.set_sampled(sampler_->should_sample(params).sampled);
        handle.parent_span_id = start.parent.span_id;
    } else {
        ctx = create_root_context(start.name);
    }

    handle.context = ctx;
    handle.start_ns = now_ns();
    handle.kind = start.kind;
    handle.actor_id = start.actor_id;
    handle.sender_actor_id = start.sender_actor_id;
    handle.type_tag = start.type_tag;
    handle.message_id = start.message_id;
    handle.payload_size = start.payload_size;
    handle.recording = ctx.sampled();
    return handle;
}

void TraceManager::finish_span(SpanHandle& span, SpanStatus status) noexcept {
    if (!span.recording) {
        return;
    }
    SpanRecord record;
    record.trace_id = span.context.trace_id;
    record.span_id = span.context.span_id;
    record.parent_span_id = span.parent_span_id;
    record.actor_id = span.actor_id;
    record.sender_actor_id = span.sender_actor_id;
    record.type_tag = static_cast<uint32_t>(span.type_tag);
    record.message_id = span.message_id.value();
    record.start_ns = span.start_ns;
    record.end_ns = now_ns();
    record.payload_size = span.payload_size;
    record.kind = span.kind;
    record.status = status;
    if (!ring_.try_push(record)) {
        spans_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    span.recording = false;
}

void TraceManager::inject_message_context(TypedMessage& msg,
                                          const ActorContext* ctx,
                                          bool allow_root) {
    if (!config_.enabled || msg.has_trace_context()) {
        return;
    }
    if (ctx != nullptr && ctx->has_current_trace_context()) {
        msg.set_trace_context(ctx->current_trace_context());
        return;
    }
    if (allow_root) {
        msg.set_trace_context(create_root_context("hpactor.message"));
    }
}

void TraceManager::drain_once() {
    if (!exporter_) {
        return;
    }
    std::vector<SpanRecord> batch;
    batch.reserve(config_.max_export_batch_size);
    ring_.drain([&](const SpanRecord& record) {
        batch.push_back(record);
        if (batch.size() >= config_.max_export_batch_size) {
            (void)exporter_->export_batch(std::span<const SpanRecord>(batch.data(), batch.size()));
            batch.clear();
        }
    });
    if (!batch.empty()) {
        (void)exporter_->export_batch(std::span<const SpanRecord>(batch.data(), batch.size()));
    }
}

} // namespace hpactor::tracing
```

Add `src/tracing/memory_exporter.cpp` to the tracing `target_sources` block.

- [ ] **Step 10: Run TraceManager tests**

Run:

```bash
ninja -C build test_sampler test_trace_manager
./build/tests/test_sampler
./build/tests/test_trace_manager
```

Expected: both tests pass.

- [ ] **Step 11: Commit**

```bash
git add include/hpactor/tracing/span.hpp include/hpactor/tracing/sampler.hpp include/hpactor/tracing/trace_exporter.hpp include/hpactor/tracing/memory_exporter.hpp include/hpactor/tracing/trace_manager.hpp src/tracing/sampler.cpp src/tracing/memory_exporter.cpp src/tracing/trace_manager.cpp CMakeLists.txt tests/CMakeLists.txt tests/tracing/test_sampler.cpp tests/tracing/test_trace_manager.cpp
git commit -m "feat(tracing): add trace manager core"
```

---

### Task 5: Wire TraceManager Into ActorSystem and ActorContext

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `include/hpactor/actor_context.hpp`
- Modify: `src/actor/actor_context.cpp`
- Create: `tests/tracing/test_trace_actor_system.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing ActorSystem ownership test**

Create `tests/tracing/test_trace_actor_system.cpp`:

```cpp
#include <hpactor/core/actor_system.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    Config disabled;
    disabled.tracing.enabled = false;
    ActorSystem no_trace(disabled);
    assert(no_trace.trace_manager() == nullptr);

    Config enabled;
    enabled.tracing.enabled = true;
    enabled.tracing.exporter = tracing::TraceExporterKind::kMemory;
    enabled.tracing.sampler = tracing::SamplerKind::kAlwaysOn;
    ActorSystem with_trace(enabled);
    assert(with_trace.trace_manager() != nullptr);
    assert(with_trace.trace_manager()->enabled());
    return 0;
}
```

Add:

```cmake
add_executable(test_trace_actor_system tracing/test_trace_actor_system.cpp)
target_link_libraries(test_trace_actor_system hpactor pthread)
add_test(NAME test_trace_actor_system COMMAND test_trace_actor_system)
```

- [ ] **Step 2: Run the failing build**

Run:

```bash
ninja -C build test_trace_actor_system
```

Expected: compile fails because `Config::tracing` and `ActorSystem::trace_manager()` do not exist.

- [ ] **Step 3: Add tracing ownership to ActorSystem**

In `include/hpactor/core/actor_system.hpp`, include:

```cpp
#include <hpactor/tracing/trace_config.hpp>
#include <hpactor/tracing/trace_manager.hpp>
```

Add to `Config`:

```cpp
tracing::TraceConfig tracing;
```

Add public methods:

```cpp
tracing::TraceManager* trace_manager() noexcept {
    return trace_manager_.get();
}

const tracing::TraceManager* trace_manager() const noexcept {
    return trace_manager_.get();
}

void apply_tracing_config(const tracing::TraceConfig& config);
```

Add private members:

```cpp
tracing::TraceConfig tracing_config_;
std::unique_ptr<tracing::TraceManager> trace_manager_;
```

In `src/actor/actor_system.cpp`, call after `scheduler_->start();`:

```cpp
apply_tracing_config(config_.tracing);
```

Add method:

```cpp
void ActorSystem::apply_tracing_config(const tracing::TraceConfig& config) {
    tracing_config_ = config;
    if (!tracing_config_.enabled) {
        if (trace_manager_) {
            trace_manager_->stop();
            trace_manager_.reset();
        }
        return;
    }
    trace_manager_ = std::make_unique<tracing::TraceManager>(tracing_config_, this);
    trace_manager_->start();
}
```

In the destructor, before `scheduler_->stop();`, add:

```cpp
if (trace_manager_) {
    trace_manager_->stop();
}
```

- [ ] **Step 4: Run ActorSystem ownership test**

Run:

```bash
ninja -C build test_trace_actor_system
./build/tests/test_trace_actor_system
```

Expected: test passes.

- [ ] **Step 5: Write the failing ActorContext current trace test**

Create `tests/tracing/test_trace_actor_context.cpp`:

```cpp
#include <hpactor/actor_context.hpp>

#include <cassert>

using namespace hpactor;

static TraceContext make_context() {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 3;
    ctx.span_id.bytes[7] = 4;
    return ctx;
}

int main() {
    ActorContext ctx(Actor{});
    assert(!ctx.has_current_trace_context());
    TraceContext trace = make_context();
    {
        ActorContext::TraceScope scope(&ctx, trace);
        assert(ctx.has_current_trace_context());
        assert(ctx.current_trace_context().trace_id == trace.trace_id);
    }
    assert(!ctx.has_current_trace_context());
    return 0;
}
```

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(test_trace_actor_context tracing/test_trace_actor_context.cpp)
target_link_libraries(test_trace_actor_context hpactor pthread)
add_test(NAME test_trace_actor_context COMMAND test_trace_actor_context)
```

- [ ] **Step 6: Run failing ActorContext trace test**

Run:

```bash
ninja -C build test_trace_actor_context
```

Expected: compile fails because `ActorContext::TraceScope` does not exist.

- [ ] **Step 7: Add current trace scope to ActorContext**

In `include/hpactor/actor_context.hpp`, add public methods:

```cpp
bool has_current_trace_context() const noexcept {
    return has_current_trace_context_;
}

const TraceContext& current_trace_context() const noexcept {
    return current_trace_context_;
}

class TraceScope {
public:
    TraceScope(ActorContext* ctx, const TraceContext& next) noexcept;
    ~TraceScope();
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

private:
    ActorContext* ctx_{nullptr};
    TraceContext previous_{};
    bool previous_valid_{false};
};
```

Add private helpers and members:

```cpp
void set_current_trace_context(const TraceContext& context) noexcept {
    current_trace_context_ = context;
    has_current_trace_context_ = context.valid();
}

void clear_current_trace_context() noexcept {
    current_trace_context_.clear();
    has_current_trace_context_ = false;
}

TraceContext current_trace_context_;
bool has_current_trace_context_{false};
```

In `src/actor/actor_context.cpp`, implement:

```cpp
ActorContext::TraceScope::TraceScope(ActorContext* ctx,
                                     const TraceContext& next) noexcept
    : ctx_(ctx) {
    if (ctx_ == nullptr) {
        return;
    }
    previous_ = ctx_->current_trace_context_;
    previous_valid_ = ctx_->has_current_trace_context_;
    ctx_->set_current_trace_context(next);
}

ActorContext::TraceScope::~TraceScope() {
    if (ctx_ == nullptr) {
        return;
    }
    if (previous_valid_) {
        ctx_->set_current_trace_context(previous_);
    } else {
        ctx_->clear_current_trace_context();
    }
}
```

- [ ] **Step 8: Inject trace context in ActorContext send paths**

In `ActorContext::send(ActorRef& target, TypedMessage msg)`, after stamping sender:

```cpp
auto* system = system_ != nullptr ? system_
    : (owner_ ? &owner_.get()->system() : nullptr);
if (system != nullptr && system->trace_manager() != nullptr) {
    system->trace_manager()->inject_message_context(
        msg, this, system->trace_manager()->config().create_roots_for_actor_context_sends);
}
```

In `send_with_priority`, add the same injection after stamping sender and before local/remote delivery.

- [ ] **Step 9: Run ActorContext tests**

Run:

```bash
ninja -C build test_trace_actor_context test_actor_context
./build/tests/test_trace_actor_context
./build/tests/test_actor_context
```

Expected: both tests pass.

- [ ] **Step 10: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp include/hpactor/actor_context.hpp src/actor/actor_context.cpp tests/CMakeLists.txt tests/tracing/test_trace_actor_system.cpp tests/tracing/test_trace_actor_context.cpp
git commit -m "feat(tracing): wire trace manager into actor system"
```

---

### Task 6: Add Local Actor Receive Spans and Propagation Tests

**Files:**
- Modify: `src/actor/event_based_actor.cpp`
- Create: `tests/tracing/test_trace_message_propagation.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing local propagation test**

Create `tests/tracing/test_trace_message_propagation.cpp`:

```cpp
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>

using namespace hpactor;

class CaptureActor final : public EventBasedActor {
public:
    CaptureActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            saw_trace = msg.has_trace_context();
            if (saw_trace) {
                trace = msg.trace_context();
            }
        });
    }

    bool saw_trace{false};
    TraceContext trace{};
};

int main() {
    Config cfg;
    cfg.tracing.enabled = true;
    cfg.tracing.exporter = tracing::TraceExporterKind::kMemory;
    cfg.tracing.sampler = tracing::SamplerKind::kAlwaysOn;
    cfg.tracing.create_roots_for_actor_context_sends = true;
    ActorSystem system(cfg);

    auto sender = system.spawn<EventBasedActor>();
    auto receiver = system.spawn<CaptureActor>();
    ActorContext ctx(sender, &system);

    TypedMessage msg(TypeTag::User, StreamBuffer{7});
    ctx.send(receiver.address(), std::move(msg));

    auto* mailbox = system.get_mailbox(receiver.id());
    TypedMessage received;
    assert(mailbox->try_pop(received));
    receiver.get()->receive(received);

    auto* cap = static_cast<CaptureActor*>(receiver.get().get());
    assert(cap->saw_trace);
    assert(cap->trace.valid());

    system.trace_manager()->force_flush();
    assert(system.trace_manager()->spans_dropped() == 0);
    return 0;
}
```

Add:

```cmake
add_executable(test_trace_message_propagation tracing/test_trace_message_propagation.cpp)
target_link_libraries(test_trace_message_propagation hpactor pthread)
add_test(NAME test_trace_message_propagation COMMAND test_trace_message_propagation)
```

- [ ] **Step 2: Run the failing test**

Run:

```bash
ninja -C build test_trace_message_propagation
./build/tests/test_trace_message_propagation
```

Expected: test fails because `EventBasedActor::receive()` does not start a receive span or install current trace scope.

- [ ] **Step 3: Add receive span guard**

In `src/actor/event_based_actor.cpp`, add a local guard near the top of the file:

```cpp
namespace {

class ReceiveSpanGuard {
public:
    ReceiveSpanGuard(tracing::TraceManager* manager,
                     tracing::SpanHandle* handle) noexcept
        : manager_(manager), handle_(handle) {}

    ~ReceiveSpanGuard() {
        if (manager_ != nullptr && handle_ != nullptr) {
            manager_->finish_span(*handle_, status_);
        }
    }

    void set_status(tracing::SpanStatus status) noexcept {
        status_ = status;
    }

private:
    tracing::TraceManager* manager_{nullptr};
    tracing::SpanHandle* handle_{nullptr};
    tracing::SpanStatus status_{tracing::SpanStatus::kOk};
};

} // namespace
```

Include:

```cpp
#include <hpactor/tracing/trace_manager.hpp>
```

- [ ] **Step 4: Start receive spans in EventBasedActor::receive**

At the start of `EventBasedActor::receive()` after `auto* ctx = context();`, add:

```cpp
auto* trace_manager = system().trace_manager();
tracing::SpanHandle receive_span;
std::unique_ptr<ActorContext::TraceScope> trace_scope;

if (trace_manager != nullptr &&
    trace_manager->enabled() &&
    trace_manager->config().record_actor_receive_spans) {
    tracing::SpanStart start;
    start.name = "hpactor.actor.receive";
    start.kind = tracing::SpanKind::kConsumer;
    start.has_parent = msg.has_trace_context();
    if (msg.has_trace_context()) {
        start.parent = msg.trace_context();
    }
    start.actor_id = id();
    start.sender_actor_id = msg.sender_address().id;
    start.type_tag = msg.type_id();
    start.payload_size = static_cast<uint32_t>(msg.payload().size());
    receive_span = trace_manager->start_span(start);
    if (ctx != nullptr && receive_span.context.valid()) {
        trace_scope = std::make_unique<ActorContext::TraceScope>(
            ctx, receive_span.context);
    }
}

ReceiveSpanGuard span_guard(trace_manager, &receive_span);
```

Add `<memory>` include if missing.

- [ ] **Step 5: Run local propagation tests**

Run:

```bash
ninja -C build test_trace_message_propagation test_event_based_actor
./build/tests/test_trace_message_propagation
./build/tests/test_event_based_actor
```

Expected: both tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/actor/event_based_actor.cpp tests/CMakeLists.txt tests/tracing/test_trace_message_propagation.cpp
git commit -m "feat(tracing): record local actor receive spans"
```

---

### Task 7: Add Wire Protocol Trace Context Propagation

**Files:**
- Modify: `include/hpactor/types/types.hpp`
- Modify: `protos/hpactor/frame.proto`
- Modify: `include/hpactor/net/frame.hpp`
- Modify: `src/net/frame_protobuf.cpp`
- Modify: `src/ref/actor_proxy.cpp`
- Modify: `src/actor/actor_system.cpp`
- Create: `tests/tracing/test_trace_wire_propagation.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing wire propagation test**

Create `tests/tracing/test_trace_wire_propagation.cpp`:

```cpp
#include <hpactor/net/frame.hpp>

#include <cassert>

using namespace hpactor;

static TraceContext make_context() {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 9;
    ctx.span_id.bytes[7] = 8;
    ctx.flags.set_sampled(true);
    return ctx;
}

int main() {
    TraceContext ctx = make_context();
    hpactor::net::WireFrame frame;
    hpactor::net::to_proto(frame.pb_frame.mutable_trace_context(), ctx);
    assert(frame.pb_frame.has_trace_context());

    auto parsed = hpactor::net::trace_context_from_proto(
        frame.pb_frame.trace_context(), 256);
    assert(parsed.has_value());
    assert(parsed.value().trace_id == ctx.trace_id);
    assert(parsed.value().span_id == ctx.span_id);
    assert(parsed.value().sampled());

    auto encoded = frame.encode();
    auto decoded = hpactor::net::WireFrame::decode(encoded);
    assert(decoded.pb_frame.has_trace_context());
    return 0;
}
```

Add:

```cmake
add_executable(test_trace_wire_propagation tracing/test_trace_wire_propagation.cpp)
target_link_libraries(test_trace_wire_propagation hpactor hpactor_proto pthread)
add_test(NAME test_trace_wire_propagation COMMAND test_trace_wire_propagation)
```

- [ ] **Step 2: Run failing build**

Run:

```bash
ninja -C build test_trace_wire_propagation
```

Expected: compile fails because `PbTraceContext` and helper functions do not exist.

- [ ] **Step 3: Update frame.proto**

In `protos/hpactor/frame.proto`, add before `ActorMsgFrame`:

```proto
message PbTraceContext {
  bytes trace_id = 1;
  bytes span_id = 2;
  uint32 flags = 3;
  string tracestate = 4;
}
```

Add field 7 to `ActorMsgFrame`:

```proto
PbTraceContext trace_context = 7;
```

- [ ] **Step 4: Add an invalid-argument error code**

In `include/hpactor/types/types.hpp`, add this error code in namespace
`hpactor::errors` after `timeout`:

```cpp
constexpr uint32_t invalid_argument = 6;
```

- [ ] **Step 5: Add helper declarations**

In `include/hpactor/net/frame.hpp`, add:

```cpp
void to_proto(::hpactor::net::PbTraceContext* pb,
              const TraceContext& context);

result<TraceContext>
trace_context_from_proto(const ::hpactor::net::PbTraceContext& pb,
                         uint16_t max_tracestate_len);
```

- [ ] **Step 6: Add helper definitions**

In `src/net/frame_protobuf.cpp`, add:

```cpp
void to_proto(::hpactor::net::PbTraceContext* pb,
              const TraceContext& context) {
    pb->set_trace_id(reinterpret_cast<const char*>(context.trace_id.bytes.data()),
                     context.trace_id.bytes.size());
    pb->set_span_id(reinterpret_cast<const char*>(context.span_id.bytes.data()),
                    context.span_id.bytes.size());
    pb->set_flags(context.flags.value);
    if (context.tracestate_len > 0) {
        pb->set_tracestate(context.tracestate.data(), context.tracestate_len);
    }
}

result<TraceContext>
trace_context_from_proto(const ::hpactor::net::PbTraceContext& pb,
                         uint16_t max_tracestate_len) {
    TraceContext ctx;
    if (pb.trace_id().size() != ctx.trace_id.bytes.size()) {
        return result<TraceContext>::make(error(errors::invalid_argument));
    }
    if (pb.span_id().size() != ctx.span_id.bytes.size()) {
        return result<TraceContext>::make(error(errors::invalid_argument));
    }
    std::memcpy(ctx.trace_id.bytes.data(), pb.trace_id().data(),
                ctx.trace_id.bytes.size());
    std::memcpy(ctx.span_id.bytes.data(), pb.span_id().data(),
                ctx.span_id.bytes.size());
    if (!ctx.trace_id.valid() || !ctx.span_id.valid()) {
        return result<TraceContext>::make(error(errors::invalid_argument));
    }
    ctx.flags.value = static_cast<uint8_t>(pb.flags() & 0xFF);
    if (pb.tracestate().size() > max_tracestate_len ||
        pb.tracestate().size() > ctx.tracestate.size()) {
        return result<TraceContext>::make(error(errors::invalid_argument));
    }
    if (!pb.tracestate().empty()) {
        std::memcpy(ctx.tracestate.data(), pb.tracestate().data(),
                    pb.tracestate().size());
        ctx.tracestate_len = static_cast<uint16_t>(pb.tracestate().size());
    }
    return result<TraceContext>::make(ctx);
}
```

- [ ] **Step 7: Serialize trace context on remote sends**

In `src/ref/actor_proxy.cpp`, after payload setup:

```cpp
if (msg.has_trace_context()) {
    net::to_proto(frame.pb_frame.mutable_trace_context(),
                  msg.trace_context());
}
```

- [ ] **Step 8: Deserialize trace context on remote receive**

In `ActorSystem::deliver_remote()` after setting sender:

```cpp
if (frame.pb_frame.has_trace_context()) {
    uint16_t max_state = tracing_config_.max_tracestate_len;
    auto parsed = net::trace_context_from_proto(
        frame.pb_frame.trace_context(), max_state);
    if (parsed.has_value()) {
        msg.set_trace_context(parsed.value());
    }
}
```

- [ ] **Step 9: Run wire test**

Run:

```bash
ninja -C build test_trace_wire_propagation
./build/tests/test_trace_wire_propagation
```

Expected: test passes.

- [ ] **Step 10: Run frame and actor proxy tests**

Run:

```bash
ninja -C build test_frame test_actor_proxy
./build/tests/test_frame
./build/tests/test_actor_proxy
```

Expected: both tests pass.

- [ ] **Step 11: Commit**

```bash
git add include/hpactor/types/types.hpp protos/hpactor/frame.proto include/hpactor/net/frame.hpp src/net/frame_protobuf.cpp src/ref/actor_proxy.cpp src/actor/actor_system.cpp tests/CMakeLists.txt tests/tracing/test_trace_wire_propagation.cpp
git commit -m "feat(tracing): propagate trace context over actor wire frames"
```

---

### Task 8: Propagate Trace Context Through RPC

**Files:**
- Create: `include/hpactor/rpc/rpc_types.hpp`
- Modify: `include/hpactor/rpc/rpc_channel.hpp`
- Modify: `src/rpc/rpc_channel.cpp`
- Modify: `include/hpactor/net/connection_pool.hpp`
- Modify: `src/net/connection_pool.cpp`
- Modify: `include/hpactor/net/tcp_transport.hpp`
- Modify: `src/actor/actor_system.cpp`
- Create: `tests/tracing/test_trace_rpc.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing RPC trace test**

Create `tests/tracing/test_trace_rpc.cpp`:

```cpp
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/tracing/memory_exporter.hpp>
#include <hpactor/tracing/trace_manager.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    tracing::TraceConfig cfg;
    cfg.enabled = true;
    cfg.sampler = tracing::SamplerKind::kAlwaysOn;
    auto* memory = new tracing::MemoryExporter();
    tracing::TraceManager manager(cfg, nullptr,
        std::unique_ptr<tracing::SpanExporter>(memory));
    manager.start();

    TraceContext parent = manager.create_root_context("rpc-test");
    PendingCall call;
    call.msg_id = MessageId::generate();
    call.target = ActorAddress{LocalEndpoint, ActorType{1}, ActorId{2}, 0};
    call.has_trace_context = true;
    call.trace_context = parent;
    assert(call.has_trace_context);
    assert(call.trace_context.trace_id == parent.trace_id);
    manager.stop();
    return 0;
}
```

Add:

```cmake
add_executable(test_trace_rpc tracing/test_trace_rpc.cpp)
target_link_libraries(test_trace_rpc hpactor pthread)
add_test(NAME test_trace_rpc COMMAND test_trace_rpc)
```

- [ ] **Step 2: Run failing build**

Run:

```bash
ninja -C build test_trace_rpc
```

Expected: compile fails because `PendingCall` has no trace fields.

- [ ] **Step 3: Add shared RPC response trace types**

Create `include/hpactor/rpc/rpc_types.hpp`:

```cpp
#pragma once

#include <hpactor/types/types.hpp>

namespace hpactor {

struct RpcResponseFrame {
    MessageId msg_id;
    StreamBuffer payload;
    bool has_trace_context{false};
    TraceContext trace_context{};
};

} // namespace hpactor
```

- [ ] **Step 4: Extend PendingCall and RPC response callback types**

In `include/hpactor/rpc/rpc_channel.hpp`, include:

```cpp
#include <hpactor/rpc/rpc_types.hpp>
```

Replace the existing response handler alias with:

```cpp
using RpcResponseHandler = std::function<void(const RpcResponseFrame&)>;
```

Update `PendingCall`:

```cpp
bool has_trace_context{false};
TraceContext trace_context{};
tracing::SpanHandle client_span{};
```

Add overload:

```cpp
RpcFuture<StreamBuffer>
call_raw(const ActorAddress& target,
         const StreamBuffer& encoded_request,
         std::chrono::milliseconds timeout_ms,
         const TraceContext* parent_context);
```

- [ ] **Step 5: Update ConnectionPool RPC response routing**

In `include/hpactor/net/connection_pool.hpp`, include:

```cpp
#include <hpactor/rpc/rpc_types.hpp>
```

Replace the RPC handler alias with:

```cpp
using rpc_response_handler = std::function<void(const RpcResponseFrame&)>;
```

In `src/net/connection_pool.cpp`, replace the RPC handler call with:

```cpp
if (rpc_handler_) {
    RpcResponseFrame response;
    response.msg_id = MessageId(frame.pb_frame.message_id());
    response.payload = StreamBuffer(frame.pb_frame.payload().begin(),
                                    frame.pb_frame.payload().end());
    if (frame.pb_frame.has_trace_context()) {
        auto parsed = net::trace_context_from_proto(
            frame.pb_frame.trace_context(), 256);
        if (parsed.has_value()) {
            response.has_trace_context = true;
            response.trace_context = parsed.value();
        }
    }
    rpc_handler_(response);
}
```

- [ ] **Step 6: Update RpcChannel request send**

In `RpcChannel::send_request()`, before encode:

```cpp
if (call.has_trace_context) {
    net::to_proto(frame.pb_frame.mutable_trace_context(),
                  call.trace_context);
}
```

Implement the overload:

```cpp
RpcFuture<StreamBuffer>
RpcChannel::call_raw(const ActorAddress& target,
                     const StreamBuffer& encoded_request,
                     std::chrono::milliseconds timeout_ms,
                     const TraceContext* parent_context) {
    MessageId msg_id = MessageId::generate();
    auto promise_ptr = std::make_shared<std::promise<result<StreamBuffer>>>();
    auto future = promise_ptr->get_future();
    auto* call_ptr =
        new PendingCall{.msg_id = msg_id,
                        .target = target,
                        .encoded_request = encoded_request,
                        .timeout = timeout_ms,
                        .retry_count = 0,
                        .max_retries = 5,
                        .promise = std::move(*promise_ptr),
                        .enqueued_at = std::chrono::steady_clock::now(),
                        .ready_ = false};
    if (parent_context != nullptr && parent_context->valid()) {
        call_ptr->has_trace_context = true;
        call_ptr->trace_context = *parent_context;
    }
    uint64_t key = msg_id.value();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.emplace(key, std::unique_ptr<PendingCall>(call_ptr));
    }
    send_request(*call_ptr, false);
    int64_t delay_ns = timeout_ms.count() * 1000000;
    scheduler_->schedule_after([this, msg_id]() { on_timeout(msg_id); }, delay_ns);
    return RpcFuture<StreamBuffer>(std::move(future), timeout_ms);
}
```

Make the existing overload delegate with `nullptr`.

- [ ] **Step 7: Update ActorSystem transport RPC handler**

In `src/actor/actor_system.cpp`, update:

```cpp
transport_->set_rpc_handler(
    [this](const hpactor::RpcResponseFrame& response) {
        rpc_channel_->on_response(response);
    });
```

Update `RpcChannel::on_response` signature to accept `const RpcResponseFrame&`.

- [ ] **Step 8: Pass current context from ActorContext::rpc**

In `src/actor/actor_context.cpp`, update `ActorContext::rpc()`:

```cpp
const TraceContext* trace = has_current_trace_context()
    ? &current_trace_context()
    : nullptr;
return system->rpc_channel().call_raw(target, encoded_request, timeout_ms, trace);
```

Use the resolved `system` pointer already computed in that method.

- [ ] **Step 9: Run RPC tests**

Run:

```bash
ninja -C build test_trace_rpc test_rpc_channel
./build/tests/test_trace_rpc
./build/tests/test_rpc_channel
```

Expected: both tests pass.

- [ ] **Step 10: Commit**

```bash
git add include/hpactor/rpc/rpc_types.hpp include/hpactor/rpc/rpc_channel.hpp src/rpc/rpc_channel.cpp include/hpactor/net/connection_pool.hpp src/net/connection_pool.cpp include/hpactor/net/tcp_transport.hpp src/actor/actor_system.cpp src/actor/actor_context.cpp tests/CMakeLists.txt tests/tracing/test_trace_rpc.cpp
git commit -m "feat(tracing): propagate trace context through RPC"
```

---

### Task 9: Propagate Trace Context Through Remote Spawn

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `src/actor/spawn_receiver.cpp`
- Modify: `include/hpactor/spawn.hpp` or `include/hpactor/async_actor.hpp` if pending spawn span storage belongs there
- Create: `tests/tracing/test_trace_spawn.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing spawn frame response test**

Create `tests/tracing/test_trace_spawn.cpp`:

```cpp
#include <hpactor/net/frame.hpp>
#include <hpactor/spawn.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 5;
    ctx.span_id.bytes[7] = 6;
    ctx.flags.set_sampled(true);

    net::WireFrame request;
    net::to_proto(request.pb_frame.mutable_trace_context(), ctx);
    request.pb_frame.set_message_id(123);

    net::WireFrame response;
    response.pb_frame.set_message_id(request.pb_frame.message_id());
    response.pb_frame.mutable_trace_context()->CopyFrom(
        request.pb_frame.trace_context());

    auto parsed = net::trace_context_from_proto(
        response.pb_frame.trace_context(), 256);
    assert(parsed.has_value());
    assert(parsed.value().trace_id == ctx.trace_id);
    return 0;
}
```

Add:

```cmake
add_executable(test_trace_spawn tracing/test_trace_spawn.cpp)
target_link_libraries(test_trace_spawn hpactor hpactor_proto pthread)
add_test(NAME test_trace_spawn COMMAND test_trace_spawn)
```

- [ ] **Step 2: Run current spawn and new trace spawn tests**

Run:

```bash
ninja -C build test_trace_spawn test_spawn_receiver
./build/tests/test_trace_spawn
./build/tests/spawn/test_spawn_receiver
```

Expected: new simple frame test passes after Task 7; existing spawn receiver still passes. This establishes the baseline before behavior changes.

- [ ] **Step 3: Add pending spawn span storage**

In `include/hpactor/core/actor_system.hpp`, add this private member next to
`pending_spawns_`:

```cpp
std::unordered_map<uint64_t, tracing::SpanHandle> pending_spawn_spans_;
```

In `src/actor/actor_system.cpp`, when configuring network transport after
`set_rpc_handler`, add a spawn response handler:

```cpp
transport_->set_spawn_handler([this](uint64_t message_id,
                                     const SpawnResponse& response) {
    std::shared_ptr<AsyncActor> pending;
    tracing::SpanHandle span;
    bool has_span = false;
    {
        std::lock_guard<std::mutex> lock(pending_spawns_mutex_);
        auto it = pending_spawns_.find(message_id);
        if (it != pending_spawns_.end()) {
            pending = it->second;
            pending_spawns_.erase(it);
        }
        auto span_it = pending_spawn_spans_.find(message_id);
        if (span_it != pending_spawn_spans_.end()) {
            span = span_it->second;
            has_span = true;
            pending_spawn_spans_.erase(span_it);
        }
    }
    if (pending) {
        pending->set_response(response);
    }
    if (has_span && trace_manager_) {
        trace_manager_->finish_span(
            span,
            response.error_code == spawn_errors::success
                ? tracing::SpanStatus::kOk
                : tracing::SpanStatus::kError);
    }
});
```

- [ ] **Step 4: Add trace context to spawn request frames**

In `ActorSystem::spawn_remote_async()`, after creating `frame` and before `transport_->send(...)`, add:

```cpp
if (trace_manager_ != nullptr) {
    tracing::SpanStart start;
    start.name = "hpactor.spawn_remote";
    start.kind = tracing::SpanKind::kClient;
    start.actor_id = system_actor_.id();
    auto span = trace_manager_->start_span(start);
    if (span.context.valid()) {
        net::to_proto(frame.pb_frame.mutable_trace_context(), span.context);
    }
    if (span.recording) {
        pending_spawn_spans_.emplace(msg_id, span);
    }
}
```

- [ ] **Step 5: Add trace context to spawn receiver response**

In `SpawnReceiver::handle_spawn_request()`, before sending `response_frame`:

```cpp
if (frame.pb_frame.has_trace_context()) {
    auto parsed = net::trace_context_from_proto(
        frame.pb_frame.trace_context(),
        system().trace_manager() != nullptr
            ? system().trace_manager()->config().max_tracestate_len
            : 256);
    if (parsed.has_value()) {
        tracing::SpanHandle receive_span;
        if (system().trace_manager() != nullptr) {
            tracing::SpanStart start;
            start.name = "hpactor.spawn_receive";
            start.kind = tracing::SpanKind::kConsumer;
            start.has_parent = true;
            start.parent = parsed.value();
            start.actor_id = id();
            receive_span = system().trace_manager()->start_span(start);
            if (receive_span.context.valid()) {
                net::to_proto(response_frame.pb_frame.mutable_trace_context(),
                              receive_span.context);
            }
            system().trace_manager()->finish_span(
                receive_span,
                response.error_code == spawn_errors::success
                    ? tracing::SpanStatus::kOk
                    : tracing::SpanStatus::kError);
        }
        if (!response_frame.pb_frame.has_trace_context()) {
            response_frame.pb_frame.mutable_trace_context()->CopyFrom(
                frame.pb_frame.trace_context());
        }
    }
}
```

- [ ] **Step 6: Run spawn tests**

Run:

```bash
ninja -C build test_trace_spawn test_spawn_receiver test_spawn_integration
./build/tests/test_trace_spawn
./build/tests/spawn/test_spawn_receiver
./build/tests/spawn/test_spawn_integration
```

Expected: all spawn tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp src/actor/spawn_receiver.cpp tests/CMakeLists.txt tests/tracing/test_trace_spawn.cpp
git commit -m "feat(tracing): propagate trace context through remote spawn"
```

---

### Task 10: Add HTTP Ingress and Egress Trace Propagation

**Files:**
- Modify: `include/hpactor/actor/http_gateway_actor.hpp`
- Modify: `src/actor/http_gateway_actor.cpp`
- Modify: `src/actor/actor_context.cpp`
- Modify: `include/hpactor/net/http_client.hpp`
- Modify: `src/net/http_client.cpp`
- Create: `tests/tracing/test_trace_http_propagation.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing HTTP propagation test**

Create `tests/tracing/test_trace_http_propagation.cpp`:

```cpp
#include <hpactor/tracing/trace_context_parser.hpp>

#include <cassert>
#include <vector>

using namespace hpactor;
using namespace hpactor::tracing;

static bool has_header(const std::vector<hpactor::net::HttpHeader>& headers,
                       const std::string& name) {
    for (const auto& h : headers) {
        if (h.name == name) return true;
    }
    return false;
}

int main() {
    auto parsed = parse_w3c_trace_context(
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",
        "",
        256);
    assert(parsed.status == TraceParseStatus::kOk);

    std::vector<hpactor::net::HttpHeader> headers;
    headers.push_back({"content-type", "application/json"});
    headers.push_back({"traceparent", format_traceparent(parsed.context)});
    assert(has_header(headers, "traceparent"));
    return 0;
}
```

Add:

```cmake
add_executable(test_trace_http_propagation tracing/test_trace_http_propagation.cpp)
target_link_libraries(test_trace_http_propagation hpactor pthread)
add_test(NAME test_trace_http_propagation COMMAND test_trace_http_propagation)
```

- [ ] **Step 2: Run baseline HTTP tests**

Run:

```bash
ninja -C build test_trace_http_propagation test_http_gateway
./build/tests/test_trace_http_propagation
./build/tests/test_http_gateway
```

Expected: baseline test passes; existing HTTP gateway test passes before changes.

- [ ] **Step 3: Extend PendingReply with trace data**

In `include/hpactor/actor/http_gateway_actor.hpp`, add to `PendingReply`:

```cpp
bool has_trace_context{false};
TraceContext trace_context{};
tracing::SpanHandle server_span{};
```

Add include:

```cpp
#include <hpactor/tracing/span.hpp>
```

- [ ] **Step 4: Extract and attach trace context on HTTP ingress**

In `HTTPGatewayActor::on_request()`, after query parsing and before route match:

```cpp
TraceContext http_context;
bool has_http_context = false;
tracing::SpanHandle server_span;
auto* trace_manager = system().trace_manager();
if (trace_manager != nullptr && trace_manager->enabled()) {
    auto traceparent = req.header("traceparent").value_or("");
    auto tracestate = req.header("tracestate").value_or("");
    auto parsed = tracing::parse_w3c_trace_context(
        traceparent, tracestate, trace_manager->config().max_tracestate_len);
    tracing::SpanStart start;
    start.name = "hpactor.http.server";
    start.kind = tracing::SpanKind::kServer;
    if (parsed.status == tracing::TraceParseStatus::kOk) {
        start.has_parent = true;
        start.parent = parsed.context;
    }
    server_span = trace_manager->start_span(start);
    if (server_span.context.valid()) {
        http_context = server_span.context;
        has_http_context = true;
    }
}
```

After route builder returns `msg`, apply:

```cpp
if (!msg.has_trace_context() && has_http_context) {
    msg.set_trace_context(http_context);
}
```

When creating `pending`, store:

```cpp
pending->has_trace_context = has_http_context;
pending->trace_context = http_context;
pending->server_span = server_span;
```

When creating `correlated_msg`, preserve trace:

```cpp
if (msg.has_trace_context()) {
    correlated_msg.set_trace_context(msg.trace_context());
}
```

- [ ] **Step 5: Finish HTTP server span and return response traceparent**

In `HTTPGatewayActor::on_reply()`, after retrieving `PendingReply`, preserve it
before erase:

```cpp
auto pending = std::move(it->second);
conn = pending->conn;
```

Then add response header if present:

```cpp
if (pending->has_trace_context) {
    headers.push_back({"traceparent",
        tracing::format_traceparent(pending->trace_context)});
}
```

Before returning, finish span:

```cpp
if (system().trace_manager() != nullptr) {
    system().trace_manager()->finish_span(
        pending->server_span, tracing::SpanStatus::kOk);
}
```

In `on_timeout()`, finish with `kError` before sending 504.

- [ ] **Step 6: Inject trace headers on HTTP egress**

In `ActorContext::http_request()`, before delegating to `HttpClient`, add:

```cpp
auto has_header = [](const std::vector<net::HttpHeader>& headers,
                     const std::string& name) {
    for (const auto& h : headers) {
        if (h.name == name) return true;
    }
    return false;
};

if (has_current_trace_context()) {
    const auto& trace = current_trace_context();
    if (trace.valid() && !has_header(headers, "traceparent")) {
        headers.push_back({"traceparent", tracing::format_traceparent(trace)});
    }
    if (trace.tracestate_len > 0 && !has_header(headers, "tracestate")) {
        headers.push_back({"tracestate", tracing::format_tracestate(trace)});
    }
}
```

Include `hpactor/tracing/trace_context_parser.hpp`.

- [ ] **Step 7: Run HTTP tests**

Run:

```bash
ninja -C build test_trace_http_propagation test_http_gateway
./build/tests/test_trace_http_propagation
./build/tests/test_http_gateway
ctest --test-dir build -R http --output-on-failure
```

Expected: `test_trace_http_propagation`, `test_http_gateway`, and all tests
matching `http` pass.

- [ ] **Step 8: Commit**

```bash
git add include/hpactor/actor/http_gateway_actor.hpp src/actor/http_gateway_actor.cpp src/actor/actor_context.cpp include/hpactor/net/http_client.hpp src/net/http_client.cpp tests/CMakeLists.txt tests/tracing/test_trace_http_propagation.cpp
git commit -m "feat(tracing): propagate trace context through HTTP"
```

---

### Task 11: Parse Tracing Config From TOML and Binary Topology

**Files:**
- Modify: `include/hpactor/config/topology_model.hpp`
- Modify: `src/config/toml_parser.cpp`
- Modify: `src/actor/actor_system.cpp`
- Modify: `include/hpactor/config/binary_format.hpp`
- Modify: `src/config/binary_serializer.cpp`
- Modify: `src/config/binary_loader.cpp`
- Modify: `tests/config/test_toml_parser.cpp`
- Modify: `tests/config/test_binary_roundtrip.cpp`
- Create: `tests/data/toml/tracing_topology.toml`

- [ ] **Step 1: Add TOML fixture**

Create `tests/data/toml/tracing_topology.toml`:

```toml
[system]
version = "1.0"
scheduler_threads = 2
enable_network = false

[system.tracing]
enabled = true
service_name = "checkout-actors"
sampler = "always_on"
sample_ratio = 1.0
propagate_unsampled = true
ring_buffer_capacity = 65536
exporter = "memory"
export_interval_ms = 250
max_export_batch_size = 128
max_tracestate_len = 128
record_actor_receive_spans = true
record_remote_producer_spans = true
record_local_producer_spans = false
record_payload_size = true
create_roots_for_actor_context_sends = true
create_roots_for_rpc = true
create_roots_for_http_ingress = true
```

- [ ] **Step 2: Write failing TOML parser assertions**

In `tests/config/test_toml_parser.cpp`, add a function:

```cpp
void test_parse_tracing_config() {
    auto path = std::string(TEST_DATA_DIR) + "/tracing_topology.toml";
    auto result = hpactor::config::TomlParser::parse(path);
    assert(result.has_value());
    const auto& tracing = result.value().system.tracing;
    assert(tracing.enabled);
    assert(tracing.service_name == "checkout-actors");
    assert(tracing.sampler == hpactor::tracing::SamplerKind::kAlwaysOn);
    assert(tracing.sample_ratio == 1.0);
    assert(tracing.exporter == hpactor::tracing::TraceExporterKind::kMemory);
    assert(tracing.export_interval.count() == 250);
    assert(tracing.max_export_batch_size == 128);
    assert(tracing.max_tracestate_len == 128);
    assert(tracing.create_roots_for_actor_context_sends);
}
```

Call it from `main()`.

- [ ] **Step 3: Run failing config test**

Run:

```bash
ninja -C build test_toml_parser
./build/tests/test_toml_parser
```

Expected: compile fails because `SystemDef::tracing` does not exist.

- [ ] **Step 4: Add tracing config to SystemDef**

In `include/hpactor/config/topology_model.hpp`, include:

```cpp
#include <hpactor/tracing/trace_config.hpp>
```

Add to `SystemDef`:

```cpp
hpactor::tracing::TraceConfig tracing;
```

- [ ] **Step 5: Parse `[system.tracing]`**

In `src/config/toml_parser.cpp`, add `read_double` next to the existing scalar
read helpers:

```cpp
static double read_double(const toml::table& tbl,
                          const char* key,
                          double def = 0.0) {
    if (auto v = tbl[key].value<double>()) {
        return *v;
    }
    if (auto v = tbl[key].value<int64_t>()) {
        return static_cast<double>(*v);
    }
    return def;
}
```

Add string parsers:

```cpp
static hpactor::tracing::SamplerKind parse_sampler(const std::string& value) {
    if (value == "always_off") return hpactor::tracing::SamplerKind::kAlwaysOff;
    if (value == "always_on") return hpactor::tracing::SamplerKind::kAlwaysOn;
    if (value == "trace_id_ratio") return hpactor::tracing::SamplerKind::kTraceIdRatio;
    return hpactor::tracing::SamplerKind::kParentBasedTraceIdRatio;
}

static hpactor::tracing::TraceExporterKind parse_exporter(const std::string& value) {
    if (value == "noop") return hpactor::tracing::TraceExporterKind::kNoop;
    if (value == "memory") return hpactor::tracing::TraceExporterKind::kMemory;
    if (value == "json_file") return hpactor::tracing::TraceExporterKind::kJsonFile;
    return hpactor::tracing::TraceExporterKind::kOtlpHttp;
}
```

Inside system table parsing, after metrics/CLI blocks:

```cpp
if (auto* tracing_node = st.get("tracing")) {
    if (tracing_node->is_table()) {
        auto& tt = *tracing_node->as_table();
        auto& cfg = data.system.tracing;
        cfg.enabled = read_bool(tt, "enabled", false);
        cfg.service_name = read_string(tt, "service_name", "hpactor");
        cfg.propagate_unsampled = read_bool(tt, "propagate_unsampled", true);
        cfg.ring_buffer_capacity = read_uint32(tt, "ring_buffer_capacity", 65536);
        cfg.sampler = parse_sampler(read_string(tt, "sampler", "parent_based_trace_id_ratio"));
        cfg.sample_ratio = read_double(tt, "sample_ratio", 0.01);
        cfg.exporter = parse_exporter(read_string(tt, "exporter", "otlp_http"));
        cfg.otlp_endpoint = read_string(tt, "otlp_endpoint", "http://127.0.0.1:4318/v1/traces");
        cfg.json_file_path = read_string(tt, "json_file_path", "");
        cfg.export_interval = std::chrono::milliseconds(
            read_uint32(tt, "export_interval_ms", 500));
        cfg.max_export_batch_size = read_uint32(tt, "max_export_batch_size", 512);
        cfg.max_tracestate_len = static_cast<uint16_t>(
            read_uint32(tt, "max_tracestate_len", 256));
        cfg.record_actor_receive_spans = read_bool(tt, "record_actor_receive_spans", true);
        cfg.record_remote_producer_spans = read_bool(tt, "record_remote_producer_spans", true);
        cfg.record_local_producer_spans = read_bool(tt, "record_local_producer_spans", false);
        cfg.record_payload_size = read_bool(tt, "record_payload_size", true);
        cfg.create_roots_for_actor_context_sends =
            read_bool(tt, "create_roots_for_actor_context_sends", false);
        cfg.create_roots_for_rpc = read_bool(tt, "create_roots_for_rpc", true);
        cfg.create_roots_for_http_ingress =
            read_bool(tt, "create_roots_for_http_ingress", true);
    }
}
```

- [ ] **Step 6: Apply topology tracing config**

In `ActorSystem::load_topology()`, after parsing `model`, call:

```cpp
apply_tracing_config(model.system.tracing);
```

Do this before spawning topology actors.

- [ ] **Step 7: Run TOML test**

Run:

```bash
ninja -C build test_toml_parser
./build/tests/test_toml_parser
```

Expected: parser test passes.

- [ ] **Step 8: Add binary format fields**

In `include/hpactor/config/binary_format.hpp`, bump binary version where defined and add fields to `BinarySystemDef`:

```cpp
uint32_t tracing_enabled;
uint32_t tracing_propagate_unsampled;
uint32_t tracing_ring_buffer_capacity;
uint32_t tracing_sampler;
uint32_t tracing_exporter;
double tracing_sample_ratio;
uint32_t tracing_export_interval_ms;
uint32_t tracing_max_export_batch_size;
uint16_t tracing_max_tracestate_len;
uint16_t tracing_pad;
uint32_t tracing_flags;
uint32_t tracing_service_name_offset;
uint32_t tracing_otlp_endpoint_offset;
uint32_t tracing_json_file_path_offset;
```

Use `tracing_flags` bits:

```text
bit 0 record_actor_receive_spans
bit 1 record_remote_producer_spans
bit 2 record_local_producer_spans
bit 3 record_payload_size
bit 4 create_roots_for_actor_context_sends
bit 5 create_roots_for_rpc
bit 6 create_roots_for_http_ingress
```

- [ ] **Step 9: Serialize and load binary tracing config**

In `src/config/binary_serializer.cpp`, populate the new fields from `model.system.tracing`.

In `src/config/binary_loader.cpp`, read the fields into `model.system.tracing`. If the binary header version is older than the new version, leave `model.system.tracing` default constructed.

- [ ] **Step 10: Extend binary roundtrip test**

In `tests/config/test_binary_roundtrip.cpp`, set tracing fields on a model before serialize and assert they survive load:

```cpp
model.system.tracing.enabled = true;
model.system.tracing.service_name = "roundtrip";
model.system.tracing.sampler = hpactor::tracing::SamplerKind::kAlwaysOn;
model.system.tracing.exporter = hpactor::tracing::TraceExporterKind::kMemory;
model.system.tracing.sample_ratio = 1.0;
```

After load:

```cpp
assert(loaded.system.tracing.enabled);
assert(loaded.system.tracing.service_name == "roundtrip");
assert(loaded.system.tracing.sampler == hpactor::tracing::SamplerKind::kAlwaysOn);
assert(loaded.system.tracing.exporter == hpactor::tracing::TraceExporterKind::kMemory);
```

- [ ] **Step 11: Run config tests**

Run:

```bash
ninja -C build test_toml_parser test_binary_roundtrip
./build/tests/test_toml_parser
./build/tests/test_binary_roundtrip
```

Expected: both tests pass.

- [ ] **Step 12: Commit**

```bash
git add include/hpactor/config/topology_model.hpp src/config/toml_parser.cpp src/actor/actor_system.cpp include/hpactor/config/binary_format.hpp src/config/binary_serializer.cpp src/config/binary_loader.cpp tests/config/test_toml_parser.cpp tests/config/test_binary_roundtrip.cpp tests/data/toml/tracing_topology.toml
git commit -m "feat(tracing): add topology tracing config"
```

---

### Task 12: Add JSON Lines and OTLP/HTTP JSON Exporters

**Files:**
- Create: `include/hpactor/tracing/json_exporter.hpp`
- Create: `src/tracing/json_exporter.cpp`
- Create: `include/hpactor/tracing/otlp_exporter.hpp`
- Create: `src/tracing/otlp_exporter.cpp`
- Modify: `src/tracing/trace_manager.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/tracing/test_trace_exporters.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write failing exporter test**

Create `tests/tracing/test_trace_exporters.cpp`:

```cpp
#include <hpactor/tracing/json_exporter.hpp>
#include <hpactor/tracing/otlp_exporter.hpp>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

using namespace hpactor;
using namespace hpactor::tracing;

static SpanRecord make_record() {
    SpanRecord r;
    r.trace_id.bytes[15] = 1;
    r.span_id.bytes[7] = 2;
    r.actor_id = ActorId{42};
    r.type_tag = static_cast<uint32_t>(TypeTag::User);
    r.start_ns = 10;
    r.end_ns = 20;
    r.kind = SpanKind::kConsumer;
    r.status = SpanStatus::kOk;
    return r;
}

int main() {
    const char* path = "/tmp/hpactor-trace-exporter-test.jsonl";
    std::remove(path);
    JsonFileExporter json(path);
    SpanRecord record = make_record();
    auto res = json.export_batch(std::span<const SpanRecord>(&record, 1));
    assert(res.has_value());
    json.shutdown();

    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    assert(line.find("\"trace_id\"") != std::string::npos);
    assert(line.find("\"actor_id\":42") != std::string::npos);

    OtlpHttpExporter otlp("http://127.0.0.1:4318/v1/traces");
    std::string body = otlp.build_json_payload_for_test(
        std::span<const SpanRecord>(&record, 1),
        "hpactor-test");
    assert(body.find("\"resourceSpans\"") != std::string::npos);
    assert(body.find("\"traceId\"") != std::string::npos);
    assert(body.find("\"spanId\"") != std::string::npos);
    return 0;
}
```

Add:

```cmake
add_executable(test_trace_exporters tracing/test_trace_exporters.cpp)
target_link_libraries(test_trace_exporters hpactor pthread)
add_test(NAME test_trace_exporters COMMAND test_trace_exporters)
```

- [ ] **Step 2: Run failing exporter build**

Run:

```bash
ninja -C build test_trace_exporters
```

Expected: compile fails because exporter headers do not exist.

- [ ] **Step 3: Implement JSON lines exporter**

Create `include/hpactor/tracing/json_exporter.hpp`:

```cpp
#pragma once

#include <hpactor/tracing/trace_exporter.hpp>

#include <fstream>
#include <mutex>
#include <string>

namespace hpactor::tracing {

class JsonFileExporter final : public SpanExporter {
public:
    explicit JsonFileExporter(std::string path);
    result<void> export_batch(std::span<const SpanRecord> batch) noexcept override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "json_file"; }

private:
    std::string path_;
    std::ofstream out_;
    std::mutex mutex_;
};

std::string span_record_to_json(const SpanRecord& record);

} // namespace hpactor::tracing
```

Create `src/tracing/json_exporter.cpp` with:

```cpp
#include <hpactor/tracing/json_exporter.hpp>
#include <hpactor/tracing/trace_context_parser.hpp>

#include <sstream>

namespace hpactor::tracing {

namespace {

std::string trace_id_hex(const TraceId& id) {
    TraceContext ctx;
    ctx.trace_id = id;
    ctx.span_id.bytes[7] = 1;
    std::string tp = format_traceparent(ctx);
    return tp.substr(3, 32);
}

std::string span_id_hex(const SpanId& id) {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 1;
    ctx.span_id = id;
    std::string tp = format_traceparent(ctx);
    return tp.substr(36, 16);
}

} // namespace

JsonFileExporter::JsonFileExporter(std::string path)
    : path_(std::move(path)), out_(path_, std::ios::app) {}

std::string span_record_to_json(const SpanRecord& record) {
    std::ostringstream os;
    os << "{\"trace_id\":\"" << trace_id_hex(record.trace_id)
       << "\",\"span_id\":\"" << span_id_hex(record.span_id)
       << "\",\"actor_id\":" << record.actor_id.value()
       << ",\"type_tag\":" << record.type_tag
       << ",\"start_ns\":" << record.start_ns
       << ",\"end_ns\":" << record.end_ns
       << ",\"kind\":" << static_cast<int>(record.kind)
       << ",\"status\":" << static_cast<int>(record.status)
       << "}";
    return os.str();
}

result<void> JsonFileExporter::export_batch(
    std::span<const SpanRecord> batch) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out_.is_open()) {
        return result<void>::make(error(errors::unknown, "trace json file not open"));
    }
    for (const auto& record : batch) {
        out_ << span_record_to_json(record) << '\n';
    }
    out_.flush();
    return result<void>::make();
}

void JsonFileExporter::shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

} // namespace hpactor::tracing
```

- [ ] **Step 4: Implement OTLP/HTTP JSON payload builder and exporter**

Create `include/hpactor/tracing/otlp_exporter.hpp`:

```cpp
#pragma once

#include <hpactor/tracing/trace_exporter.hpp>

#include <string>

namespace hpactor::tracing {

class OtlpHttpExporter final : public SpanExporter {
public:
    explicit OtlpHttpExporter(std::string endpoint);
    result<void> export_batch(std::span<const SpanRecord> batch) noexcept override;
    void shutdown() noexcept override {}
    const char* name() const noexcept override { return "otlp_http"; }

    std::string build_json_payload_for_test(std::span<const SpanRecord> batch,
                                            const std::string& service_name) const;

private:
    std::string endpoint_;
};

} // namespace hpactor::tracing
```

Create `src/tracing/otlp_exporter.cpp`:

```cpp
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/tracing/otlp_exporter.hpp>
#include <hpactor/tracing/json_exporter.hpp>
#include <hpactor/tracing/trace_context_parser.hpp>

#include <sstream>

namespace hpactor::tracing {

namespace {

std::string trace_id_hex(const TraceId& id) {
    TraceContext ctx;
    ctx.trace_id = id;
    ctx.span_id.bytes[7] = 1;
    return format_traceparent(ctx).substr(3, 32);
}

std::string span_id_hex(const SpanId& id) {
    TraceContext ctx;
    ctx.trace_id.bytes[15] = 1;
    ctx.span_id = id;
    return format_traceparent(ctx).substr(36, 16);
}

int otlp_span_kind(SpanKind kind) {
    switch (kind) {
    case SpanKind::kInternal: return 1;
    case SpanKind::kServer: return 2;
    case SpanKind::kClient: return 3;
    case SpanKind::kProducer: return 4;
    case SpanKind::kConsumer: return 5;
    }
    return 0;
}

} // namespace

OtlpHttpExporter::OtlpHttpExporter(std::string endpoint)
    : endpoint_(std::move(endpoint)) {}

std::string OtlpHttpExporter::build_json_payload_for_test(
    std::span<const SpanRecord> batch,
    const std::string& service_name) const {
    std::ostringstream os;
    os << "{\"resourceSpans\":[{\"resource\":{\"attributes\":["
       << "{\"key\":\"service.name\",\"value\":{\"stringValue\":\""
       << service_name << "\"}}]},\"scopeSpans\":[{\"scope\":{\"name\":\"hpactor-native\"},"
       << "\"spans\":[";
    for (size_t i = 0; i < batch.size(); ++i) {
        const auto& r = batch[i];
        if (i != 0) os << ',';
        os << "{\"traceId\":\"" << trace_id_hex(r.trace_id)
           << "\",\"spanId\":\"" << span_id_hex(r.span_id)
           << "\",\"name\":\"hpactor.span\""
           << ",\"kind\":" << otlp_span_kind(r.kind)
           << ",\"startTimeUnixNano\":\"" << r.start_ns << "\""
           << ",\"endTimeUnixNano\":\"" << r.end_ns << "\""
           << ",\"attributes\":["
           << "{\"key\":\"hpactor.actor.id\",\"value\":{\"intValue\":\""
           << r.actor_id.value() << "\"}},"
           << "{\"key\":\"hpactor.message.type_tag\",\"value\":{\"intValue\":\""
           << r.type_tag << "\"}}]}";
    }
    os << "]}]}]}]}";
    return os.str();
}

result<void> OtlpHttpExporter::export_batch(
    std::span<const SpanRecord> batch) noexcept {
    if (batch.empty()) {
        return result<void>::make();
    }
    std::string body_text = build_json_payload_for_test(batch, "hpactor");
    StreamBuffer body;
    body.append(reinterpret_cast<const uint8_t*>(body_text.data()),
                body_text.size());

    hpactor::net::HttpClient client(nullptr);
    std::vector<hpactor::net::HttpHeader> headers = {
        {"content-type", "application/json"},
    };
    auto response = client.post(endpoint_, std::move(body), std::move(headers)).get();
    if (!response.has_value()) {
        return result<void>::make(error(response.error().code(),
                                        response.error().message()));
    }
    return result<void>::make();
}

} // namespace hpactor::tracing
```

This uses OTLP JSON field names (`resourceSpans`, `scopeSpans`, `traceId`,
`spanId`) and numeric enum values, matching the official OTLP/HTTP JSON rules.
The unit test verifies payload shape without requiring a collector; deployments
verify the `HttpClient` POST path by pointing `otlp_endpoint` at a collector.

- [ ] **Step 5: Teach TraceManager to choose exporters**

In `TraceManager` constructor, if no exporter is injected, choose by config:

```cpp
if (!exporter_) {
    switch (config_.exporter) {
    case TraceExporterKind::kNoop:
        exporter_ = std::make_unique<MemoryExporter>();
        break;
    case TraceExporterKind::kMemory:
        exporter_ = std::make_unique<MemoryExporter>();
        break;
    case TraceExporterKind::kJsonFile:
        exporter_ = std::make_unique<JsonFileExporter>(config_.json_file_path);
        break;
    case TraceExporterKind::kOtlpHttp:
        exporter_ = std::make_unique<OtlpHttpExporter>(config_.otlp_endpoint);
        break;
    }
}
```

Include the new exporter headers in `trace_manager.cpp`.

- [ ] **Step 6: Run exporter tests**

Run:

```bash
ninja -C build test_trace_exporters test_trace_manager
./build/tests/test_trace_exporters
./build/tests/test_trace_manager
```

Expected: both tests pass.

- [ ] **Step 7: Commit**

```bash
git add include/hpactor/tracing/json_exporter.hpp src/tracing/json_exporter.cpp include/hpactor/tracing/otlp_exporter.hpp src/tracing/otlp_exporter.cpp src/tracing/trace_manager.cpp CMakeLists.txt tests/CMakeLists.txt tests/tracing/test_trace_exporters.cpp
git commit -m "feat(tracing): add trace exporters"
```

---

### Task 13: Final Verification, Compile-Off Build, and Documentation Update

**Files:**
- Modify: `CLAUDE_MEMORY.md`
- Modify: `docs/architecture/actor/distributed-tracing-design.md` if implementation decisions changed
- Modify: `docs/superpowers/specs/2026-05-10-distributed-tracing-design.md` if implementation decisions changed

- [ ] **Step 1: Run all focused tracing tests**

Run:

```bash
ninja -C build \
  test_trace_config_smoke \
  test_trace_context \
  test_w3c_trace_context \
  test_trace_typed_message \
  test_sampler \
  test_trace_manager \
  test_trace_actor_system \
  test_trace_actor_context \
  test_trace_message_propagation \
  test_trace_wire_propagation \
  test_trace_rpc \
  test_trace_spawn \
  test_trace_http_propagation \
  test_trace_exporters
./build/tests/test_trace_config_smoke
./build/tests/test_trace_context
./build/tests/test_w3c_trace_context
./build/tests/test_trace_typed_message
./build/tests/test_sampler
./build/tests/test_trace_manager
./build/tests/test_trace_actor_system
./build/tests/test_trace_actor_context
./build/tests/test_trace_message_propagation
./build/tests/test_trace_wire_propagation
./build/tests/test_trace_rpc
./build/tests/test_trace_spawn
./build/tests/test_trace_http_propagation
./build/tests/test_trace_exporters
```

Expected: all focused tracing tests pass.

- [ ] **Step 2: Run subsystem regression tests**

Run:

```bash
ninja -C build \
  test_actor_context \
  test_event_based_actor \
  test_actor_system \
  test_actor_proxy \
  test_frame \
  test_rpc_channel \
  test_spawn_receiver \
  test_spawn_integration \
  test_http_gateway \
  test_toml_parser \
  test_binary_roundtrip
./build/tests/test_actor_context
./build/tests/test_event_based_actor
./build/tests/test_actor_system
./build/tests/test_actor_proxy
./build/tests/test_frame
./build/tests/test_rpc_channel
./build/tests/spawn/test_spawn_receiver
./build/tests/spawn/test_spawn_integration
./build/tests/test_http_gateway
./build/tests/test_toml_parser
./build/tests/test_binary_roundtrip
```

Expected: all listed regression tests pass.

- [ ] **Step 3: Run the full suite**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 4: Verify compile-off mode**

Run:

```bash
cmake -S . -B build-no-tracing -GNinja -DENABLE_ACTOR_TRACING=OFF
ninja -C build-no-tracing
ctest --test-dir build-no-tracing --output-on-failure
```

Expected: build succeeds, tracing-specific test executables are not built, and the remaining tests pass.

- [ ] **Step 5: Update project memory**

In `CLAUDE_MEMORY.md`, add a new completed/current feature entry:

```markdown
**Distributed Tracing:** Complete
- W3C-compatible TraceContext propagated through TypedMessage and ActorMsgFrame
- Actor receive spans, local send/reply propagation, remote actor frame propagation
- RPC, remote spawn, HTTP ingress, and HTTP egress context propagation
- TraceManager with parent-based sampling, bounded ring buffer, memory/json/OTLP JSON exporters
- TOML `[system.tracing]` and binary topology support
- Tests: tracing unit/integration suites plus tracing compile-off verification
```

Use the actual test count from `ctest` output rather than copying a number from this plan.

- [ ] **Step 6: Commit verification docs**

```bash
git add CLAUDE_MEMORY.md docs/architecture/actor/distributed-tracing-design.md docs/superpowers/specs/2026-05-10-distributed-tracing-design.md
git commit -m "docs: record distributed tracing implementation status"
```

Only include the docs files that actually changed.

---

## Self-Review Checklist for Implementers

- [ ] `TraceContext` is fixed-size and valid only when trace ID and span ID are non-zero.
- [ ] `TypedMessage` move constructor and move assignment preserve trace context.
- [ ] `ActorContext::send()` preserves explicit message context before injecting active context.
- [ ] `EventBasedActor::receive()` finishes spans on every early return path.
- [ ] `ActorSystem::deliver_local()` does not mutate trace context.
- [ ] `ActorProxy::send()` writes `PbTraceContext` only when the message has one.
- [ ] `ActorSystem::deliver_remote()` drops malformed trace sidecars without dropping the actor message.
- [ ] RPC response routing still handles spawn responses before raw RPC responses.
- [ ] HTTP gateway request-id wrapping preserves the message trace sidecar.
- [ ] Exporters never block actor execution.
- [ ] `ENABLE_ACTOR_TRACING=OFF` still builds.
- [ ] Full `ctest --test-dir build --output-on-failure` passes before merging.
