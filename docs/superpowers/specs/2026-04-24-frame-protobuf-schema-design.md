# Frame Protobuf Schema Design Spec

> **Date:** 2026-04-24
> **Status:** Draft

## Goal

Replace hardcoded `memcpy`-based frame encoding/decoding in `src/net/frame.cpp` with [Protocol Buffers](https://protobuf.dev/) schema definition for type-safe, versionable network serialization.

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

## Schema Design

```protobuf
// protos/hpactor/frame.proto
syntax = "proto3";

package hpactor.net;

message Frame {
  ActorAddress sender = 1;
  ActorAddress receiver = 2;
  uint64 message_id = 3;
  uint32 flags = 4;
  bytes payload = 5;
  uint32 type_tag = 6;  // For dispatch routing
}

message ActorAddress {
  Endpoint endpoint = 1;
  uint32 type = 2;           // ActorType
  uint64 actor_id = 3;       // ActorId.value()
  uint64 incarnation = 4;
}

message Endpoint {
  oneof type {
    Ipv4Endpoint ipv4 = 1;
    Ipv6Endpoint ipv6 = 2;
  }
}

message Ipv4Endpoint {
  fixed32 addr = 1;   // Network byte order (4 bytes)
  uint32 port = 2;    // Network byte order (2 bytes packed into 4)
}

message Ipv6Endpoint {
  bytes addr = 1;     // 16 bytes, network byte order
  uint32 port = 2;    // Network byte order
}
```

### Design Decisions

1. **`fixed32` for IPv4 addr** — `fixed32` is little-endian by default in protobuf, but we use it because the 4 bytes serialize as-is without varint encoding overhead. We must handle endianness explicitly since HPActor stores addresses in **network byte order (big-endian)**.

2. **`uint32` for port in Ipv4Endpoint** — Port is 2 bytes but `uint32` is used for alignment. Protobuf packing handles it efficiently.

3. **`ActorAddress.endpoint` is `Endpoint` message (not raw bytes)** — Allows endpoint inspection/debugging without deserializing the full frame.

4. **`oneof` for endpoint variant** — Forces discriminated union, prevents invalid state where both IPv4 and IPv6 are set.

## Wire Compatibility

The new protobuf serialization produces **different bytes** than the current hardcoded format. This is a breaking change requiring a version bump.

To maintain backward compatibility during migration:

| Strategy | Pros | Cons |
|----------|------|------|
| Version flag in Frame.flags | Single format, versioned | Complex parsing logic |
| **New wire format (chosen)** | Clean, forward-only | Requires coordinated deploy |

Since HPActor is not yet in production with external users, we can adopt the new format directly with a version marker in `Frame.flags`.

## Implementation Plan

### Phase 1: Protobuf Integration

1. Add `find_package(protobuf)` to CMakeLists.txt (with error if not found)
2. Create `protos/hpactor/frame.proto`
3. Generate C++ headers via `protobuf_generate()` CMake function
4. Add generated files to gitignore

### Phase 2: Frame → Protobuf Interop

1. Add `to_proto(const ActorAddress&)` / `from_proto(const ActorAddress&)` helpers
2. Add `to_proto(const Frame&)` / `from_proto(const Frame&)` helpers
3. Keep `encode_endpoint` / `decode_endpoint` for internal use (they're used elsewhere)
4. Update `Frame::encode()` to use protobuf serialization
5. Update `Frame::decode()` to use protobuf parsing
6. Update `calculate_header_size()` to return protobuf estimate

### Phase 3: Testing

1. Update `tests/net/test_frame.cpp` — existing tests should pass (same semantics)
2. Add test for malformed data (protobuf validation catches it)
3. Add test for large payloads (> 64KB)
4. Add test for IPv6 endpoints

### Phase 4: Cleanup

1. Remove manual `memcpy` code from `frame.cpp`
2. Remove hardcoded offset constants (`PayloadLengthOffset`, etc.)
3. Remove `FixedHeaderSize` — protobuf doesn't have fixed headers

## File Changes

| File | Change |
|------|--------|
| `protos/hpactor/frame.proto` | **Create** — protobuf schema |
| `CMakeLists.txt` | Modify — add protobuf find_package, protobuf_generate |
| `src/net/frame.cpp` | Rewrite — use protobuf instead of memcpy |
| `include/hpactor/net/frame.hpp` | Modify — keep interface, remove hardcoded constants |
| `tests/net/test_frame.cpp` | Modify — update to test protobuf serialization |

## Dependencies

- **protobuf** — `find_package(protobuf REQUIRED)` with fall-back to `protobuf_generate` from cmake/protobuf
- **libprotobuf-lite** — For `-fno-exceptions` build, use `protobuf-lite` instead of full protobuf

## Open Questions

1. **Should we keep the 4-byte length prefix outside protobuf?** — Allows framing without parsing protobuf (useful for connection multiplexing). Protobuf delimited messages (`ParseDelimitedFrom()`) encode their own length prefix.

2. **IPv6 port handling** — `uint32 port` wastes 2 bytes per message. Acceptable tradeoff for schema cleanliness.

3. **Schema evolution policy** — Document: never reuse field numbers, never change field types, only add optional fields with defaults.

## Alternatives Considered

| Alternative | Why Not Chosen |
|-------------|----------------|
| Cap'n Proto | Less widespread tooling, different mental model |
| FlatBuffers | No schema evolution story, more complex |
| MessagePack | No schema, just typed JSON |
| Custom binary | Reinventing the wheel, same problems we're solving |
| nanopb | Embedded-focused, overkill for our use case |
