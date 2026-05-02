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

The `PbActorAddress`, `PbActorEndpoint`, `PbIpv4Endpoint`, and `PbIpv6Endpoint` types are shared in `protos/hpactor/common.proto` and reused by other protobuf messages (registrar, spawn).

### Design Decisions

1. **Magic header "HPAC"** — 4-byte fixed prefix (`0x48 0x50 0x41 0x43`) identifies the stream as HPACTOR protocol. Guards against misdirected connections and enables protocol detection.

2. **Length prefix outside protobuf** — A 4-byte big-endian length field precedes the protobuf payload. This enables message boundary detection without parsing protobuf, which is essential for TCP stream reassembly.

3. **`ActorMsgFrame` message name** — Renamed from `Frame` to `ActorMsgFrame` for clarity (distinguishes the protobuf transport frame from other "frame" concepts in the codebase).

4. **Shared address types in `common.proto`** — `PbActorAddress`, `PbActorEndpoint`, `PbIpv4Endpoint`, `PbIpv6Endpoint` are defined in `common.proto` and imported by `frame.proto`. This avoids duplication with `registrar.proto` and `messages.proto`.

5. **`fixed32` for IPv4 addr** — `fixed32` is little-endian by default in protobuf, but we use it because the 4 bytes serialize as-is without varint encoding overhead. Endianness is handled explicitly since HPActor stores addresses in **network byte order (big-endian)**.

6. **`oneof` for endpoint variant** — Forces discriminated union, prevents invalid state where both IPv4 and IPv6 are set.

## Wire Compatibility

The protobuf-based wire format with magic+length framing produces **different bytes** than the original hardcoded format. This is a breaking change.

Since HPActor is not yet in production with external users, the new format was adopted directly. The magic header "HPAC" provides a clear protocol marker — any legacy peers sending the old format will fail the magic check and be rejected cleanly.

## Implementation Summary

### Phase 1: Protobuf Integration ✅

1. Added `find_package(protobuf)` to CMakeLists.txt
2. Created `protos/hpactor/frame.proto` with `ActorMsgFrame` message
3. Created `protos/hpactor/common.proto` for shared address types
4. Generated C++ headers via `protobuf_generate()` CMake function

### Phase 2: Frame → Protobuf Interop ✅

1. Added `to_proto()` / `from_proto()` helpers for ActorAddress, EndPoint, etc. in `src/net/frame_protobuf.cpp`
2. `frame_to_proto()` converts `WireFrame` → protobuf `ActorMsgFrame` → serialized bytes (pure payload, no framing)
3. `frame_from_proto()` converts serialized bytes → protobuf `ActorMsgFrame` → `WireFrame` (pure payload, no framing)
4. `WireFrame::encode()` adds magic "HPAC" + big-endian length prefix + protobuf payload
5. `WireFrame::decode()` validates magic, reads length, extracts protobuf payload, deserializes

### Phase 3: Testing ✅

1. Updated `tests/net/test_frame.cpp` — encode/decode roundtrip, IPv4/IPv6, malformed data
2. Protobuf parsing handles malformed data safely (returns default `WireFrame` on failure)
3. Large payloads handled by protobuf's variable-length encoding

### Phase 4: Cleanup ✅

1. Removed manual `memcpy` code from frame encoding/decoding
2. Removed hardcoded offset constants
3. Frame header is now 8 bytes (magic + length) — fixed cost, protobuf body is variable-length

## File Changes

| File | Change |
|------|--------|
| `protos/hpactor/frame.proto` | **Create** — `ActorMsgFrame` protobuf schema |
| `protos/hpactor/common.proto` | **Create** — shared `PbActorAddress`, `PbActorEndpoint`, etc. |
| `CMakeLists.txt` | Modify — add protobuf find_package, protobuf_generate |
| `src/net/frame.cpp` | Rewrite — `encode()` adds magic+length framing; `decode()` validates and strips it |
| `src/net/frame_protobuf.cpp` | **Create** — `frame_to_proto()` / `frame_from_proto()` conversion helpers |
| `include/hpactor/net/frame.hpp` | Modify — add `MagicHeader`, `HeaderSize` constants; update wire format docs |
| `tests/net/test_frame.cpp` | Modify — update to test protobuf serialization with magic+length framing |

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
