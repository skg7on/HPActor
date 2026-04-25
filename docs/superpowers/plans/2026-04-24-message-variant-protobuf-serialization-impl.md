# MessageVariant Protobuf Serialization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `memcpy`-based `encode_system`/`decode_system` and `encode_spawn`/`decode_spawn` in `DefaultSerializer` with protobuf serialization, factoring shared endpoint types into `common.proto` imported by both `frame.proto` and `messages.proto`.

**Architecture:** Three proto files: `common.proto` (shared types), `frame.proto` (WireFrame transport), `messages.proto` (system messages). `DefaultSerializer` produces protobuf bytes stored in `WireFrame.payload`.

**Tech Stack:** Protocol Buffers, C++20, CMake, Ninja

---

## File Structure

| File | Role |
|------|------|
| `protos/hpactor/common.proto` | New — shared `ActorEndpoint`, `ActorRef`, `ActorAddress`, `Ipv4Endpoint`, `Ipv6Endpoint` |
| `protos/hpactor/messages.proto` | New — `DownMessage`, `ExitMessage`, `LinkMessage`, `UnlinkMessage`, `SpawnRequestMessage`, `SpawnResponseMessage` |
| `protos/hpactor/frame.proto` | Modify — remove duplicate endpoint types, import `common.proto` |
| `src/net/frame_protobuf.cpp` | Modify — update to use `common.proto` types instead of inline definitions |
| `src/core/serialization.cpp` | Rewrite — `encode_system`/`decode_system`/`encode_spawn`/`decode_spawn` use protobuf |
| `CMakeLists.txt` | Modify — add `common.proto` and `messages.proto` to protobuf generation |
| `include/hpactor/types/serialization.hpp` | No interface change (impl only) |

---

## Task 1: Refactor frame.proto to import common.proto

**Files:**
- Modify: `protos/hpactor/frame.proto`

- [ ] **Step 1: Create common.proto with shared types**

Create `protos/hpactor/common.proto`:

```protobuf
// protos/hpactor/common.proto
syntax = "proto3";

package hpactor;

message ActorEndpoint {
  oneof type {
    Ipv4Endpoint ipv4 = 1;
    Ipv6Endpoint ipv6 = 2;
  }
}

message ActorRef {
  ActorEndpoint endpoint = 1;
  uint32 type = 2;
  uint64 actor_id = 3;
  uint64 incarnation = 4;
}

message ActorAddress {
  ActorEndpoint endpoint = 1;
  uint32 type = 2;
  uint64 actor_id = 3;
  uint64 incarnation = 4;
}

message Ipv4Endpoint {
  fixed32 addr = 1;
  uint32 port = 2;
}

message Ipv6Endpoint {
  bytes addr = 1;
  uint32 port = 2;
}
```

- [ ] **Step 2: Rewrite frame.proto to import common.proto**

Replace the content of `protos/hpactor/frame.proto` with:

```protobuf
// protos/hpactor/frame.proto
syntax = "proto3";

package hpactor.net;

import "hpactor/common.proto";

message Frame {
  hpactor.ActorAddress sender = 1;
  hpactor.ActorAddress receiver = 2;
  uint64 message_id = 3;
  uint32 flags = 4;
  bytes payload = 5;
  uint32 type_tag = 6;
}
```

> **Note:** `hpactor.net.Frame` uses `hpactor.ActorAddress` from `common.proto` (imported as package `hpactor`). The `ActorAddress` message is referenced by its full protobuf package path.

- [ ] **Step 3: Test protobuf generation**

Run: `cmake -S . -B build -GNinja && ninja -C build 2>&1 | grep -E "(frame|common|proto)"`
Expected: `frame.pb.cc` regenerated, `common.proto` generates `common.pb.cc/h`

- [ ] **Step 4: Fix compilation errors**

Common issues after changing `ActorAddress` to `hpactor.ActorAddress`:
- Generated code uses `::hpactor::net::ActorAddress` — must be `::hpactor::ActorAddress`
- `frame_protobuf.cpp` must `#include <hpactor/common.pb.h>`

Fix `src/net/frame_protobuf.cpp` to include the common protobuf header:

```cpp
#include <hpactor/net/frame.hpp>
#include <hpactor/common.pb.h>   // ADD THIS
#include <hpactor/frame.pb.h>
```

- [ ] **Step 5: Update frame_protobuf.cpp for common.proto types**

The `to_proto`/`from_proto` helpers in `frame_protobuf.cpp` currently use `::hpactor::net::Ipv4Endpoint`, `::hpactor::net::Endpoint`, etc. These must change to `::hpactor::Ipv4Endpoint`, `::hpactor::Endpoint` (from `common.proto` package `hpactor`).

Update all type references in `src/net/frame_protobuf.cpp`:
- `::hpactor::net::Ipv4Endpoint` → `::hpactor::Ipv4Endpoint`
- `::hpactor::net::Ipv6Endpoint` → `::hpactor::Ipv6Endpoint`
- `::hpactor::net::Endpoint` → `::hpactor::Endpoint`
- `::hpactor::net::ActorAddress` → `::hpactor::ActorAddress`
- `::hpactor::net::Frame` → `::hpactor::net::Frame` (Frame stays in `hpactor.net`)

- [ ] **Step 6: Verify build**

Run: `ninja -C build 2>&1 | tail -20`
Expected: Compiles cleanly with no errors

- [ ] **Step 7: Run tests**

Run: `ctest --output-on-failure -j4`
Expected: All tests pass

- [ ] **Step 8: Commit**

```bash
git add protos/hpactor/common.proto protos/hpactor/frame.proto src/net/frame_protobuf.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
refactor(proto): factor shared endpoint types into common.proto

frame.proto now imports common.proto (package hpactor) for ActorAddress,
Endpoint, Ipv4Endpoint, Ipv6Endpoint. Frame message stays in hpactor.net.

Updates frame_protobuf.cpp to use types from common.pb.h.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Create messages.proto and add to CMake build

**Files:**
- Create: `protos/hpactor/messages.proto`
- Modify: `CMakeLists.txt:158-162`

- [ ] **Step 1: Create messages.proto**

Create `protos/hpactor/messages.proto`:

```protobuf
// protos/hpactor/messages.proto
syntax = "proto3";

package hpactor;

import "hpactor/common.proto";

message DownMessage {
  ActorEndpoint endpoint = 1;
  uint64 actor_id = 2;
  uint32 reason_code = 3;
}

message ExitMessage {
  ActorEndpoint sender = 1;
  uint64 actor_id = 2;
  uint32 reason_code = 3;
}

message LinkMessage {
  ActorEndpoint target = 1;
  uint64 actor_id = 2;
}

message UnlinkMessage {
  ActorEndpoint target = 1;
  uint64 actor_id = 2;
}

message SpawnRequestMessage {
  string actor_type_name = 1;
  uint32 args_type = 2;
  bytes serialized_args = 3;
  ActorRef supervisor = 4;
}

message SpawnResponseMessage {
  ActorAddress actor_addr = 1;
  uint32 error_code = 2;
}
```

- [ ] **Step 2: Update CMakeLists.txt to generate messages.pb**

Modify the protobuf generation block (around line 158-162) to include `messages.proto`:

```cmake
# Generate protobuf C++ headers/sources
include(${CMAKE_SOURCE_DIR}/cmake/protobuf.cmake)
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS
    ${CMAKE_SOURCE_DIR}/protos/hpactor/frame.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/common.proto
    ${CMAKE_SOURCE_DIR}/protos/hpactor/messages.proto
)
```

- [ ] **Step 3: Test protobuf generation**

Run: `cmake -S . -B build -GNinja && ninja -C build 2>&1 | grep -E "messages"`
Expected: `messages.pb.cc` and `messages.pb.h` generated

- [ ] **Step 4: Verify all protos build**

Run: `ninja -C build 2>&1 | tail -5`
Expected: No protobuf-related errors

- [ ] **Step 5: Commit**

```bash
git add protos/hpactor/messages.proto CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(proto): add messages.proto for system message serialization

DownMessage, ExitMessage, LinkMessage, UnlinkMessage,
SpawnRequestMessage, SpawnResponseMessage — all importing
common.proto for shared endpoint types.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Rewrite DefaultSerializer encode/decode with protobuf

**Files:**
- Modify: `src/core/serialization.cpp` (rewrite encode_system, decode_system, encode_spawn, decode_spawn)
- Create: `src/core/messages_protobuf.cpp` (optional — conversion helpers)

- [ ] **Step 1: Add protobuf headers to serialization.cpp**

Add at the top of `src/core/serialization.cpp`:

```cpp
#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>
```

- [ ] **Step 2: Write protobuf conversion helpers for endpoint types**

Add these static helper functions at the top of the anonymous namespace in `serialization.cpp` (before `encode_endpoint`):

```cpp
// Protobuf conversion helpers using common.proto types (package hpactor)

// HPActor Ipv4Endpoint → protobuf Ipv4Endpoint
static void to_proto(::hpactor::Ipv4Endpoint* pb_ep, const Ipv4Endpoint& ep) {
    pb_ep->set_addr(ep.addr);  // Network byte order
    pb_ep->set_port(ep.port_nw);
}

// HPActor Ipv6Endpoint → protobuf Ipv6Endpoint
static void to_proto(::hpactor::Ipv6Endpoint* pb_ep, const Ipv6Endpoint& ep) {
    pb_ep->set_addr(ep.addr.data(), 16);
    pb_ep->set_port(ep.port_nw);
}

// HPActor CommunicationEndpoint → protobuf ActorEndpoint
static void to_proto(::hpactor::ActorEndpoint* pb_endpoint, const CommunicationEndpoint& ep) {
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
        to_proto(pb_endpoint->mutable_ipv4(), *ipv4);
    } else if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&ep)) {
        to_proto(pb_endpoint->mutable_ipv6(), *ipv6);
    }
}

// protobuf Ipv4Endpoint → HPActor Ipv4Endpoint
static Ipv4Endpoint from_proto(const ::hpactor::Ipv4Endpoint& pb_ep) {
    return Ipv4Endpoint{pb_ep.addr(), static_cast<uint16_t>(pb_ep.port())};
}

// protobuf Ipv6Endpoint → HPActor Ipv6Endpoint
static Ipv6Endpoint from_proto(const ::hpactor::Ipv6Endpoint& pb_ep) {
    std::array<uint8_t, 16> addr;
    std::memcpy(addr.data(), pb_ep.addr().data(), 16);
    return Ipv6Endpoint{addr, static_cast<uint16_t>(pb_ep.port())};
}

// protobuf ActorEndpoint → HPActor CommunicationEndpoint
static CommunicationEndpoint from_proto(const ::hpactor::ActorEndpoint& pb_endpoint) {
    if (pb_endpoint.has_ipv4()) {
        return from_proto(pb_endpoint.ipv4());
    } else if (pb_endpoint.has_ipv6()) {
        return from_proto(pb_endpoint.ipv6());
    }
    return Ipv4Endpoint{};
}
```

> Keep the existing `encode_endpoint`/`decode_endpoint` functions for now — they are used in `encode_spawn`/`decode_spawn` which will also be migrated. Once all migration is done, they can be removed.

- [ ] **Step 3: Rewrite encode_system to use protobuf**

Replace the entire `encode_system` function body. The switch statement stays, but the body of each case uses protobuf instead of `memcpy`:

```cpp
bytes DefaultSerializer::encode_system(const MessageVariant& msg) {
    bytes result;

    if (std::holds_alternative<down_msg>(msg)) {
        const down_msg& m = std::get<down_msg>(msg);
        ::hpactor::DownMessage pb_msg;
        to_proto(pb_msg.mutable_endpoint(), m.terminated_actor.endpoint);
        pb_msg.set_actor_id(m.terminated_actor.id.value());
        pb_msg.set_reason_code(m.reason.code());
        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }
    if (std::holds_alternative<exit_msg>(msg)) {
        const exit_msg& m = std::get<exit_msg>(msg);
        ::hpactor::ExitMessage pb_msg;
        to_proto(pb_msg.mutable_sender(), m.sender.endpoint);
        pb_msg.set_actor_id(m.sender.id.value());
        pb_msg.set_reason_code(m.reason.code());
        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }
    if (std::holds_alternative<link_msg>(msg)) {
        const link_msg& m = std::get<link_msg>(msg);
        ::hpactor::LinkMessage pb_msg;
        to_proto(pb_msg.mutable_target(), m.target.endpoint);
        pb_msg.set_actor_id(m.target.id.value());
        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }
    if (std::holds_alternative<unlink_msg>(msg)) {
        const unlink_msg& m = std::get<unlink_msg>(msg);
        ::hpactor::UnlinkMessage pb_msg;
        to_proto(pb_msg.mutable_target(), m.target.endpoint);
        pb_msg.set_actor_id(m.target.id.value());
        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }

    return bytes{};
}
```

- [ ] **Step 4: Rewrite decode_system to use protobuf**

Replace the entire `decode_system` function body:

```cpp
MessageVariant DefaultSerializer::decode_system(TypeTag tag, const bytes& data) {
    if (data.empty()) {
        return MessageVariant{};
    }

    std::string serialized(data.begin(), data.end());

    switch (tag) {
    case TypeTag::DownMsg: {
        ::hpactor::DownMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return MessageVariant{};
        }
        down_msg m;
        m.terminated_actor.endpoint = from_proto(pb_msg.endpoint());
        m.terminated_actor.id = ActorId(pb_msg.actor_id());
        m.reason = error(pb_msg.reason_code());
        return m;
    }
    case TypeTag::ExitMsg: {
        ::hpactor::ExitMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return MessageVariant{};
        }
        exit_msg m;
        m.sender.endpoint = from_proto(pb_msg.sender());
        m.sender.id = ActorId(pb_msg.actor_id());
        m.reason = error(pb_msg.reason_code());
        return m;
    }
    case TypeTag::LinkMsg: {
        ::hpactor::LinkMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return MessageVariant{};
        }
        link_msg m;
        m.target.endpoint = from_proto(pb_msg.target());
        m.target.id = ActorId(pb_msg.actor_id());
        return m;
    }
    case TypeTag::UnlinkMsg: {
        ::hpactor::UnlinkMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return MessageVariant{};
        }
        unlink_msg m;
        m.target.endpoint = from_proto(pb_msg.target());
        m.target.id = ActorId(pb_msg.actor_id());
        return m;
    }
    default:
        return MessageVariant{};
    }
}
```

- [ ] **Step 5: Rewrite encode_spawn to use protobuf**

Replace the `encode_spawn` function. Keep the `encode_endpoint` helper for supervisor address (same as existing pattern):

```cpp
bytes DefaultSerializer::encode_spawn([[maybe_unused]] TypeTag tag, const SpawnMessageVariant& msg) {
    if (std::holds_alternative<SpawnRequest>(msg)) {
        const SpawnRequest& m = std::get<SpawnRequest>(msg);
        ::hpactor::SpawnRequestMessage pb_msg;
        pb_msg.set_actor_type_name(m.actor_type_name);
        pb_msg.set_args_type(static_cast<uint32_t>(m.args_type));
        pb_msg.set_serialized_args(m.serialized_args.data(), m.serialized_args.size());

        // supervisor as ActorRef
        ::hpactor::ActorRef* pb_sup = pb_msg.mutable_supervisor();
        to_proto(pb_sup->mutable_endpoint(), m.supervisor_addr.endpoint);
        pb_sup->set_type(m.supervisor_addr.type);
        pb_sup->set_actor_id(m.supervisor_addr.id.value());
        pb_sup->set_incarnation(m.supervisor_addr.incarnation);

        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }
    if (std::holds_alternative<SpawnResponse>(msg)) {
        const SpawnResponse& m = std::get<SpawnResponse>(msg);
        ::hpactor::SpawnResponseMessage pb_msg;

        // actor_addr as ActorAddress
        ::hpactor::ActorAddress* pb_addr = pb_msg.mutable_actor_addr();
        to_proto(pb_addr->mutable_endpoint(), m.actor_addr.endpoint);
        pb_addr->set_type(m.actor_addr.type);
        pb_addr->set_actor_id(m.actor_addr.id.value());
        pb_addr->set_incarnation(m.actor_addr.incarnation);

        pb_msg.set_error_code(m.error_code);

        std::string serialized = pb_msg.SerializeAsString();
        return bytes(serialized.begin(), serialized.end());
    }
    return bytes{};
}
```

- [ ] **Step 6: Rewrite decode_spawn to use protobuf**

Replace the `decode_spawn` function. Add `from_proto` overloads for `ActorRef` and `ActorAddress`:

First, add these helper functions alongside the existing `from_proto` overloads:

```cpp
// protobuf ActorRef → HPActor ActorAddress
static ActorAddress from_proto(const ::hpactor::ActorRef& pb_ref) {
    return ActorAddress{
        from_proto(pb_ref.endpoint()),
        static_cast<ActorType>(pb_ref.type()),
        ActorId{pb_ref.actor_id()},
        pb_ref.incarnation()
    };
}
```

Then rewrite `decode_spawn`:

```cpp
SpawnMessageVariant DefaultSerializer::decode_spawn(TypeTag tag, const bytes& data) {
    if (data.empty()) {
        return SpawnMessageVariant{};
    }

    std::string serialized(data.begin(), data.end());

    switch (tag) {
    case TypeTag::SpawnRequestTag: {
        ::hpactor::SpawnRequestMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return SpawnMessageVariant{};
        }
        SpawnRequest m;
        m.actor_type_name = pb_msg.actor_type_name();
        m.args_type = static_cast<TypeTag>(pb_msg.args_type());
        m.serialized_args.assign(pb_msg.serialized_args().begin(),
                                 pb_msg.serialized_args().end());
        m.supervisor_addr = from_proto(pb_msg.supervisor());
        return m;
    }
    case TypeTag::SpawnResponseTag: {
        ::hpactor::SpawnResponseMessage pb_msg;
        if (!pb_msg.ParseFromString(serialized)) {
            return SpawnMessageVariant{};
        }
        SpawnResponse m;
        m.actor_addr = from_proto(pb_msg.actor_addr());
        m.error_code = pb_msg.error_code();
        return m;
    }
    default:
        return SpawnMessageVariant{};
    }
}
```

- [ ] **Step 7: Remove old encode_endpoint/decode_endpoint helpers**

Now that `encode_spawn`/`decode_spawn` use protobuf helpers instead of `encode_endpoint`/`decode_endpoint`, remove the old helpers from the anonymous namespace. The old code in serialization.cpp has:

```cpp
namespace {
// Endpoint serialization (network byte order)
// Wire format: [protocol:1][addr:n][port:2]
//   protocol: 0x04 = IPv4, 0x06 = IPv6
bytes encode_endpoint(const CommunicationEndpoint& ep) { ... }
CommunicationEndpoint decode_endpoint(bytes data) { ... }
} // anonymous namespace
```

Delete both functions. Verify no other code references them before removing.

- [ ] **Step 8: Verify compilation**

Run: `ninja -C build 2>&1 | tail -30`
Expected: Compiles cleanly. Common issues:
- Missing `#include <hpactor/common.pb.h>` or `<hpactor/messages.pb.h>`
- Namespace issues with `::hpactor::*` types
- `from_proto` overload ambiguity between `ActorRef` and `ActorAddress`

- [ ] **Step 9: Run tests**

Run: `ctest --output-on-failure -j4`
Expected: All tests pass

- [ ] **Step 10: Commit**

```bash
git add src/core/serialization.cpp
git commit -m "$(cat <<'EOF'
refactor(core): replace memcpy with protobuf in DefaultSerializer

encode_system/decode_system and encode_spawn/decode_spawn now use
protobuf serialization. encode_endpoint/decode_endpoint removed.
Implements protos/hpactor/messages.proto for system message types.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Update tests for protobuf serialization

**Files:**
- Modify: `tests/core/test_serializer.cpp` (or relevant test file)

- [ ] **Step 1: Find the serializer test file**

Run: `find tests -name "*serial*" -o -name "*message*" | head -10`
Identify the file that tests `encode_system`/`decode_system`

- [ ] **Step 2: Add round-trip tests for each system message type**

Add test cases for `down_msg`, `exit_msg`, `link_msg`, `unlink_msg`, `SpawnRequest`, `SpawnResponse`. Each test:
1. Construct a message
2. `encode_system` / `decode_system` (or `encode_spawn` / `decode_spawn`)
3. Verify all fields round-trip correctly

Example:

```cpp
// Test down_msg round-trip
{
    down_msg original;
    original.terminated_actor.endpoint = Ipv4Endpoint{htonl(0x7F000001), htons(8080)};
    original.terminated_actor.id = ActorId(42);
    original.reason = error(123);

    bytes encoded = serializer.encode_system(original);
    auto decoded = serializer.decode_system(TypeTag::DownMsg, encoded);
    ASSERT_TRUE(std::holds_alternative<down_msg>(decoded));

    const auto& d = std::get<down_msg>(decoded);
    ASSERT_EQ(d.terminated_actor.id.value(), 42u);
    ASSERT_EQ(d.reason.code(), 123u);
}
```

- [ ] **Step 3: Add malformed data test**

```cpp
// Test malformed data returns default
{
    bytes malformed = {0xFF, 0xFF, 0xFF, 0xFF};
    auto result = serializer.decode_system(TypeTag::DownMsg, malformed);
    // Should return default-constructed MessageVariant, not crash
}
```

- [ ] **Step 4: Run tests**

Run: `ctest --output-on-failure -j4`
Expected: All tests pass

- [ ] **Step 5: Commit**

```bash
git add tests/  # whichever test files were modified
git commit -m "$(cat <<'EOF'
test(core): add round-trip tests for protobuf serialization

Add tests for each system message type (down_msg, exit_msg,
link_msg, unlink_msg, SpawnRequest, SpawnResponse) verifying
protobuf round-trip encoding/decoding. Add malformed data test.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Final verification

- [ ] **Step 1: Run full test suite**

Run: `ctest --output-on-failure -j4`
Expected: All tests pass (51+ tests)

- [ ] **Step 2: Check for dead code**

Run: `grep -r "encode_endpoint\|decode_endpoint" --include="*.cpp" --include="*.hpp" src/ include/`
Expected: No matches (all references removed)

- [ ] **Step 3: Final commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
refactor(core): complete MessageVariant protobuf serialization

All system messages (down_msg, exit_msg, link_msg, unlink_msg) and
spawn protocol (SpawnRequest, SpawnResponse) now use protobuf
serialization via protos/hpactor/messages.proto. Shared endpoint
types factored into protos/hpactor/common.proto imported by both
frame.proto and messages.proto.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Summary

After completing all tasks:
- `protos/hpactor/common.proto` — shared types, imported by `frame.proto` and `messages.proto`
- `protos/hpactor/messages.proto` — system message definitions
- `protos/hpactor/frame.proto` — updated to import `common.proto`
- `src/core/serialization.cpp` — fully migrated to protobuf
- `src/net/frame_protobuf.cpp` — updated for `common.proto` types
- `CMakeLists.txt` — generates all three proto files
- Tests — round-trip and malformed data coverage

**Breaking change:** Wire format changed from manual memcpy to protobuf (already accepted — no external users).
