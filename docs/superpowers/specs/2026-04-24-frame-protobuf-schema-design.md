# Frame Protobuf Schema Design Spec

> **Date:** 2026-04-24
> **Updated:** 2026-05-02 — Added magic header + length framing around protobuf payload
> **Status:** Implemented

## Goal

Replace hardcoded `memcpy`-based frame encoding/decoding in `src/net/frame.cpp` with [Protocol Buffers](https://protobuf.dev/) schema definition for type-safe, versionable network serialization, wrapped in a magic+length frame for message boundary detection.

## Motivation

### Problems with Current Hardcoded Approach

| Problem | Impact |
|---------|--------|
| No schema evolution | Adding fields breaks wire compatibility |
| Manual `memcpy` is error-prone | Buffer overflows, endianness bugs |
| No field validation | Garbage in → garbage out silently |
| Protocol versioning is invasive | Every change requires manual offset tracking |
| Opaque binary | Hard to debug, log, or inspect |
| Type tag is TODO (line 41) | Not wired up to actual payload type system |

### Why Protobuf

- **Schema evolution** — unknown fields are preserved, fields can be added/deprecated
- **Validation** — malformed data returns error, not garbage
- **Code generation** — type-safe C++ with clear interfaces
- **Cross-language** — other services can consume HPActor frames
- **Well-understood** — battle-tested, extensive tooling

## Wire Format

The complete wire format wraps a protobuf body in a magic+length frame:

```
[4 bytes: magic "HPAC" (0x48 0x50 0x41 0x43)]
[4 bytes: remaining_length (uint32_t, network byte order)]
[N bytes: protobuf-serialized ActorMsgFrame]
```

The magic header identifies the stream as HPACTOR protocol and guards against misdirected connections. The length prefix enables message boundary detection without parsing protobuf.

## Schema Design

```protobuf
// protos/hpactor/frame.proto
syntax = "proto3";

package hpactor.net;

import "hpactor/common.proto";

message ActorMsgFrame {
  hpactor.PbActorAddress sender = 1;
  hpactor.PbActorAddress receiver = 2;
  uint32 type_tag = 3;
  uint64 message_id = 4;
  uint32 flags = 5;
  bytes payload = 6;
}
```

The address types are shared in `protos/hpactor/common.proto`:

```protobuf
// protos/hpactor/common.proto
message PbIpv4Endpoint {
  fixed32 addr = 1;
  uint32 port = 2;
}

message PbIpv6Endpoint {
  bytes addr = 1;     // 16 bytes
  uint32 port = 2;
}

message PbActorEndpoint {
  oneof type {
    PbIpv4Endpoint ipv4 = 1;
    PbIpv6Endpoint ipv6 = 2;
  }
}

message PbLocalActorAddress {
  uint32 actor_type = 1;
  uint64 actor_id = 2;
  uint64 incarnation = 3;
}

message PbGlobalActorAddress {
  PbActorEndpoint endpoint = 1;
  PbLocalActorAddress local_addr = 2;
}

message PbActorAddress {
  oneof type {
    PbLocalActorAddress local_addr = 1;
    PbGlobalActorAddress global_addr = 2;
  }
}

message PbActorRef {
  oneof type {
    PbLocalActorAddress local_addr = 1;
    PbGlobalActorAddress global_addr = 2;
  }
}
```

### Design Decisions

1. **Magic header "HPAC"** — 4-byte fixed prefix (`0x48 0x50 0x41 0x43`) identifies the stream as HPACTOR protocol. Guards against misdirected connections and enables protocol detection.

2. **Length prefix outside protobuf** — A 4-byte big-endian length field precedes the protobuf payload. This enables message boundary detection without parsing protobuf, which is essential for TCP stream reassembly.

3. **`ActorMsgFrame` message name** — Named `ActorMsgFrame` to distinguish the protobuf transport frame from C++ `WireFrame` and other "frame" concepts in the codebase.

4. **WireFrame embeds `pb_frame` directly** — The C++ `WireFrame` struct holds `magic_hdr`, `length`, and `pb_frame` (the `ActorMsgFrame` protobuf message) as direct members. `encode()` serializes `pb_frame` and prepends the framing header; `decode()` strips the header and parses into `pb_frame`. This eliminates the need for separate `frame_to_proto`/`frame_from_proto` conversion functions.

5. **Shared address types in `common.proto`** — `PbActorAddress`, `PbActorRef`, `PbActorEndpoint`, `PbIpv4Endpoint`, `PbIpv6Endpoint`, `PbLocalActorAddress`, `PbGlobalActorAddress` are defined in `common.proto` and imported by `frame.proto`. This avoids duplication with `registrar.proto` and `messages.proto`.

6. **`oneof { local_addr, global_addr }` for `PbActorAddress` and `PbActorRef`** — Local actors (default loopback endpoint) use the compact `local_addr` variant with just `actor_type`, `actor_id`, `incarnation`. All other addresses use `global_addr` which adds a full `PbActorEndpoint`. This preserves endpoint fidelity for non-local addresses including IPv6 loopback.

7. **`to_proto` / `from_proto` address conversion helpers** — Public functions declared in `frame.hpp` convert between C++ `ActorAddress` and protobuf `PbActorAddress`/`PbActorRef`. They handle the `oneof` selection automatically based on whether the endpoint is the default localhost:0.

8. **`fixed32` for IPv4 addr** — `fixed32` is little-endian by default in protobuf, but we use it because the 4 bytes serialize as-is without varint encoding overhead. Endianness is handled explicitly since HPActor stores addresses in **network byte order (big-endian)**.

## Wire Compatibility

The protobuf-based wire format with magic+length framing produces **different bytes** than the original hardcoded format. This is a breaking change.

Since HPActor is not yet in production with external users, the new format was adopted directly. The magic header "HPAC" provides a clear protocol marker — any legacy peers sending the old format will fail the magic check and be rejected cleanly.

## Implementation Summary

### Phase 1: Protobuf Integration ✅

1. Added `find_package(protobuf)` to CMakeLists.txt
2. Created `protos/hpactor/frame.proto` with `ActorMsgFrame` message
3. Created `protos/hpactor/common.proto` for shared address types
4. Generated C++ headers via `protobuf_generate()` CMake function

### Phase 2: WireFrame + Address Interop ✅

1. Added `to_proto()` / `from_proto()` helpers for `ActorAddress` ↔ `PbActorAddress`/`PbActorRef` in `src/net/frame_protobuf.cpp`, declared publicly in `frame.hpp`
2. `WireFrame` struct embeds `pb_frame` (`ActorMsgFrame`) directly with `magic_hdr` and `length` members
3. `WireFrame::encode()` serializes `pb_frame` + prepends magic "HPAC" + big-endian length prefix
4. `WireFrame::decode()` validates magic, reads length, parses protobuf into `pb_frame`
5. `PbActorAddress` uses `oneof { local_addr, global_addr }` — default localhost:0 uses compact `local_addr`; all other endpoints use `global_addr` to preserve full endpoint fidelity

### Phase 3: Testing ✅

1. Updated `tests/net/test_frame.cpp` — encode/decode roundtrip, IPv4/IPv6, malformed data
2. Updated `tests/spawn/test_spawn_integration.cpp` — frame encode/decode, message_id correlation
3. Updated `tests/spawn/test_spawn_serialization.cpp` — protobuf serialize/deserialize with new oneof address types
4. Updated `tests/actor/test_unified_message_passing.cpp` — `deliver_remote` with new WireFrame API
5. Updated `tests/rpc/test_rpc_channel.cpp` — `RpcChannel::send_request` with new WireFrame API
6. Updated `tests/actor/test_proto_registry.cpp` — `PbActorRef` oneof field access

### Phase 4: Cleanup ✅

1. Removed manual `memcpy` code from frame encoding/decoding
2. Removed hardcoded offset constants
3. Frame header is now 8 bytes (magic + length) — fixed cost, protobuf body is variable-length
4. Removed duplicate address conversion helpers from `connection_pool.cpp` (now uses public `net::from_proto`)
5. All callers updated: `actor_proxy.cpp`, `actor_system.cpp`, `spawn_receiver.cpp`, `connection_pool.cpp`, `rpc_channel.cpp`

## File Changes

| File | Change |
|------|--------|
| `protos/hpactor/frame.proto` | **Create** — `ActorMsgFrame` protobuf schema |
| `protos/hpactor/common.proto` | **Create** — shared `PbActorAddress` (oneof local/global), `PbActorRef`, `PbActorEndpoint`, etc. |
| `CMakeLists.txt` | Modify — add protobuf find_package, protobuf_generate |
| `include/hpactor/net/frame.hpp` | Modify — `WireFrame` embeds `pb_frame`, `magic_hdr`, `length`; public `to_proto`/`from_proto` helpers |
| `src/net/frame.cpp` | Rewrite — `encode()` serializes `pb_frame` + prepends framing; `decode()` strips framing + parses into `pb_frame` |
| `src/net/frame_protobuf.cpp` | **Create** — `to_proto`/`from_proto` for `ActorAddress` ↔ `PbActorAddress`/`PbActorRef` |
| `src/ref/actor_proxy.cpp` | Modify — use `to_proto()` and `pb_frame` setters |
| `src/actor/actor_system.cpp` | Modify — `deliver_remote()` and `spawn_remote_async()` use new API |
| `src/actor/spawn_receiver.cpp` | Modify — response frame uses `to_proto()` and `pb_frame` |
| `src/net/connection_pool.cpp` | Modify — replace local `from_proto` helpers with public `net::from_proto` |
| `src/rpc/rpc_channel.cpp` | Modify — `send_request()` uses `to_proto()` and `pb_frame` |
| `tests/net/test_frame.cpp` | Modify — test `pb_frame.*` accessors and address conversion helpers |
| `tests/spawn/test_spawn_*.cpp` | Modify — test new oneof address types in spawn messages |
| `tests/actor/test_unified_message_passing.cpp` | Modify — `deliver_remote` test with new WireFrame API |
| `tests/actor/test_proto_registry.cpp` | Modify — `PbActorRef` oneof field access |

## Dependencies

- **protobuf** — `find_package(protobuf REQUIRED)` with fall-back to `protobuf_generate` from cmake/protobuf
- **libprotobuf-lite** — For `-fno-exceptions` build, use `protobuf-lite` instead of full protobuf

## Resolved Questions

1. **Should we keep the 4-byte length prefix outside protobuf?** → **Resolved: Yes.** The final design uses an 8-byte framing header (4-byte magic "HPAC" + 4-byte big-endian length) outside the protobuf payload. This enables message boundary detection without parsing protobuf, which is essential for TCP stream reassembly and connection multiplexing.

2. **IPv6 port handling** — `uint32 port` wastes 2 bytes per message. Accepted as a tradeoff for schema cleanliness.

3. **Schema evolution policy** — Never reuse field numbers, never change field types, only add optional fields with defaults. New fields are appended with the next available field number.

## Alternatives Considered

| Alternative | Why Not Chosen |
|-------------|----------------|
| Cap'n Proto | Less widespread tooling, different mental model |
| FlatBuffers | No schema evolution story, more complex |
| MessagePack | No schema, just typed JSON |
| Custom binary | Reinventing the wheel, same problems we're solving |
| nanopb | Embedded-focused, overkill for our use case |
