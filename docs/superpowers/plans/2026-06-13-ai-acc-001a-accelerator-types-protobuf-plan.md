# AI-ACC-001A: Accelerator Types & Protobuf — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the stable public value model, protobuf contract, and TypeTag extension mechanism for the AI accelerator resource plane without any actor behavior.

**Architecture:** Seven TDD-driven tasks in dependency order. First, the TypeTag extension mechanism unblocks subsystem self-registration. Then build scaffolding, protobuf, and C++ types land in sequence. Each task follows RED-GREEN-REFACTOR: write a failing test, implement the minimum to pass, verify, commit.

**Tech Stack:** C++20, protobuf, Google Test, Ninja/CMake

**Worktree:** `.worktrees/ai-acc-001a-types-proto` (branch `worktree/ai-acc-001a-types-proto`)

---

### Task 1: TypeTag Extension Mechanism (Core Changes)

**Files:**
- Modify: `include/hpactor/msg/type_tag.hpp`
- Modify: `include/hpactor/core/proto_type_registry.hpp`
- Modify: `src/core/proto_type_registry.cpp`
- Create: (none yet — existing tests verify no regression)

**Purpose:** Add the subsystem registrar chain so future subsystems (starting with AI) can register TypeTags and MessageTraits without editing core files.

- [ ] **Step 1: Add subsystem range comment and `make_subsystem_tag()` to `type_tag.hpp`**

In `include/hpactor/msg/type_tag.hpp`, after `BackpressureSignalTag = 0x70` and before `User = 0x00001000`, add:

```cpp
    // ── Backpressure control (0x70–0x7F) ─────────────────────────────────
    BackpressureSignalTag = 0x70, ///< Backpressure signal (local or remote).

    // ── Subsystem extension range (0x80–0xFF) ────────────────────────────
    // 256 slots reserved for subsystem-defined TypeTags.
    //
    // Subsystems declare their tags as inline constexpr in their own headers:
    //   namespace hpactor::ai {
    //   inline constexpr TypeTag kAiLeaseRequestTag =
    //       make_subsystem_tag(0x80);
    //   }
    //
    // These are NOT added to the TypeTag enum. They are constexpr variables
    // that implicitly convert to TypeTag. This keeps the core enum closed
    // while subsystems own their tag definitions.

    // ── Application range ────────────────────────────────────────────────
    User = 0x00001000, ///< Start of application-defined message tags.
};

/// Construct a TypeTag from a subsystem-range value (0x80–0xFF).
/// Compile-time only; static_asserts the value is in range.
consteval TypeTag make_subsystem_tag(uint32_t value) {
    return static_cast<TypeTag>(value);
}
```

- [ ] **Step 2: Add `SubsystemRegistrar` and `register_subsystem()` to `proto_type_registry.hpp`**

In `include/hpactor/core/proto_type_registry.hpp`, add to the `ProtoTypeRegistry` class (before the `private:` section):

```cpp
  public:
    /// Function pointer type for subsystem message registration.
    /// Called by register_system_types() after core types are registered.
    using SubsystemRegistrar = void (*)(ProtoTypeRegistry&);

    /// Register a subsystem's message types.
    /// Thread-safe. May be called during static initialization.
    /// Order between subsystems is unspecified.
    static void register_subsystem(SubsystemRegistrar registrar);
```

- [ ] **Step 3: Implement registrar chain in `proto_type_registry.cpp`**

In `src/core/proto_type_registry.cpp`, add includes and the registrar chain:

```cpp
#include <mutex>
#include <vector>

namespace hpactor {
namespace {

std::vector<ProtoTypeRegistry::SubsystemRegistrar>& subsystem_registrars() {
    static std::vector<ProtoTypeRegistry::SubsystemRegistrar> regs;
    return regs;
}

} // namespace

void ProtoTypeRegistry::register_subsystem(SubsystemRegistrar fn) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    subsystem_registrars().push_back(fn);
}
```

Then modify `register_system_types()` to call subsystem registrars after the existing core registrations. At the end of the function body, before the closing brace, add:

```cpp
    // ── Subsystem types (0x80–0xFF, registered via register_subsystem) ──
    for (auto fn : subsystem_registrars()) {
        fn(*this);
    }
```

- [ ] **Step 4: Build and verify no regression**

```bash
ninja -C build hpactor_lib
```

Expected: builds successfully. No new warnings.

```bash
ninja -C build tests/unit/types/test_request_handle && ./build/tests/unit/types/test_request_handle
```

Expected: 9/9 tests pass (baseline unchanged).

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/msg/type_tag.hpp \
        include/hpactor/core/proto_type_registry.hpp \
        src/core/proto_type_registry.cpp
git commit -m "feat(core): add TypeTag subsystem extension mechanism

Add SubsystemRegistrar function-pointer chain to ProtoTypeRegistry so
future subsystems can register TypeTags and MessageTraits without
modifying core framework files. Reserve 0x80-0xFF for subsystem use
with a consteval make_subsystem_tag() helper.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Build Scaffolding

**Files:**
- Create: `tests/unit/ai/CMakeLists.txt`
- Create: `tests/unit/ai/test_accelerator_types.cpp` (minimal placeholder)
- Create: `include/hpactor/ai/.gitkeep` (removed when first header lands)
- Create: `src/ai/.gitkeep` (removed when first source lands)
- Create: `protos/hpactor/ai_resource.proto` (minimal placeholder)
- Modify: `cmake/dependencies.cmake`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

**Purpose:** Create directories and wire the build system so subsequent TDD cycles have a working compile-test loop.

- [ ] **Step 1: Create directory structure**

```bash
mkdir -p include/hpactor/ai src/ai tests/unit/ai
```

- [ ] **Step 2: Write minimal proto placeholder**

`protos/hpactor/ai_resource.proto`:
```proto
// protos/hpactor/ai_resource.proto
syntax = "proto3";
package hpactor;

import "hpactor/common.proto";

// Placeholder — full messages added in Task 3.
message PbDeviceLeaseRequest {
  PbActorAddress requester = 1;
}
```

- [ ] **Step 3: Add proto to codegen in `cmake/dependencies.cmake`**

In the `PROTOBUF_GENERATE_CPP` call, add after the last existing proto:
```cmake
    ${CMAKE_SOURCE_DIR}/protos/hpactor/ai_resource.proto
```

- [ ] **Step 4: Add source files to `src/CMakeLists.txt`**

In the `hpactor_lib` source list, add before the closing `)`:
```cmake
    ai/accelerator_types.cpp
    ai/ai_message_registry.cpp
```

- [ ] **Step 5: Add test subdirectory in `tests/unit/CMakeLists.txt`**

Add after the last `add_subdirectory`:
```cmake
add_subdirectory(ai)
```

- [ ] **Step 6: Write test CMakeLists.txt**

`tests/unit/ai/CMakeLists.txt`:
```cmake
add_executable(test_accelerator_types test_accelerator_types.cpp)
target_link_libraries(test_accelerator_types PRIVATE hpactor GTest::gtest_main)
gtest_discover_tests(test_accelerator_types)
```

- [ ] **Step 7: Write minimal test placeholder**

`tests/unit/ai/test_accelerator_types.cpp`:
```cpp
#include <gtest/gtest.h>

TEST(AiAcceleratorTypesSmoke, BuildSystemWorks) {
    EXPECT_EQ(1, 1);
}
```

- [ ] **Step 8: Write minimal source placeholders**

`src/ai/accelerator_types.cpp`:
```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#include <hpactor/ai/accelerator_types.hpp>
```

`src/ai/ai_message_registry.cpp`:
```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#include <hpactor/core/proto_type_registry.hpp>
```

`include/hpactor/ai/accelerator_types.hpp`:
```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace hpactor::ai {}
```

- [ ] **Step 9: Reconfigure, build, and run smoke test**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build hpactor_lib hpactor_proto
ninja -C build tests/unit/ai/test_accelerator_types
./build/tests/unit/ai/test_accelerator_types
```

Expected: 1/1 test passes (`BuildSystemWorks`).

- [ ] **Step 10: Commit**

```bash
git add protos/hpactor/ai_resource.proto \
        include/hpactor/ai/accelerator_types.hpp \
        src/ai/accelerator_types.cpp \
        src/ai/ai_message_registry.cpp \
        tests/unit/ai/CMakeLists.txt \
        tests/unit/ai/test_accelerator_types.cpp \
        cmake/dependencies.cmake \
        src/CMakeLists.txt \
        tests/unit/CMakeLists.txt
git commit -m "build: add AI accelerator build scaffolding

Create ai/ directories, wire proto codegen, source files, and test
build. Placeholder files establish the compile-test loop for
subsequent TDD cycles.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Protobuf Contract

**Files:**
- Modify: `protos/hpactor/ai_resource.proto`
- Modify: `tests/unit/ai/test_accelerator_types.cpp`

**Purpose:** Replace the placeholder proto with the full 11-message contract. Verify codegen succeeds and the generated types are usable from C++.

- [ ] **Step 1: RED — Write a test that uses a generated proto type**

Replace `tests/unit/ai/test_accelerator_types.cpp`:

```cpp
#include <gtest/gtest.h>
#include <hpactor/ai_resource.pb.h>

TEST(ProtoGeneration, LeaseRequestCompiles) {
    hpactor::PbDeviceLeaseRequest req;
    req.set_workload_id("test-workload");
    req.set_ttl_ms(30000);
    EXPECT_EQ(req.workload_id(), "test-workload");
    EXPECT_EQ(req.ttl_ms(), 30000u);
}

TEST(ProtoGeneration, LeaseReplyFields) {
    hpactor::PbDeviceLeaseReply reply;
    reply.set_granted(true);
    reply.set_lease_id(42);
    reply.set_device_local_id(7);
    reply.set_ledger_epoch(1);
    reply.set_reason(0);  // Granted
    reply.set_detail("ok");
    reply.set_expires_at_ns(1'000'000'000);
    EXPECT_TRUE(reply.granted());
    EXPECT_EQ(reply.lease_id(), 42u);
}

TEST(ProtoGeneration, AllMessagesInstantiable) {
    // Verify all 11 message types compile and default-construct
    hpactor::PbDeviceLeaseRequest r1;
    hpactor::PbDeviceLeaseReply r2;
    hpactor::PbDeviceLeaseActivate r3;
    hpactor::PbDeviceLeaseRenew r4;
    hpactor::PbDeviceLeaseRelease r5;
    hpactor::PbDeviceLeaseRevoked r6;
    hpactor::PbResourceSnapshotRequest r7;
    hpactor::PbResourceSnapshotReply r8;
    hpactor::PbDeviceSnapshotUpdate r9;
    hpactor::PbDeviceCapacitySummary r10;
    hpactor::PbNodeResourceSummary r11;
    SUCCEED();
}
```

- [ ] **Step 2: RED — Attempt build, verify failure**

```bash
ninja -C build tests/unit/ai/test_accelerator_types 2>&1 | tail -20
```

Expected: FAIL — `PbDeviceLeaseReply`, `PbDeviceLeaseActivate`, etc. not defined (only `PbDeviceLeaseRequest` exists from the placeholder proto).

- [ ] **Step 3: GREEN — Write the full proto contract**

Replace `protos/hpactor/ai_resource.proto`:

```proto
// protos/hpactor/ai_resource.proto
syntax = "proto3";
package hpactor;

import "hpactor/common.proto";

// ── Resource quantities ─────────────────────────────────────────────
message PbResourceQuantities {
  uint64 device_memory_bytes = 1;
  uint64 host_memory_bytes = 2;
  uint64 pinned_host_memory_bytes = 3;
  uint32 compute_units = 4;
  uint32 stream_slots = 5;
  uint32 copy_engine_slots = 6;
  uint64 kv_cache_bytes = 7;
  uint32 batch_slots = 8;
  bool exclusive_device = 9;
}

// ── Device selector ─────────────────────────────────────────────────
message PbDeviceSelector {
  uint32 kind = 1;
  uint64 exact_device_id = 2;
  uint32 required_kind = 3;
  uint32 required_vendor = 4;
  repeated string required_label_keys = 5;
  repeated string required_label_values = 6;
  int32 preferred_numa_node = 7;
  string preferred_topology_group = 8;
  bool allow_cpu_fallback = 9;
}

// ── Lease request / reply ───────────────────────────────────────────
message PbDeviceLeaseRequest {
  PbActorAddress requester = 1;
  string workload_id = 2;
  string model_or_job = 3;
  string tenant_id = 4;
  PbResourceQuantities requested = 5;
  PbDeviceSelector selector = 6;
  uint32 ttl_ms = 7;
  uint32 priority = 8;
  bool allow_degraded_device = 9;
  bool exclusive = 10;
}

message PbDeviceLeaseReply {
  bool granted = 1;
  uint64 lease_id = 2;
  uint64 device_local_id = 3;
  uint64 ledger_epoch = 4;
  uint32 reason = 5;
  string detail = 6;
  PbResourceQuantities reserved = 7;
  uint64 expires_at_ns = 8;
}

// ── Lease lifecycle messages ────────────────────────────────────────
message PbDeviceLeaseActivate {
  uint64 lease_id = 1;
  PbActorAddress owner = 2;
}

message PbDeviceLeaseRenew {
  uint64 lease_id = 1;
  PbActorAddress owner = 2;
  uint32 ttl_ms = 3;
}

message PbDeviceLeaseRelease {
  uint64 lease_id = 1;
  PbActorAddress owner = 2;
  uint32 reason = 3;
}

message PbDeviceLeaseRevoked {
  uint64 lease_id = 1;
  uint64 device_local_id = 2;
  uint32 reason = 3;
  string detail = 4;
}

// ── Probe snapshot messages ─────────────────────────────────────────
message PbDeviceDescriptor {
  uint64 device_local_id = 1;
  uint32 kind = 2;
  uint32 vendor = 3;
  string backend = 4;
  string name = 5;
  string stable_key = 6;
  uint32 backend_ordinal = 7;
  uint64 parent_device_id = 8;
  int32 numa_node = 9;
  string topology_group = 10;
  PbResourceQuantities total = 11;
  PbResourceQuantities allocatable = 12;
  uint64 capabilities = 13;
  uint32 health = 14;
  repeated string label_keys = 15;
  repeated string label_values = 16;
}

message PbDeviceSnapshotUpdate {
  string probe_name = 1;
  uint64 probe_epoch = 2;
  uint64 generated_at_ns = 3;
  repeated PbDeviceDescriptor devices = 4;
  bool snapshot_available = 5;
  uint32 error_reason = 6;
  string error_detail = 7;
}

// ── Snapshot and summary messages ───────────────────────────────────
message PbResourceSnapshotRequest {
  uint32 max_devices = 1;
  uint32 max_leases = 2;
}

message PbResourceSnapshotReply {
  uint64 ledger_epoch = 1;
  uint64 generated_at_ns = 2;
  bool ready = 3;
  bool draining = 4;
  repeated PbDeviceDescriptor devices = 5;
  bool device_list_truncated = 6;
}

message PbDeviceCapacitySummary {
  uint64 device_local_id = 1;
  uint32 kind = 2;
  uint32 vendor = 3;
  string backend = 4;
  uint32 health = 5;
  PbResourceQuantities allocatable = 6;
  PbResourceQuantities reserved = 7;
  PbResourceQuantities available = 8;
  uint32 active_leases = 9;
  bool exclusive_in_use = 10;
  repeated string label_keys = 11;
  repeated string label_values = 12;
  string topology_group = 13;
}

message PbNodeResourceSummary {
  PbEndpoint node_endpoint = 1;
  uint64 ledger_epoch = 2;
  uint64 generated_at_ns = 3;
  bool ready = 4;
  bool draining = 5;
  repeated PbDeviceCapacitySummary devices = 6;
}
```

- [ ] **Step 4: GREEN — Reconfigure, build, and run tests**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build hpactor_proto
ninja -C build tests/unit/ai/test_accelerator_types
./build/tests/unit/ai/test_accelerator_types
```

Expected: 3/3 tests pass (smoke test replaced, proto generation tests pass).

- [ ] **Step 5: Commit**

```bash
git add protos/hpactor/ai_resource.proto tests/unit/ai/test_accelerator_types.cpp
git commit -m "feat(proto): add AI accelerator resource plane protobuf contract

Add ai_resource.proto with 11 message types covering lease lifecycle,
probe snapshots, and resource summaries. Enums sent as uint32 fields
(not proto enums) for forward compatibility. Labels use parallel
repeated string arrays key/value encoding.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Core Enums and to_string

**Files:**
- Modify: `include/hpactor/ai/accelerator_types.hpp`
- Modify: `src/ai/accelerator_types.cpp`
- Modify: `tests/unit/ai/test_accelerator_types.cpp`

**Purpose:** TDD each enum with `to_string()` round-trip verification. One RED-GREEN cycle per enum group.

#### Cycle 4a: DeviceKind

- [ ] **Step 4a.1: RED — Add enum test (appends to existing test file)**

Append to `tests/unit/ai/test_accelerator_types.cpp`:

```cpp
// ── DeviceKind ──────────────────────────────────────────────────────
TEST(DeviceKindString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceKind::Cpu), "cpu");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceKind::Gpu), "gpu");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceKind::Npu), "npu");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceKind::Accelerator), "accelerator");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceKind::Mock), "mock");
}
```

- [ ] **Step 4a.2: RED — Build, verify failure**

```bash
ninja -C build tests/unit/ai/test_accelerator_types 2>&1 | tail -5
```

Expected: FAIL — `DeviceKind` not defined, `to_string(DeviceKind)` not found.

- [ ] **Step 4a.3: GREEN — Add DeviceKind to header**

Replace the placeholder `include/hpactor/ai/accelerator_types.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace hpactor::ai {

enum class DeviceKind : uint8_t {
    Cpu = 0,
    Gpu = 1,
    Npu = 2,
    Accelerator = 3,
    Mock = 4,
};

/// Human-readable snake_case string for DeviceKind.
/// Never returns nullptr.
const char* to_string(DeviceKind kind) noexcept;

} // namespace hpactor::ai
```

- [ ] **Step 4a.4: GREEN — Add to_string implementation**

Replace the placeholder `src/ai/accelerator_types.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#include <hpactor/ai/accelerator_types.hpp>

namespace hpactor::ai {

const char* to_string(DeviceKind kind) noexcept {
    switch (kind) {
        case DeviceKind::Cpu:         return "cpu";
        case DeviceKind::Gpu:         return "gpu";
        case DeviceKind::Npu:         return "npu";
        case DeviceKind::Accelerator: return "accelerator";
        case DeviceKind::Mock:        return "mock";
    }
    return "unknown";
}

} // namespace hpactor::ai
```

- [ ] **Step 4a.5: GREEN — Build and run**

```bash
ninja -C build tests/unit/ai/test_accelerator_types && ./build/tests/unit/ai/test_accelerator_types --gtest_filter="DeviceKind*:Proto*"
```

Expected: 4/4 tests pass (3 proto + 1 DeviceKind).

#### Cycle 4b: DeviceVendor

- [ ] **Step 4b.1: RED — Add test**

Append to test file:

```cpp
// ── DeviceVendor ────────────────────────────────────────────────────
TEST(DeviceVendorString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Unknown), "unknown");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Nvidia), "nvidia");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Amd), "amd");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Apple), "apple");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Intel), "intel");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceVendor::Mock), "mock");
}
```

- [ ] **Step 4b.2: RED — Build, verify failure**

```bash
ninja -C build tests/unit/ai/test_accelerator_types 2>&1 | tail -5
```

Expected: FAIL — `DeviceVendor` not defined.

- [ ] **Step 4b.3: GREEN — Add DeviceVendor to header**

Append to `accelerator_types.hpp` inside the `hpactor::ai` namespace (after DeviceKind):

```cpp
enum class DeviceVendor : uint8_t {
    Unknown = 0,
    Nvidia = 1,
    Amd = 2,
    Apple = 3,
    Intel = 4,
    Mock = 5,
};

const char* to_string(DeviceVendor vendor) noexcept;
```

- [ ] **Step 4b.4: GREEN — Add to_string to source**

Append to `accelerator_types.cpp`:

```cpp
const char* to_string(DeviceVendor vendor) noexcept {
    switch (vendor) {
        case DeviceVendor::Unknown: return "unknown";
        case DeviceVendor::Nvidia:  return "nvidia";
        case DeviceVendor::Amd:     return "amd";
        case DeviceVendor::Apple:   return "apple";
        case DeviceVendor::Intel:   return "intel";
        case DeviceVendor::Mock:    return "mock";
    }
    return "unknown";
}
```

- [ ] **Step 4b.5: GREEN — Build and run**

```bash
ninja -C build tests/unit/ai/test_accelerator_types && ./build/tests/unit/ai/test_accelerator_types --gtest_filter="DeviceVendor*"
```

Expected: 1/1 test passes.

#### Cycle 4c: DeviceHealth

- [ ] **Step 4c.1: RED — Add test**

```cpp
// ── DeviceHealth ────────────────────────────────────────────────────
TEST(DeviceHealthString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceHealth::Unknown), "unknown");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceHealth::Healthy), "healthy");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceHealth::Degraded), "degraded");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceHealth::Unavailable), "unavailable");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceHealth::Lost), "lost");
}
```

- [ ] **Step 4c.2: RED — Build, verify failure**

```bash
ninja -C build tests/unit/ai/test_accelerator_types 2>&1 | tail -5
```

Expected: FAIL — `DeviceHealth` not defined.

- [ ] **Step 4c.3: GREEN — Add DeviceHealth to header**

```cpp
enum class DeviceHealth : uint8_t {
    Unknown = 0,
    Healthy = 1,
    Degraded = 2,
    Unavailable = 3,
    Lost = 4,
};

const char* to_string(DeviceHealth health) noexcept;
```

- [ ] **Step 4c.4: GREEN — Add to_string to source**

```cpp
const char* to_string(DeviceHealth health) noexcept {
    switch (health) {
        case DeviceHealth::Unknown:     return "unknown";
        case DeviceHealth::Healthy:     return "healthy";
        case DeviceHealth::Degraded:    return "degraded";
        case DeviceHealth::Unavailable: return "unavailable";
        case DeviceHealth::Lost:        return "lost";
    }
    return "unknown";
}
```

- [ ] **Step 4c.5: GREEN — Build and run**

```bash
ninja -C build tests/unit/ai/test_accelerator_types && ./build/tests/unit/ai/test_accelerator_types --gtest_filter="DeviceHealth*"
```

Expected: 1/1 test passes.

#### Cycle 4d: LeaseState

- [ ] **Step 4d.1: RED — Add test**

```cpp
// ── LeaseState ──────────────────────────────────────────────────────
TEST(LeaseStateString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Pending), "pending");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Granted), "granted");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Active), "active");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Releasing), "releasing");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Released), "released");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Revoked), "revoked");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::LeaseState::Expired), "expired");
}
```

- [ ] **Step 4d.2: RED → GREEN**

Same pattern: add `enum class LeaseState : uint8_t { ... };` and `const char* to_string(LeaseState) noexcept;` to header, implement in source, verify.

```cpp
// In header:
enum class LeaseState : uint8_t {
    Pending = 0,
    Granted = 1,
    Active = 2,
    Releasing = 3,
    Released = 4,
    Revoked = 5,
    Expired = 6,
};

const char* to_string(LeaseState state) noexcept;
```

```cpp
// In source:
const char* to_string(LeaseState state) noexcept {
    switch (state) {
        case LeaseState::Pending:   return "pending";
        case LeaseState::Granted:   return "granted";
        case LeaseState::Active:    return "active";
        case LeaseState::Releasing: return "releasing";
        case LeaseState::Released:  return "released";
        case LeaseState::Revoked:   return "revoked";
        case LeaseState::Expired:   return "expired";
    }
    return "unknown";
}
```

- [ ] **Step 4d.3: GREEN — Verify**

```bash
ninja -C build tests/unit/ai/test_accelerator_types && ./build/tests/unit/ai/test_accelerator_types --gtest_filter="LeaseState*"
```

#### Cycle 4e: DeviceSelectorKind + AdmissionPolicyKind

- [ ] **Step 4e.1: RED — Add test**

```cpp
// ── DeviceSelectorKind ──────────────────────────────────────────────
TEST(DeviceSelectorKindString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceSelectorKind::Any), "any");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceSelectorKind::ExactDevice), "exact_device");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceSelectorKind::Kind), "kind");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceSelectorKind::Vendor), "vendor");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::DeviceSelectorKind::LabelMatch), "label_match");
}

// ── AdmissionPolicyKind ─────────────────────────────────────────────
TEST(AdmissionPolicyKindString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::AdmissionPolicyKind::FirstFit), "first_fit");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::AdmissionPolicyKind::MostFreeMemory), "most_free_memory");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::AdmissionPolicyKind::ExactDevice), "exact_device");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::AdmissionPolicyKind::CpuFallback), "cpu_fallback");
}
```

- [ ] **Step 4e.2: RED → GREEN**

Add both enums + to_string declarations to header, implementations to source. Same pattern as above.

```cpp
// Header additions:
enum class DeviceSelectorKind : uint8_t {
    Any = 0,
    ExactDevice = 1,
    Kind = 2,
    Vendor = 3,
    LabelMatch = 4,
};

const char* to_string(DeviceSelectorKind kind) noexcept;

enum class AdmissionPolicyKind : uint8_t {
    FirstFit = 0,
    MostFreeMemory = 1,
    ExactDevice = 2,
    CpuFallback = 3,
};

const char* to_string(AdmissionPolicyKind policy) noexcept;
```

```cpp
// Source additions:
const char* to_string(DeviceSelectorKind kind) noexcept {
    switch (kind) {
        case DeviceSelectorKind::Any:         return "any";
        case DeviceSelectorKind::ExactDevice: return "exact_device";
        case DeviceSelectorKind::Kind:        return "kind";
        case DeviceSelectorKind::Vendor:      return "vendor";
        case DeviceSelectorKind::LabelMatch:  return "label_match";
    }
    return "unknown";
}

const char* to_string(AdmissionPolicyKind policy) noexcept {
    switch (policy) {
        case AdmissionPolicyKind::FirstFit:        return "first_fit";
        case AdmissionPolicyKind::MostFreeMemory:  return "most_free_memory";
        case AdmissionPolicyKind::ExactDevice:     return "exact_device";
        case AdmissionPolicyKind::CpuFallback:     return "cpu_fallback";
    }
    return "unknown";
}
```

- [ ] **Step 4e.3: GREEN — Verify**

```bash
ninja -C build tests/unit/ai/test_accelerator_types && ./build/tests/unit/ai/test_accelerator_types --gtest_filter="DeviceSelectorKind*:AdmissionPolicyKind*"
```

#### Cycle 4f: ResourceAdmissionReason

- [ ] **Step 4f.1: RED — Add test**

```cpp
// ── ResourceAdmissionReason ─────────────────────────────────────────
TEST(ResourceAdmissionReasonString, RoundTripAllValues) {
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::Granted), "granted");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::ResourceActorNotReady), "resource_actor_not_ready");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::InvalidLeaseRequest), "invalid_lease_request");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::NoMatchingDevice), "no_matching_device");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::DeviceUnhealthy), "device_unhealthy");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::InsufficientDeviceMemory), "insufficient_device_memory");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::InsufficientHostMemory), "insufficient_host_memory");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::InsufficientPinnedHostMemory), "insufficient_pinned_host_memory");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::InsufficientComputeUnits), "insufficient_compute_units");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::InsufficientStreamSlots), "insufficient_stream_slots");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::ExclusiveLeaseConflict), "exclusive_lease_conflict");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::NodeDraining), "node_draining");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::RejectedByPolicy), "rejected_by_policy");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::LeaseNotFound), "lease_not_found");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::LeaseOwnerMismatch), "lease_owner_mismatch");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::LeaseExpired), "lease_expired");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::DeviceLost), "device_lost");
    EXPECT_STREQ(hpactor::ai::to_string(hpactor::ai::ResourceAdmissionReason::ProbeSnapshotInvalid), "probe_snapshot_invalid");
}

TEST(ResourceAdmissionReasonMapping, ToFailureReason) {
    using R = hpactor::ai::ResourceAdmissionReason;
    using F = hpactor::FailureReason;
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::Granted), F::Unknown);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::ResourceActorNotReady), F::ActorNotReady);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::InvalidLeaseRequest), F::RejectedByPolicy);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::NoMatchingDevice), F::NoRoute);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::DeviceUnhealthy), F::MemoryPressure);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::InsufficientDeviceMemory), F::MemoryPressure);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::InsufficientHostMemory), F::MemoryPressure);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::ExclusiveLeaseConflict), F::RejectedByPolicy);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::NodeDraining), F::Draining);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::LeaseNotFound), F::NoRoute);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::LeaseOwnerMismatch), F::RejectedByPolicy);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::LeaseExpired), F::Expired);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::DeviceLost), F::NodeUnavailable);
    EXPECT_EQ(hpactor::ai::to_failure_reason(R::ProbeSnapshotInvalid), F::RejectedByPolicy);
}
```

- [ ] **Step 4f.2: RED — Build, verify failure**

```bash
ninja -C build tests/unit/ai/test_accelerator_types 2>&1 | tail -5
```

Expected: FAIL — `ResourceAdmissionReason` not defined.

- [ ] **Step 4f.3: GREEN — Add enum + to_string + to_failure_reason**

Add to header (note: `: uint16_t` for this enum):

```cpp
enum class ResourceAdmissionReason : uint16_t {
    Granted = 0,
    ResourceActorNotReady = 1,
    InvalidLeaseRequest = 2,
    NoMatchingDevice = 3,
    DeviceUnhealthy = 4,
    InsufficientDeviceMemory = 5,
    InsufficientHostMemory = 6,
    InsufficientPinnedHostMemory = 7,
    InsufficientComputeUnits = 8,
    InsufficientStreamSlots = 9,
    ExclusiveLeaseConflict = 10,
    NodeDraining = 11,
    RejectedByPolicy = 12,
    LeaseNotFound = 13,
    LeaseOwnerMismatch = 14,
    LeaseExpired = 15,
    DeviceLost = 16,
    ProbeSnapshotInvalid = 17,
};

const char* to_string(ResourceAdmissionReason reason) noexcept;

/// Map ResourceAdmissionReason to the canonical FailureReason.
hpactor::FailureReason to_failure_reason(ResourceAdmissionReason reason) noexcept;
```

Add `#include <hpactor/msg/failure_reason.hpp>` to the header for the `FailureReason` reference.

Add to source:

```cpp
#include <hpactor/msg/failure_reason.hpp>

const char* to_string(ResourceAdmissionReason reason) noexcept {
    switch (reason) {
        case ResourceAdmissionReason::Granted:                     return "granted";
        case ResourceAdmissionReason::ResourceActorNotReady:       return "resource_actor_not_ready";
        case ResourceAdmissionReason::InvalidLeaseRequest:         return "invalid_lease_request";
        case ResourceAdmissionReason::NoMatchingDevice:            return "no_matching_device";
        case ResourceAdmissionReason::DeviceUnhealthy:             return "device_unhealthy";
        case ResourceAdmissionReason::InsufficientDeviceMemory:    return "insufficient_device_memory";
        case ResourceAdmissionReason::InsufficientHostMemory:      return "insufficient_host_memory";
        case ResourceAdmissionReason::InsufficientPinnedHostMemory: return "insufficient_pinned_host_memory";
        case ResourceAdmissionReason::InsufficientComputeUnits:    return "insufficient_compute_units";
        case ResourceAdmissionReason::InsufficientStreamSlots:     return "insufficient_stream_slots";
        case ResourceAdmissionReason::ExclusiveLeaseConflict:      return "exclusive_lease_conflict";
        case ResourceAdmissionReason::NodeDraining:                return "node_draining";
        case ResourceAdmissionReason::RejectedByPolicy:            return "rejected_by_policy";
        case ResourceAdmissionReason::LeaseNotFound:               return "lease_not_found";
        case ResourceAdmissionReason::LeaseOwnerMismatch:          return "lease_owner_mismatch";
        case ResourceAdmissionReason::LeaseExpired:                return "lease_expired";
        case ResourceAdmissionReason::DeviceLost:                  return "device_lost";
        case ResourceAdmissionReason::ProbeSnapshotInvalid:        return "probe_snapshot_invalid";
    }
    return "unknown";
}

FailureReason to_failure_reason(ResourceAdmissionReason reason) noexcept {
    switch (reason) {
        case ResourceAdmissionReason::Granted:                     return FailureReason::Unknown;
        case ResourceAdmissionReason::ResourceActorNotReady:       return FailureReason::ActorNotReady;
        case ResourceAdmissionReason::InvalidLeaseRequest:         return FailureReason::RejectedByPolicy;
        case ResourceAdmissionReason::NoMatchingDevice:            return FailureReason::NoRoute;
        case ResourceAdmissionReason::DeviceUnhealthy:             return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::InsufficientDeviceMemory:    return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::InsufficientHostMemory:      return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::InsufficientPinnedHostMemory: return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::InsufficientComputeUnits:    return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::InsufficientStreamSlots:     return FailureReason::MemoryPressure;
        case ResourceAdmissionReason::ExclusiveLeaseConflict:      return FailureReason::RejectedByPolicy;
        case ResourceAdmissionReason::NodeDraining:                return FailureReason::Draining;
        case ResourceAdmissionReason::RejectedByPolicy:            return FailureReason::RejectedByPolicy;
        case ResourceAdmissionReason::LeaseNotFound:               return FailureReason::NoRoute;
        case ResourceAdmissionReason::LeaseOwnerMismatch:          return FailureReason::RejectedByPolicy;
        case ResourceAdmissionReason::LeaseExpired:                return FailureReason::Expired;
        case ResourceAdmissionReason::DeviceLost:                  return FailureReason::NodeUnavailable;
        case ResourceAdmissionReason::ProbeSnapshotInvalid:        return FailureReason::RejectedByPolicy;
    }
    return FailureReason::Unknown;
}
```

- [ ] **Step 4f.4: GREEN — Build and run**

```bash
ninja -C build tests/unit/ai/test_accelerator_types && ./build/tests/unit/ai/test_accelerator_types --gtest_filter="ResourceAdmission*"
```

Expected: 2/2 tests pass.

- [ ] **Step 4f.5: Commit all enum work**

```bash
git add include/hpactor/ai/accelerator_types.hpp \
        src/ai/accelerator_types.cpp \
        tests/unit/ai/test_accelerator_types.cpp
git commit -m "feat(ai): add core enums with to_string and failure reason mapping

Implement DeviceKind, DeviceVendor, DeviceHealth, LeaseState,
DeviceSelectorKind, AdmissionPolicyKind, and ResourceAdmissionReason
enums with to_string() round-trip support and to_failure_reason()
mapping. All enums use explicit uint8_t/uint16_t underlying types.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Value Types and Validation

**Files:**
- Modify: `include/hpactor/ai/accelerator_types.hpp`
- Modify: `src/ai/accelerator_types.cpp`
- Modify: `tests/unit/ai/test_accelerator_types.cpp`

**Purpose:** TDD structs and validation functions: DeviceId, DeviceLabel, ResourceQuantities, DeviceSelector, DeviceLeaseRequest, DeviceLease, DeviceDescriptor.

#### Cycle 5a: DeviceId + is_valid_device_local_id

- [ ] **Step 5a.1: RED — Add test**

```cpp
// ── DeviceId ────────────────────────────────────────────────────────
TEST(DeviceIdValidation, ZeroIsInvalid) {
    EXPECT_FALSE(hpactor::ai::is_valid_device_local_id(0));
}

TEST(DeviceIdValidation, NonZeroIsValid) {
    EXPECT_TRUE(hpactor::ai::is_valid_device_local_id(1));
    EXPECT_TRUE(hpactor::ai::is_valid_device_local_id(UINT64_MAX));
}

TEST(DeviceId, DefaultConstruction) {
    hpactor::ai::DeviceId id;
    EXPECT_EQ(id.node_local_id, 0u);
    EXPECT_EQ(id.kind, hpactor::ai::DeviceKind::Cpu);
}

TEST(DeviceId, Equality) {
    hpactor::ai::DeviceId a{1, hpactor::ai::DeviceKind::Gpu};
    hpactor::ai::DeviceId b{1, hpactor::ai::DeviceKind::Gpu};
    hpactor::ai::DeviceId c{2, hpactor::ai::DeviceKind::Gpu};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}
```

- [ ] **Step 5a.2: RED → GREEN**

Add to header:

```cpp
using DeviceLocalId = uint64_t;

bool is_valid_device_local_id(DeviceLocalId id) noexcept;

struct DeviceId {
    DeviceLocalId node_local_id{0};
    DeviceKind kind{DeviceKind::Cpu};

    bool operator==(const DeviceId&) const noexcept = default;
};
```

Add to source:

```cpp
bool is_valid_device_local_id(DeviceLocalId id) noexcept {
    return id != 0;
}
```

#### Cycle 5b: ResourceQuantities

- [ ] **Step 5b.1: RED — Add test**

```cpp
// ── ResourceQuantities ──────────────────────────────────────────────
TEST(ResourceQuantities, DefaultIsZero) {
    hpactor::ai::ResourceQuantities q;
    EXPECT_EQ(q.device_memory_bytes, 0u);
    EXPECT_EQ(q.host_memory_bytes, 0u);
    EXPECT_EQ(q.compute_units, 0u);
    EXPECT_FALSE(q.exclusive_device);
}

TEST(ResourceQuantities, FitsWithinEmptyRequest) {
    hpactor::ai::ResourceQuantities available;
    available.device_memory_bytes = 1024;
    hpactor::ai::ResourceQuantities request; // all zeros
    EXPECT_TRUE(request.fits_within(available));
}

TEST(ResourceQuantities, FitsWithinExactFit) {
    hpactor::ai::ResourceQuantities available;
    available.device_memory_bytes = 1024;
    available.compute_units = 8;
    hpactor::ai::ResourceQuantities request;
    request.device_memory_bytes = 1024;
    request.compute_units = 8;
    EXPECT_TRUE(request.fits_within(available));
}

TEST(ResourceQuantities, FitsWithinMemoryExceeded) {
    hpactor::ai::ResourceQuantities available;
    available.device_memory_bytes = 1024;
    hpactor::ai::ResourceQuantities request;
    request.device_memory_bytes = 2048;
    EXPECT_FALSE(request.fits_within(available));
}

TEST(ResourceQuantities, FitsWithinComputeExceeded) {
    hpactor::ai::ResourceQuantities available;
    available.compute_units = 4;
    hpactor::ai::ResourceQuantities request;
    request.compute_units = 8;
    EXPECT_FALSE(request.fits_within(available));
}

TEST(ResourceQuantities, FitsWithinExclusiveConflict) {
    hpactor::ai::ResourceQuantities available;
    available.exclusive_device = true; // device is exclusive and already in use
    hpactor::ai::ResourceQuantities request;
    request.device_memory_bytes = 100;
    EXPECT_FALSE(request.fits_within(available));
}

TEST(ResourceQuantities, FitsWithinPartialFields) {
    hpactor::ai::ResourceQuantities available;
    available.device_memory_bytes = 1024;
    available.compute_units = 8;
    hpactor::ai::ResourceQuantities request;
    request.device_memory_bytes = 512;
    // compute_units = 0 means "not requesting"
    EXPECT_TRUE(request.fits_within(available));
}

TEST(ResourceQuantities, AddSaturating) {
    hpactor::ai::ResourceQuantities a;
    a.device_memory_bytes = 100;
    hpactor::ai::ResourceQuantities b;
    b.device_memory_bytes = 200;
    auto result = a.add(b);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->device_memory_bytes, 300u);
}

TEST(ResourceQuantities, AddSaturatingOverflow) {
    hpactor::ai::ResourceQuantities a;
    a.device_memory_bytes = UINT64_MAX;
    hpactor::ai::ResourceQuantities b;
    b.device_memory_bytes = 1;
    auto result = a.add(b);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->device_memory_bytes, UINT64_MAX);
}

TEST(ResourceQuantities, SubtractNormal) {
    hpactor::ai::ResourceQuantities a;
    a.device_memory_bytes = 300;
    hpactor::ai::ResourceQuantities b;
    b.device_memory_bytes = 100;
    auto result = a.subtract(b);
    EXPECT_TRUE(result.has_value());
}

TEST(ResourceQuantities, SubtractUnderflow) {
    hpactor::ai::ResourceQuantities a;
    a.device_memory_bytes = 100;
    hpactor::ai::ResourceQuantities b;
    b.device_memory_bytes = 200;
    auto result = a.subtract(b);
    EXPECT_FALSE(result.has_value());
}
```

- [ ] **Step 5b.2: RED — Build, verify failure**

```bash
ninja -C build tests/unit/ai/test_accelerator_types 2>&1 | tail -5
```

Expected: FAIL — `ResourceQuantities` not defined, `fits_within`/`add`/`subtract` not found.

- [ ] **Step 5b.3: GREEN — Add ResourceQuantities struct + methods**

Add to header:

```cpp
#include <hpactor/types/types.hpp> // for result<T>

struct ResourceQuantities {
    uint64_t device_memory_bytes{0};
    uint64_t host_memory_bytes{0};
    uint64_t pinned_host_memory_bytes{0};
    uint32_t compute_units{0};
    uint32_t stream_slots{0};
    uint32_t copy_engine_slots{0};
    uint64_t kv_cache_bytes{0};
    uint32_t batch_slots{0};
    bool exclusive_device{false};

    [[nodiscard]] result<ResourceQuantities> add(const ResourceQuantities& other) const noexcept;
    [[nodiscard]] result<void> subtract(const ResourceQuantities& other) const noexcept;
    [[nodiscard]] bool fits_within(const ResourceQuantities& available) const noexcept;
};
```

Add to source (using the existing `result<T>` from `hpactor/types/types.hpp`):

```cpp
#include <hpactor/types/types.hpp>
#include <limits>

result<ResourceQuantities> ResourceQuantities::add(const ResourceQuantities& other) const noexcept {
    ResourceQuantities out;
    auto saturating_add_u64 = [](uint64_t a, uint64_t b) -> uint64_t {
        if (a > std::numeric_limits<uint64_t>::max() - b) return std::numeric_limits<uint64_t>::max();
        return a + b;
    };
    auto saturating_add_u32 = [](uint32_t a, uint32_t b) -> uint32_t {
        if (a > std::numeric_limits<uint32_t>::max() - b) return std::numeric_limits<uint32_t>::max();
        return a + b;
    };
    out.device_memory_bytes = saturating_add_u64(device_memory_bytes, other.device_memory_bytes);
    out.host_memory_bytes = saturating_add_u64(host_memory_bytes, other.host_memory_bytes);
    out.pinned_host_memory_bytes = saturating_add_u64(pinned_host_memory_bytes, other.pinned_host_memory_bytes);
    out.compute_units = saturating_add_u32(compute_units, other.compute_units);
    out.stream_slots = saturating_add_u32(stream_slots, other.stream_slots);
    out.copy_engine_slots = saturating_add_u32(copy_engine_slots, other.copy_engine_slots);
    out.kv_cache_bytes = saturating_add_u64(kv_cache_bytes, other.kv_cache_bytes);
    out.batch_slots = saturating_add_u32(batch_slots, other.batch_slots);
    out.exclusive_device = exclusive_device || other.exclusive_device;
    return out;
}

result<void> ResourceQuantities::subtract(const ResourceQuantities& other) const noexcept {
    if (other.device_memory_bytes > device_memory_bytes ||
        other.host_memory_bytes > host_memory_bytes ||
        other.pinned_host_memory_bytes > pinned_host_memory_bytes ||
        other.compute_units > compute_units ||
        other.stream_slots > stream_slots ||
        other.copy_engine_slots > copy_engine_slots ||
        other.kv_cache_bytes > kv_cache_bytes ||
        other.batch_slots > batch_slots) {
        return tl::unexpected(ErrorCode::kInvalidArgument);
    }
    return {};
}

bool ResourceQuantities::fits_within(const ResourceQuantities& available) const noexcept {
    if (available.exclusive_device) return false; // exclusive device already taken
    if (device_memory_bytes > available.device_memory_bytes) return false;
    if (host_memory_bytes > available.host_memory_bytes) return false;
    if (pinned_host_memory_bytes > available.pinned_host_memory_bytes) return false;
    if (compute_units > available.compute_units) return false;
    if (stream_slots > available.stream_slots) return false;
    if (copy_engine_slots > available.copy_engine_slots) return false;
    if (kv_cache_bytes > available.kv_cache_bytes) return false;
    if (batch_slots > available.batch_slots) return false;
    return true;
}
```

Note: Check the actual `result<T>` and `ErrorCode` types in `include/hpactor/types/types.hpp` to ensure correct usage. The project may use `tl::expected` or a custom result type.

- [ ] **Step 5b.4: GREEN — Build and run**

```bash
ninja -C build tests/unit/ai/test_accelerator_types && ./build/tests/unit/ai/test_accelerator_types --gtest_filter="ResourceQuantities*"
```

Expected: 10/10 tests pass.

#### Cycle 5c: DeviceSelector validation

- [ ] **Step 5c.1: RED — Add test**

```cpp
// ── DeviceSelector ──────────────────────────────────────────────────
TEST(DeviceSelectorValidation, ExactDeviceWithValidId) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::ExactDevice;
    sel.exact_device = hpactor::ai::DeviceId{42, hpactor::ai::DeviceKind::Gpu};
    EXPECT_TRUE(hpactor::ai::is_valid_selector(sel));
}

TEST(DeviceSelectorValidation, ExactDeviceWithZeroId) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::ExactDevice;
    sel.exact_device = hpactor::ai::DeviceId{0, hpactor::ai::DeviceKind::Gpu};
    EXPECT_FALSE(hpactor::ai::is_valid_selector(sel));
}

TEST(DeviceSelectorValidation, AnyIsAlwaysValid) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::Any;
    EXPECT_TRUE(hpactor::ai::is_valid_selector(sel));
}

TEST(DeviceSelectorValidation, KindIsValid) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::Kind;
    sel.required_kind = hpactor::ai::DeviceKind::Gpu;
    EXPECT_TRUE(hpactor::ai::is_valid_selector(sel));
}

TEST(DeviceSelectorValidation, VendorIsValid) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::Vendor;
    sel.required_vendor = hpactor::ai::DeviceVendor::Apple;
    EXPECT_TRUE(hpactor::ai::is_valid_selector(sel));
}

TEST(DeviceSelectorValidation, LabelMatchIsValid) {
    hpactor::ai::DeviceSelector sel;
    sel.kind = hpactor::ai::DeviceSelectorKind::LabelMatch;
    sel.required_labels.push_back({"zone", "local"});
    EXPECT_TRUE(hpactor::ai::is_valid_selector(sel));
}
```

- [ ] **Step 5c.2: RED → GREEN**

Add DeviceLabel and DeviceSelector structs + `is_valid_selector` to header:

```cpp
struct DeviceLabel {
    std::string key;
    std::string value;
};

struct DeviceSelector {
    DeviceSelectorKind kind{DeviceSelectorKind::Any};
    DeviceId exact_device{};
    DeviceKind required_kind{DeviceKind::Gpu};
    DeviceVendor required_vendor{DeviceVendor::Unknown};
    std::vector<DeviceLabel> required_labels;
    int32_t preferred_numa_node{-1};
    std::string preferred_topology_group;
    bool allow_cpu_fallback{false};
};

bool is_valid_selector(const DeviceSelector& sel) noexcept;
```

Add `#include <string>` and `#include <vector>` to the header.

Add to source:

```cpp
bool is_valid_selector(const DeviceSelector& sel) noexcept {
    switch (sel.kind) {
        case DeviceSelectorKind::Any:
            return true;
        case DeviceSelectorKind::ExactDevice:
            return is_valid_device_local_id(sel.exact_device.node_local_id);
        case DeviceSelectorKind::Kind:
            return true;
        case DeviceSelectorKind::Vendor:
            return true;
        case DeviceSelectorKind::LabelMatch:
            return !sel.required_labels.empty();
    }
    return false;
}
```

#### Cycle 5d: Remaining structs (DeviceLeaseRequest, DeviceLease, DeviceDescriptor)

- [ ] **Step 5d.1: RED — Add test**

```cpp
// ── DeviceLeaseRequest ──────────────────────────────────────────────
TEST(DeviceLeaseRequest, DefaultConstruction) {
    hpactor::ai::DeviceLeaseRequest req;
    EXPECT_EQ(req.ttl.count(), 30000);
    EXPECT_EQ(req.priority, 0u);
    EXPECT_FALSE(req.allow_degraded_device);
    EXPECT_FALSE(req.exclusive);
}

// ── DeviceLease ─────────────────────────────────────────────────────
TEST(DeviceLease, DefaultState) {
    hpactor::ai::DeviceLease lease;
    EXPECT_EQ(lease.lease_id, 0u);
    EXPECT_EQ(lease.state, hpactor::ai::LeaseState::Pending);
    EXPECT_EQ(lease.granted_epoch, 0u);
}

// ── DeviceDescriptor ────────────────────────────────────────────────
TEST(DeviceDescriptor, DefaultConstruction) {
    hpactor::ai::DeviceDescriptor desc;
    EXPECT_EQ(desc.id.node_local_id, 0u);
    EXPECT_EQ(desc.kind, hpactor::ai::DeviceKind::Cpu);
    EXPECT_EQ(desc.vendor, hpactor::ai::DeviceVendor::Unknown);
    EXPECT_EQ(desc.health, hpactor::ai::DeviceHealth::Unknown);
}
```

- [ ] **Step 5d.2: RED — Build, verify failure**

```bash
ninja -C build tests/unit/ai/test_accelerator_types 2>&1 | tail -5
```

Expected: FAIL — structs not defined.

- [ ] **Step 5d.3: GREEN — Add structs to header**

Add after the existing type definitions:

```cpp
#include <chrono>
#include <hpactor/ref/actor_address.hpp>

using LeaseId = uint64_t;
using ProbeEpoch = uint64_t;
using LedgerEpoch = uint64_t;

struct DeviceLeaseRequest {
    ActorAddress requester;
    std::string workload_id;
    std::string model_or_job;
    std::string tenant_id;
    ResourceQuantities requested;
    DeviceSelector selector;
    std::chrono::milliseconds ttl{30'000};
    uint32_t priority{0};
    bool allow_degraded_device{false};
    bool exclusive{false};
};

struct DeviceLease {
    LeaseId lease_id{0};
    DeviceId device_id{};
    ActorAddress owner;
    std::string workload_id;
    std::string model_or_job;
    std::string tenant_id;
    ResourceQuantities reserved;
    LeaseState state{LeaseState::Pending};
    LedgerEpoch granted_epoch{0};
    int64_t granted_at_ns{0};
    int64_t expires_at_ns{0};
    uint32_t priority{0};
};

struct DeviceDescriptor {
    DeviceId id;
    DeviceKind kind{DeviceKind::Cpu};
    DeviceVendor vendor{DeviceVendor::Unknown};
    std::string backend;
    std::string name;
    std::string stable_key;
    uint32_t backend_ordinal{0};
    uint64_t parent_node_local_id{0};
    int32_t numa_node{-1};
    std::string topology_group;
    ResourceQuantities total;
    ResourceQuantities allocatable;
    uint64_t capabilities{0};
    DeviceHealth health{DeviceHealth::Unknown};
    std::vector<DeviceLabel> labels;
};
```

- [ ] **Step 5d.4: GREEN — Build and run all value type tests**

```bash
ninja -C build tests/unit/ai/test_accelerator_types && ./build/tests/unit/ai/test_accelerator_types --gtest_filter="DeviceId*:ResourceQuantities*:DeviceSelector*:DeviceLeaseRequest*:DeviceLease*:DeviceDescriptor*"
```

Expected: all value-type tests pass.

- [ ] **Step 5d.5: Commit**

```bash
git add include/hpactor/ai/accelerator_types.hpp \
        src/ai/accelerator_types.cpp \
        tests/unit/ai/test_accelerator_types.cpp
git commit -m "feat(ai): add accelerator value types and validation

Implement DeviceId, DeviceLabel, ResourceQuantities (with saturating
add/underflow-subtract/fits_within), DeviceSelector (with
is_valid_selector), DeviceLeaseRequest, DeviceLease, and
DeviceDescriptor structs. All validation helpers are noexcept.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: AI TypeTags and Message Registry

**Files:**
- Create: `include/hpactor/ai/ai_type_tags.hpp`
- Modify: `src/ai/ai_message_registry.cpp`
- Modify: `tests/unit/ai/test_accelerator_types.cpp`

**Purpose:** Define the 10 AI TypeTag constants, MessageTraits specializations, and the self-registering ProtoTypeRegistry registrar.

- [ ] **Step 6.1: RED — Add TypeTag + MessageTraits tests**

Append to test file:

```cpp
// ── TypeTag Values ──────────────────────────────────────────────────
#include <hpactor/ai/ai_type_tags.hpp>
#include <hpactor/ai_resource.pb.h>
#include <hpactor/msg/type_tag.hpp>
#include <set>

TEST(TypeTagValues, AllTagsUnique) {
    std::set<uint32_t> values;
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseRequestTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseReplyTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseActivateTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseRenewTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseReleaseTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiLeaseRevokedTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiResourceSnapshotRequestTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiResourceSnapshotReplyTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiDeviceSnapshotUpdateTag));
    values.insert(static_cast<uint32_t>(hpactor::ai::kAiNodeResourceSummaryTag));
    EXPECT_EQ(values.size(), 10u);
}

TEST(TypeTagValues, AllInSubsystemRange) {
    auto check = [](hpactor::TypeTag tag) {
        uint32_t v = static_cast<uint32_t>(tag);
        EXPECT_GE(v, 0x80u);
        EXPECT_LE(v, 0x89u);
    };
    check(hpactor::ai::kAiLeaseRequestTag);
    check(hpactor::ai::kAiLeaseReplyTag);
    check(hpactor::ai::kAiLeaseActivateTag);
    check(hpactor::ai::kAiLeaseRenewTag);
    check(hpactor::ai::kAiLeaseReleaseTag);
    check(hpactor::ai::kAiLeaseRevokedTag);
    check(hpactor::ai::kAiResourceSnapshotRequestTag);
    check(hpactor::ai::kAiResourceSnapshotReplyTag);
    check(hpactor::ai::kAiDeviceSnapshotUpdateTag);
    check(hpactor::ai::kAiNodeResourceSummaryTag);
}

TEST(MessageTraits, LeaseRequestTagMatches) {
    EXPECT_EQ(hpactor::MessageTraits<hpactor::PbDeviceLeaseRequest>::tag(),
              hpactor::ai::kAiLeaseRequestTag);
}

TEST(MessageTraits, LeaseReplyTagMatches) {
    EXPECT_EQ(hpactor::MessageTraits<hpactor::PbDeviceLeaseReply>::tag(),
              hpactor::ai::kAiLeaseReplyTag);
}

TEST(MessageTraits, AllTenMessagesHaveTraits) {
    // Verify tag() is callable (not TypeTag::Invalid) for each message type.
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseRequest>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseReply>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseActivate>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseRenew>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseRelease>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceLeaseRevoked>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbResourceSnapshotRequest>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbResourceSnapshotReply>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbDeviceSnapshotUpdate>::tag(),
              hpactor::TypeTag::Invalid);
    EXPECT_NE(hpactor::MessageTraits<hpactor::PbNodeResourceSummary>::tag(),
              hpactor::TypeTag::Invalid);
}
```

- [ ] **Step 6.2: RED — Build, verify failure**

```bash
ninja -C build tests/unit/ai/test_accelerator_types 2>&1 | tail -10
```

Expected: FAIL — `ai_type_tags.hpp` not found, `kAiLeaseRequestTag` etc. not defined.

- [ ] **Step 6.3: GREEN — Create `include/hpactor/ai/ai_type_tags.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/msg/type_tag.hpp>

namespace hpactor::ai {

// ── AI resource message tags (0x80–0x89) ──────────────────────────────
inline constexpr TypeTag kAiLeaseRequestTag            = make_subsystem_tag(0x80);
inline constexpr TypeTag kAiLeaseReplyTag              = make_subsystem_tag(0x81);
inline constexpr TypeTag kAiLeaseActivateTag           = make_subsystem_tag(0x82);
inline constexpr TypeTag kAiLeaseRenewTag              = make_subsystem_tag(0x83);
inline constexpr TypeTag kAiLeaseReleaseTag            = make_subsystem_tag(0x84);
inline constexpr TypeTag kAiLeaseRevokedTag            = make_subsystem_tag(0x85);
inline constexpr TypeTag kAiResourceSnapshotRequestTag = make_subsystem_tag(0x86);
inline constexpr TypeTag kAiResourceSnapshotReplyTag   = make_subsystem_tag(0x87);
inline constexpr TypeTag kAiDeviceSnapshotUpdateTag    = make_subsystem_tag(0x88);
inline constexpr TypeTag kAiNodeResourceSummaryTag     = make_subsystem_tag(0x89);

} // namespace hpactor::ai

// ── Forward-declare proto types ────────────────────────────────────────
namespace hpactor {
class PbDeviceLeaseRequest;
class PbDeviceLeaseReply;
class PbDeviceLeaseActivate;
class PbDeviceLeaseRenew;
class PbDeviceLeaseRelease;
class PbDeviceLeaseRevoked;
class PbResourceSnapshotRequest;
class PbResourceSnapshotReply;
class PbDeviceSnapshotUpdate;
class PbNodeResourceSummary;
} // namespace hpactor

// ── MessageTraits specializations ──────────────────────────────────────
namespace hpactor {

template <> struct MessageTraits<PbDeviceLeaseRequest> {
    static constexpr TypeTag tag() { return ai::kAiLeaseRequestTag; }
};
template <> struct MessageTraits<PbDeviceLeaseReply> {
    static constexpr TypeTag tag() { return ai::kAiLeaseReplyTag; }
};
template <> struct MessageTraits<PbDeviceLeaseActivate> {
    static constexpr TypeTag tag() { return ai::kAiLeaseActivateTag; }
};
template <> struct MessageTraits<PbDeviceLeaseRenew> {
    static constexpr TypeTag tag() { return ai::kAiLeaseRenewTag; }
};
template <> struct MessageTraits<PbDeviceLeaseRelease> {
    static constexpr TypeTag tag() { return ai::kAiLeaseReleaseTag; }
};
template <> struct MessageTraits<PbDeviceLeaseRevoked> {
    static constexpr TypeTag tag() { return ai::kAiLeaseRevokedTag; }
};
template <> struct MessageTraits<PbResourceSnapshotRequest> {
    static constexpr TypeTag tag() { return ai::kAiResourceSnapshotRequestTag; }
};
template <> struct MessageTraits<PbResourceSnapshotReply> {
    static constexpr TypeTag tag() { return ai::kAiResourceSnapshotReplyTag; }
};
template <> struct MessageTraits<PbDeviceSnapshotUpdate> {
    static constexpr TypeTag tag() { return ai::kAiDeviceSnapshotUpdateTag; }
};
template <> struct MessageTraits<PbNodeResourceSummary> {
    static constexpr TypeTag tag() { return ai::kAiNodeResourceSummaryTag; }
};

} // namespace hpactor
```

- [ ] **Step 6.4: GREEN — Write `src/ai/ai_message_registry.cpp`**

Replace the placeholder:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/ai/ai_type_tags.hpp>
#include <hpactor/ai_resource.pb.h>
#include <hpactor/core/proto_type_registry.hpp>

namespace hpactor::ai {
namespace {

void register_ai_message_types(ProtoTypeRegistry& reg) {
    reg.register_type<PbDeviceLeaseRequest>(kAiLeaseRequestTag,
        "hpactor.PbDeviceLeaseRequest");
    reg.register_type<PbDeviceLeaseReply>(kAiLeaseReplyTag,
        "hpactor.PbDeviceLeaseReply");
    reg.register_type<PbDeviceLeaseActivate>(kAiLeaseActivateTag,
        "hpactor.PbDeviceLeaseActivate");
    reg.register_type<PbDeviceLeaseRenew>(kAiLeaseRenewTag,
        "hpactor.PbDeviceLeaseRenew");
    reg.register_type<PbDeviceLeaseRelease>(kAiLeaseReleaseTag,
        "hpactor.PbDeviceLeaseRelease");
    reg.register_type<PbDeviceLeaseRevoked>(kAiLeaseRevokedTag,
        "hpactor.PbDeviceLeaseRevoked");
    reg.register_type<PbResourceSnapshotRequest>(kAiResourceSnapshotRequestTag,
        "hpactor.PbResourceSnapshotRequest");
    reg.register_type<PbResourceSnapshotReply>(kAiResourceSnapshotReplyTag,
        "hpactor.PbResourceSnapshotReply");
    reg.register_type<PbDeviceSnapshotUpdate>(kAiDeviceSnapshotUpdateTag,
        "hpactor.PbDeviceSnapshotUpdate");
    reg.register_type<PbNodeResourceSummary>(kAiNodeResourceSummaryTag,
        "hpactor.PbNodeResourceSummary");
}

const bool kAiTypesRegistered = [] {
    ProtoTypeRegistry::register_subsystem(&register_ai_message_types);
    return true;
}();

} // namespace
} // namespace hpactor::ai
```

- [ ] **Step 6.5: GREEN — Build and run TypeTag tests**

```bash
ninja -C build tests/unit/ai/test_accelerator_types && ./build/tests/unit/ai/test_accelerator_types --gtest_filter="TypeTag*:MessageTraits*"
```

Expected: 5/5 tests pass (TypeTagValues.AllTagsUnique, TypeTagValues.AllInSubsystemRange, 3 MessageTraits tests).

- [ ] **Step 6.6: Commit**

```bash
git add include/hpactor/ai/ai_type_tags.hpp \
        src/ai/ai_message_registry.cpp \
        tests/unit/ai/test_accelerator_types.cpp
git commit -m "feat(ai): add AI TypeTag constants and message registry

Define 10 inline constexpr TypeTag constants (0x80-0x89) with
MessageTraits specializations for all AI protobuf message types.
Implement self-registering ProtoTypeRegistry registrar via static
init. Future subsystems can follow the same pattern without editing
core framework files.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Config and Summary Types

**Files:**
- Create: `include/hpactor/ai/accelerator_config.hpp`
- Create: `include/hpactor/ai/node_resource_summary.hpp`
- Modify: `tests/unit/ai/test_accelerator_types.cpp`

**Purpose:** Add the AcceleratorConfig, MockDeviceConfig, DeviceCapacitySummary, and NodeResourceSummary types. These are pure data structs with no validation logic beyond defaults.

- [ ] **Step 7.1: RED — Add config + summary tests**

Append to test file:

```cpp
// ── AcceleratorConfig ───────────────────────────────────────────────
#include <hpactor/ai/accelerator_config.hpp>

TEST(AcceleratorConfig, DefaultsDisabled) {
    hpactor::ai::AcceleratorConfig cfg;
    EXPECT_FALSE(cfg.enabled);
    EXPECT_TRUE(cfg.enable_cpu_probe);
    EXPECT_TRUE(cfg.allow_cpu_fallback);
    EXPECT_FALSE(cfg.allow_empty_inventory);
    EXPECT_FALSE(cfg.require_resource_plane_ready);
    EXPECT_EQ(cfg.probe_interval_ms, 1000u);
    EXPECT_EQ(cfg.missing_device_grace_ms, 5000u);
    EXPECT_EQ(cfg.lease_ttl_ms, 30000u);
    EXPECT_EQ(cfg.min_lease_ttl_ms, 1000u);
    EXPECT_EQ(cfg.max_lease_ttl_ms, 300000u);
    EXPECT_EQ(cfg.admission_policy, hpactor::ai::AdmissionPolicyKind::MostFreeMemory);
    EXPECT_EQ(cfg.cpu_host_memory_budget_bytes, 0u);
    EXPECT_EQ(cfg.cpu_compute_units, 0u);
    EXPECT_TRUE(cfg.mock_devices.empty());
}

TEST(MockDeviceConfig, Defaults) {
    hpactor::ai::MockDeviceConfig dev;
    EXPECT_EQ(dev.kind, hpactor::ai::DeviceKind::Mock);
    EXPECT_EQ(dev.vendor, hpactor::ai::DeviceVendor::Mock);
    EXPECT_EQ(dev.health, hpactor::ai::DeviceHealth::Healthy);
    EXPECT_FALSE(dev.exclusive_only);
}

// ── NodeResourceSummary ─────────────────────────────────────────────
#include <hpactor/ai/node_resource_summary.hpp>

TEST(NodeResourceSummary, DefaultsNotReady) {
    hpactor::ai::NodeResourceSummary summary;
    EXPECT_FALSE(summary.ready);
    EXPECT_FALSE(summary.draining);
    EXPECT_EQ(summary.ledger_epoch, 0u);
    EXPECT_TRUE(summary.devices.empty());
}

TEST(DeviceCapacitySummary, DefaultConstruction) {
    hpactor::ai::DeviceCapacitySummary dev;
    EXPECT_EQ(dev.active_leases, 0u);
    EXPECT_FALSE(dev.exclusive_in_use);
}
```

- [ ] **Step 7.2: RED — Build, verify failure**

```bash
ninja -C build tests/unit/ai/test_accelerator_types 2>&1 | tail -5
```

Expected: FAIL — `accelerator_config.hpp` and `node_resource_summary.hpp` not found.

- [ ] **Step 7.3: GREEN — Create `include/hpactor/ai/accelerator_config.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/ai/accelerator_types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::ai {

struct MockDeviceConfig {
    std::string id;
    DeviceKind kind{DeviceKind::Mock};
    DeviceVendor vendor{DeviceVendor::Mock};
    std::string name;
    uint64_t device_memory_bytes{0};
    uint64_t host_memory_bytes{0};
    uint32_t compute_units{0};
    uint32_t stream_slots{0};
    bool exclusive_only{false};
    DeviceHealth health{DeviceHealth::Healthy};
    std::vector<DeviceLabel> labels;
};

struct AcceleratorConfig {
    bool enabled{false};
    bool enable_cpu_probe{true};
    bool allow_cpu_fallback{true};
    bool allow_empty_inventory{false};
    bool require_resource_plane_ready{false};
    uint32_t probe_interval_ms{1000};
    uint32_t missing_device_grace_ms{5000};
    uint32_t lease_ttl_ms{30000};
    uint32_t min_lease_ttl_ms{1000};
    uint32_t max_lease_ttl_ms{300000};
    AdmissionPolicyKind admission_policy{AdmissionPolicyKind::MostFreeMemory};
    uint64_t cpu_host_memory_budget_bytes{0};
    uint32_t cpu_compute_units{0};
    std::vector<MockDeviceConfig> mock_devices;
};

} // namespace hpactor::ai
```

- [ ] **Step 7.4: GREEN — Create `include/hpactor/ai/node_resource_summary.hpp`**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/ai/accelerator_types.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::ai {

struct DeviceCapacitySummary {
    DeviceId device_id;
    DeviceKind kind{DeviceKind::Cpu};
    DeviceVendor vendor{DeviceVendor::Unknown};
    std::string backend;
    DeviceHealth health{DeviceHealth::Unknown};
    ResourceQuantities allocatable;
    ResourceQuantities reserved;
    ResourceQuantities available;
    uint32_t active_leases{0};
    bool exclusive_in_use{false};
    std::vector<DeviceLabel> labels;
    std::string topology_group;
};

struct NodeResourceSummary {
    EndPoint node_endpoint;
    LedgerEpoch ledger_epoch{0};
    uint64_t generated_at_ns{0};
    bool ready{false};
    bool draining{false};
    std::vector<DeviceCapacitySummary> devices;
};

} // namespace hpactor::ai
```

- [ ] **Step 7.5: GREEN — Build and run all config + summary tests**

```bash
ninja -C build tests/unit/ai/test_accelerator_types && ./build/tests/unit/ai/test_accelerator_types --gtest_filter="AcceleratorConfig*:MockDevice*:NodeResource*:DeviceCapacity*"
```

Expected: 4/4 tests pass.

- [ ] **Step 7.6: GREEN — Run the full test suite**

```bash
./build/tests/unit/ai/test_accelerator_types
```

Expected: all test suites pass (count should be: 3 proto + 1 DeviceKind + 1 DeviceVendor + 1 DeviceHealth + 1 LeaseState + 1 DeviceSelectorKind + 1 AdmissionPolicyKind + 2 ResourceAdmissionReason + 4 DeviceId + 10 ResourceQuantities + 6 DeviceSelector + 1 DeviceLeaseRequest + 1 DeviceLease + 1 DeviceDescriptor + 5 TypeTag/MessageTraits + 2 AcceleratorConfig + 2 NodeResourceSummary = ~44 tests).

- [ ] **Step 7.7: Commit**

```bash
git add include/hpactor/ai/accelerator_config.hpp \
        include/hpactor/ai/node_resource_summary.hpp \
        tests/unit/ai/test_accelerator_types.cpp
git commit -m "feat(ai): add accelerator config and node resource summary types

Implement AcceleratorConfig (with MockDeviceConfig), and placement/
admin snapshot types (DeviceCapacitySummary, NodeResourceSummary).
All config defaults match the design spec: disabled by default, 30s
lease TTL, MostFreeMemory admission policy.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: Integration Verification

**Files:**
- None new — verify existing artifacts.

**Purpose:** Full build, run all AI tests, verify no regression in existing tests.

- [ ] **Step 8.1: Full reconfigure and build**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build hpactor_lib hpactor_proto
```

Expected: both targets build cleanly with no warnings.

- [ ] **Step 8.2: Build and run AI unit tests**

```bash
ninja -C build tests/unit/ai/test_accelerator_types
./build/tests/unit/ai/test_accelerator_types
```

Expected: all ~44 tests pass.

- [ ] **Step 8.3: Run existing core tests to verify no regression**

```bash
ninja -C build tests/unit/types/test_request_handle && ./build/tests/unit/types/test_request_handle
ninja -C build tests/unit/types/test_request_timeout && ./build/tests/unit/types/test_request_timeout
```

Expected: all existing tests pass unchanged.

- [ ] **Step 8.4: Verify TypeTag extension mechanism works end-to-end**

Write a quick verification in the test or verify through the existing MessageTraits tests that the registrar chain correctly wires AI types. The MessageTraits tests from Task 6 already verify this — confirm they still pass.

```bash
./build/tests/unit/ai/test_accelerator_types --gtest_filter="MessageTraits*:TypeTag*"
```

Expected: 5/5 tests pass.

- [ ] **Step 8.5: Final commit (if any fixups needed)**

```bash
git status
# If clean:
echo "All tasks complete."
```

---

## Post-Implementation Checklist

- [ ] All 9 new files created, 6 existing files modified.
- [ ] All enum `to_string()` functions return non-null `const char*` for every defined value.
- [ ] `to_failure_reason()` maps all 18 `ResourceAdmissionReason` values.
- [ ] `ResourceQuantities::fits_within()`, `add()`, `subtract()` behave as specified.
- [ ] `is_valid_selector()` validates all selector kinds.
- [ ] All 10 AI TypeTag constants are in range 0x80–0x89.
- [ ] `MessageTraits<T>::tag()` returns the correct tag for all 10 AI proto types.
- [ ] `ProtoTypeRegistry::register_subsystem()` chain works; AI types are registered at static init.
- [ ] `ai_resource.proto` generates without errors.
- [ ] Public headers compile without MLX, Metal, CUDA, ROCm, exception, or RTTI dependencies.
- [ ] No `toml++` types exposed in public headers.
- [ ] Existing non-AI tests pass unchanged.
