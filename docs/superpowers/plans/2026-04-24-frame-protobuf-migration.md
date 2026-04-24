# Frame Protobuf Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace hardcoded memcpy-based frame encoding/decoding with Protocol Buffers schema definition for type-safe, versionable network serialization.

**Architecture:** Add protobuf as a build dependency, define `frame.proto` schema, generate C++ headers, and refactor `Frame::encode()`/`decode()` to use protobuf serialization while keeping the same public interface.

**Tech Stack:** Protocol Buffers (protobuf), CMake `find_package(protobuf)`, C++20

---

## Task 1: Add protobuf to CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt:152-156`
- Create: `cmake/FindProtobuf.cmake` (if needed for fallback)

- [ ] **Step 1: Add protobuf find_package**

After `find_package(OpenSSL REQUIRED)` on line 154, add:

```cmake
# protobuf for frame serialization
find_package(protobuf REQUIRED)
target_link_libraries(hpactor_lib PUBLIC protobuf::libprotobuf)
```

- [ ] **Step 2: Verify cmake runs**

Run: `cmake -S . -B build -GNinja 2>&1 | head -50`
Expected: Protobuf found, no FATAL_ERROR

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add protobuf dependency for frame serialization"
```

---

## Task 2: Create frame.proto schema

**Files:**
- Create: `protos/hpactor/frame.proto`

- [ ] **Step 1: Create protos directory**

Run: `mkdir -p protos/hpactor`

- [ ] **Step 2: Write frame.proto**

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
  uint32 type_tag = 6;
}

message ActorAddress {
  Endpoint endpoint = 1;
  uint32 type = 2;
  uint64 actor_id = 3;
  uint64 incarnation = 4;
}

message Endpoint {
  oneof type {
    Ipv4Endpoint ipv4 = 1;
    Ipv6Endpoint ipv6 = 2;
  }
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

- [ ] **Step 3: Add cmake/protobuf.cmake for code generation**

Create `cmake/protobuf.cmake`:

```cmake
# CMake module to run protoc for protobuf code generation
include(ExternalProject)

function(PROTOBUF_GENERATE_CPP SRCS HDRS)
  if(NOT ARGN)
    message(SEND_ERROR "PROTOBUF_GENERATE_CPP called with no input files")
    return()
  endif()

  if(protobuf_FOUND)
    get_target_property(PROTOC_IMPORT_PATH protobuf::protoc IMPORTED_LOCATION_RELEASE)
  endif()

  foreach(FIL ${ARGN})
    get_filename_component(FIL_abs ${FIL} ABSOLUTE)
    get_filename_component(FIL_base ${FIL} NAME_WE)

    set(${SRCS} ${${SRCS}} ${CMAKE_CURRENT_BINARY_DIR}/${FIL_base}.pb.cc PARENT_SCOPE)
    set(${HDRS} ${HDRS} ${CMAKE_CURRENT_BINARY_DIR}/${FIL_base}.pb.h PARENT_SCOPE)

    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${FIL_base}.pb.cc
             ${CMAKE_CURRENT_BINARY_DIR}/${FIL_base}.pb.h
      COMMAND ${PROTOC} --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
              -I${CMAKE_CURRENT_SOURCE_DIR}/protos
              ${FIL_abs}
      DEPENDS ${FIL} protobuf::protoc
      COMMENT "Generating ${FIL_base}.pb.[cc,h] from ${FIL}"
      VERBATIM
    )
  endforeach()
endfunction()
```

- [ ] **Step 4: Update CMakeLists.txt to generate protobuf**

After `target_include_directories(hpactor_lib PUBLIC include)` on line 152, add:

```cmake
# Generate protobuf C++ headers/sources
include(cmake/protobuf)
PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS ${CMAKE_SOURCE_DIR}/protos/hpactor/frame.proto)
add_library(hpactor_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
target_link_libraries(hpactor_proto PUBLIC protobuf::libprotobuf)
target_include_directories(hpactor_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
add_dependencies(hpactor_proto hpactor_lib)
```

- [ ] **Step 5: Test protobuf generation**

Run: `cmake -S . -B build -GNinja && ninja -C build 2>&1 | grep -E "(frame|proto)"`
Expected: frame.pb.cc and frame.pb.h generated

- [ ] **Step 6: Commit**

```bash
git add protos/hpactor/frame.proto cmake/protobuf.cmake CMakeLists.txt
git commit -m "build: add protobuf schema and code generation for frame serialization"
```

---

## Task 3: Add protobuf interop helpers

**Files:**
- Create: `src/net/frame_protobuf.cpp` (or add to `frame.cpp`)
- Modify: `include/hpactor/net/frame.hpp`

- [ ] **Step 1: Add protobuf conversion declarations to frame.hpp**

After the Frame struct (around line 75), add:

```cpp
// Protobuf interop
namespace pb {
class Frame;
class ActorAddress;
class Endpoint;
} // namespace pb

// Convert between HPActor types and protobuf
bytes frame_to_proto(const Frame& frame);
Frame frame_from_proto(const bytes& data);
```

- [ ] **Step 2: Implement protobuf conversion functions**

Create `src/net/frame_protobuf.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
#include <hpactor/net/frame.hpp>
#include <hpactor/net/frame.pb.h>  // Generated protobuf header

namespace hpactor::net {

// Helper: convert Ipv4Endpoint to protobuf
static void to_proto(Ipv4Endpoint* pb_ep, const hpactor::Ipv4Endpoint& ep) {
    pb_ep->set_addr(ep.addr);  // Network byte order
    pb_ep->set_port(ep.port_nw);
}

// Helper: convert Ipv6Endpoint to protobuf
static void to_proto(Ipv6Endpoint* pb_ep, const hpactor::Ipv6Endpoint& ep) {
    pb_ep->set_addr(ep.addr.data(), 16);
    pb_ep->set_port(ep.port_nw);
}

// Helper: convert CommunicationEndpoint to protobuf Endpoint
static void to_proto(pb::Endpoint* pb_endpoint, const CommunicationEndpoint& ep) {
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&ep)) {
        to_proto(pb_endpoint->mutable_ipv4(), *ipv4);
    } else if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&ep)) {
        to_proto(pb_endpoint->mutable_ipv6(), *ipv6);
    }
}

// Helper: convert ActorAddress to protobuf
static void to_proto(pb::ActorAddress* pb_addr, const ActorAddress& addr) {
    to_proto(pb_addr->mutable_endpoint(), addr.endpoint);
    pb_addr->set_type(addr.type);
    pb_addr->set_actor_id(addr.id.value());
    pb_addr->set_incarnation(addr.incarnation);
}

bytes frame_to_proto(const Frame& frame) {
    pb::Frame pb_frame;
    to_proto(pb_frame.mutable_sender(), frame.sender);
    to_proto(pb_frame.mutable_receiver(), frame.receiver);
    pb_frame.set_message_id(frame.message_id);
    pb_frame.set_flags(frame.flags);
    pb_frame.set_payload(frame.payload.data(), frame.payload.size());
    pb_frame.set_type_tag(frame.type_tag);

    std::string serialized = pb_frame.SerializeAsString();
    return bytes(serialized.begin(), serialized.end());
}

// Helper: convert protobuf Ipv4Endpoint to HPActor
static Ipv4Endpoint from_proto(const pb::Ipv4Endpoint& pb_ep) {
    return Ipv4Endpoint{pb_ep.addr(), static_cast<uint16_t>(pb_ep.port())};
}

// Helper: convert protobuf Ipv6Endpoint to HPActor
static Ipv6Endpoint from_proto(const pb::Ipv6Endpoint& pb_ep) {
    std::array<uint8_t, 16> addr;
    std::memcpy(addr.data(), pb_ep.addr().data(), 16);
    return Ipv6Endpoint{addr, static_cast<uint16_t>(pb_ep.port())};
}

// Helper: convert protobuf Endpoint to HPActor CommunicationEndpoint
static CommunicationEndpoint from_proto(const pb::Endpoint& pb_endpoint) {
    if (pb_endpoint.has_ipv4()) {
        return from_proto(pb_endpoint.ipv4());
    } else if (pb_endpoint.has_ipv6()) {
        return from_proto(pb_endpoint.ipv6());
    }
    return Ipv4Endpoint{};
}

// Helper: convert protobuf ActorAddress to HPActor
static ActorAddress from_proto(const pb::ActorAddress& pb_addr) {
    return ActorAddress{
        from_proto(pb_addr.endpoint()),
        static_cast<ActorType>(pb_addr.type()),
        ActorId{pb_addr.actor_id()},
        pb_addr.incarnation()
    };
}

Frame frame_from_proto(const bytes& data) {
    pb::Frame pb_frame;
    std::string serialized(data.begin(), data.end());
    if (!pb_frame.ParseFromString(serialized)) {
        return Frame{};  // Return default frame on parse failure
    }

    Frame frame;
    frame.sender = from_proto(pb_frame.sender());
    frame.receiver = from_proto(pb_frame.receiver());
    frame.message_id = pb_frame.message_id();
    frame.flags = pb_frame.flags();
    frame.payload.assign(pb_frame.payload().begin(), pb_frame.payload().end());
    frame.type_tag = pb_frame.type_tag();
    return frame;
}

} // namespace hpactor::net
```

- [ ] **Step 3: Commit**

```bash
git add src/net/frame_protobuf.cpp include/hpactor/net/frame.hpp
git commit -m "feat(net): add protobuf interop helpers for Frame serialization"
```

---

## Task 4: Update Frame::encode() and decode() to use protobuf

**Files:**
- Modify: `src/net/frame.cpp:35-171`

- [ ] **Step 1: Update Frame::encode()**

Replace the entire `encode()` method (lines 35-106) with:

```cpp
bytes Frame::encode() const {
    return frame_to_proto(*this);
}
```

- [ ] **Step 2: Update Frame::decode()**

Replace `Frame::decode()` (lines 108-171) with:

```cpp
Frame Frame::decode(const bytes& data) {
    return frame_from_proto(data);
}
```

- [ ] **Step 3: Remove obsolete helper functions**

Delete:
- `calculate_header_size()` (lines 23-33)
- `encode_endpoint()` (lines 176-193)
- `decode_endpoint()` (lines 195-222)

These are no longer needed since protobuf handles serialization.

- [ ] **Step 4: Update frame.hpp**

Remove hardcoded constants (lines 80-84):
- `PayloadLengthOffset`
- `TypeTagOffset`
- `FlagsOffset`
- `MessageIdOffset`
- `FixedHeaderSize`

Remove `encode_endpoint()` and `decode_endpoint()` declarations (lines 60-66).

- [ ] **Step 5: Test compilation**

Run: `ninja -C build 2>&1 | head -30`
Expected: Compiles with warnings about unused includes (we'll fix these)

- [ ] **Step 6: Fix compilation errors**

Common issues:
- Missing include for generated protobuf header
- Namespace issues (pb:: vs hpactor::net::)

- [ ] **Step 7: Run existing tests**

Run: `./build/tests/net/test_frame`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add src/net/frame.cpp include/hpactor/net/frame.hpp
git commit -m "refactor(net): replace memcpy frame encoding with protobuf serialization"
```

---

## Task 5: Update tests for protobuf serialization

**Files:**
- Modify: `tests/net/test_frame.cpp`

- [ ] **Step 1: Add malformed data test**

Add to `main()` after existing tests:

```cpp
// Test malformed data handling
bytes malformed = {0xFF, 0xFF, 0xFF, 0xFF};  // Invalid protobuf
Frame f_bad = Frame::decode(malformed);
// Should return default frame, not crash
assert(f_bad.sender.id.value() == 0);
```

- [ ] **Step 2: Add IPv6 endpoint test**

Add test case with `Ipv6Endpoint`:

```cpp
// Test IPv6 endpoint
std::array<uint8_t, 16> ipv6_addr = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
ActorAddress ipv6_sender{Ipv6Endpoint{ipv6_addr, htons(8080)}, 1, ActorId(300), 1};
ActorAddress ipv6_receiver{Ipv6Endpoint{ipv6_addr, htons(9090)}, 2, ActorId(400), 2};

Frame f_ipv6;
f_ipv6.sender = ipv6_sender;
f_ipv6.receiver = ipv6_receiver;
f_ipv6.payload = {1, 2, 3};
f_ipv6.message_id = 99999;

bytes encoded_ipv6 = f_ipv6.encode();
Frame decoded_ipv6 = Frame::decode(encoded_ipv6);

assert(std::get<Ipv6Endpoint>(decoded_ipv6.sender.endpoint).port_nw == htons(8080));
assert(decoded_ipv6.message_id == 99999);
```

- [ ] **Step 3: Run tests**

Run: `./build/tests/net/test_frame`
Expected: All assertions pass

- [ ] **Step 4: Commit**

```bash
git add tests/net/test_frame.cpp
git commit -m "test(net): add IPv6 and malformed data tests for protobuf frame"
```

---

## Task 6: Final verification and cleanup

- [ ] **Step 1: Run all tests**

Run: `ctest --output-on-failure`
Expected: All 50+ tests pass

- [ ] **Step 2: Check for dead code**

Run: `grep -r "encode_endpoint\|decode_endpoint\|FixedHeaderSize\|PayloadLengthOffset" --include="*.cpp" --include="*.hpp" src/ include/`
Expected: No matches (all references removed)

- [ ] **Step 3: Clean up unused includes in frame.cpp**

Remove any includes no longer needed after protobuf migration.

- [ ] **Step 4: Final commit**

```bash
git add -A
git commit -m "refactor(net): complete protobuf migration for frame serialization"
```

---

## Summary

After completing all tasks:
- `protos/hpactor/frame.proto` — Protobuf schema definition
- `src/net/frame.cpp` — Uses protobuf for encode/decode
- `src/net/frame_protobuf.cpp` — Conversion helpers
- `include/hpactor/net/frame.hpp` — Clean public interface
- `tests/net/test_frame.cpp` — Updated with IPv6 and malformed data tests
- CMake integration for `find_package(protobuf)` and code generation

**Breaking change:** Wire format is different from previous version. Deploy requires coordinated update.
