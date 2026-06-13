# AI-ACC-001A: Accelerator Public Types and Protobuf Contract — Design Spec

**Status:** Approved design; implementation not started
**Requirement ID:** AI-ACC-001A
**Parent Issue:** [#157](https://github.com/skg7on/HPActor/issues/157) — AI-ACC-001 Accelerator resource plane implementation backlog
**Source Spec:** [Accelerator Resource Plane Detailed Design Spec](../../architecture/ai/accelerator-resource-plane-detailed-design-spec.md)
**Related Issue:** [#159](https://github.com/skg7on/HPActor/issues/159) — AI-ACC-001B Build gate and TOML config parser

## 1. Purpose

This is the first implementation issue of the AI accelerator resource plane
(AI-ACC-001). It defines the stable public value model, protobuf control-message
contract, TypeTag reservations, and string-conversion helpers for node-local
accelerator resources — without implementing any actor behavior, ledger logic,
probes, or config parsing.

A secondary deliverable is a **TypeTag extension mechanism** that removes the need
to edit core framework files (`type_tag.hpp`, `proto_type_registry.hpp`,
`proto_type_registry.cpp`) when future subsystems add system-range message types.

## 2. Scope

### In scope

- `include/hpactor/ai/accelerator_types.hpp` — DeviceId, enums (DeviceKind,
  DeviceVendor, DeviceHealth, LeaseState, DeviceSelectorKind,
  ResourceAdmissionReason, AdmissionPolicyKind), ResourceQuantities,
  DeviceSelector, DeviceLeaseRequest, DeviceLease, DeviceDescriptor,
  DeviceLabel, and no-throw validation helpers.
- `include/hpactor/ai/accelerator_config.hpp` — AcceleratorConfig,
  MockDeviceConfig structs.
- `include/hpactor/ai/node_resource_summary.hpp` — DeviceCapacitySummary,
  NodeResourceSummary for placement/admin snapshots.
- `include/hpactor/ai/ai_type_tags.hpp` — `inline constexpr TypeTag` constants
  for 0x80–0x89 + MessageTraits specializations for AI proto messages.
- `protos/hpactor/ai_resource.proto` — Lease, lifecycle, probe snapshot,
  inventory snapshot, and node resource summary messages.
- `src/ai/accelerator_types.cpp` — `to_string()` implementations for all AI
  enums, `to_failure_reason()` mapping, selector/quantity validation.
- `src/ai/ai_message_registry.cpp` — Self-registering ProtoTypeRegistry
  registrar for AI message types.
- TypeTag extension mechanism: core changes to `type_tag.hpp`,
  `proto_type_registry.hpp`, and `proto_type_registry.cpp` that enable
  subsystems to self-register without modifying core files.
- Unit tests covering enum conversion, quantity arithmetic, selector
  validation, reason-code mapping, config defaults, TypeTag values, and
  MessageTraits wiring.

### Out of scope (deferred)

- `ENABLE_AI_ACCELERATORS` CMake option — lands in #159.
- TOML configuration parser — lands in #159.
- `ResourceLedger`, `DeviceProbe`, `AcceleratorResourceActor`,
  `DeviceProbeActor` — land in #160–#164.
- Integration or actor tests — unit tests only.
- MLX, Metal, CUDA, ROCm, or any vendor SDK dependency.

## 3. Design Constraints

- C++20, no exceptions, no RTTI, no `dynamic_cast`/`typeid`.
- No public headers require MLX, Metal, CUDA, ROCm, or vendor SDK headers.
- Enums use explicit numeric values; never renumber.
- Protobuf fields use explicit field numbers; never renumber.
- String conversion returns `const char*` (never throws, never returns nullptr).
- All validation helpers are `noexcept`.
- Quantity arithmetic is saturating (add) or error-returning (subtract underflow).
- Preserve source compatibility with existing non-AI actor APIs.

## 4. File Layout

```
New files:
  include/hpactor/ai/accelerator_types.hpp
  include/hpactor/ai/accelerator_config.hpp
  include/hpactor/ai/node_resource_summary.hpp
  include/hpactor/ai/ai_type_tags.hpp
  protos/hpactor/ai_resource.proto
  src/ai/accelerator_types.cpp
  src/ai/ai_message_registry.cpp
  tests/unit/ai/CMakeLists.txt
  tests/unit/ai/test_accelerator_types.cpp

Modified files:
  include/hpactor/msg/type_tag.hpp                # Subsystem range comment + make_subsystem_tag()
  include/hpactor/core/proto_type_registry.hpp    # SubsystemRegistrar + register_subsystem()
  src/core/proto_type_registry.cpp                # Registrar chain + wire into register_system_types()
  cmake/dependencies.cmake                        # Add ai_resource.proto to codegen
  src/CMakeLists.txt                              # Add ai/ sources
  tests/unit/CMakeLists.txt                       # Add ai/ subdirectory
```

## 5. C++ Type Model

### 5.1 Type Aliases

```cpp
namespace hpactor::ai {

using DeviceLocalId = uint64_t;   // 0 = invalid sentinel
using LeaseId = uint64_t;         // Monotonic, never reused within one process
using ProbeEpoch = uint64_t;      // From probe snapshots
using LedgerEpoch = uint64_t;     // Increments on every inventory or lease mutation

} // namespace hpactor::ai
```

### 5.2 Enums

All enums use `: uint8_t` unless noted. Every value is explicit. Every enum has a
`const char* to_string(Enum) noexcept;` free function declared in the header and
implemented in `src/ai/accelerator_types.cpp`.

#### DeviceKind
```cpp
enum class DeviceKind : uint8_t {
    Cpu = 0,
    Gpu = 1,
    Npu = 2,
    Accelerator = 3,
    Mock = 4,
};
```

#### DeviceVendor
```cpp
enum class DeviceVendor : uint8_t {
    Unknown = 0,
    Nvidia = 1,
    Amd = 2,
    Apple = 3,
    Intel = 4,
    Mock = 5,
};
```

#### DeviceHealth
```cpp
enum class DeviceHealth : uint8_t {
    Unknown = 0,
    Healthy = 1,
    Degraded = 2,
    Unavailable = 3,
    Lost = 4,
};
```

Health semantics (enforced by ResourceLedger in #160, documented here):

| State | New Leases | Existing Leases | Placement Summary |
|-------|------------|-----------------|-------------------|
| `Unknown` | Reject unless config permits | Continue | Not ready |
| `Healthy` | Allowed | Continue | Available |
| `Degraded` | Avoided unless `allow_degraded` | Continue | Degraded |
| `Unavailable` | Rejected | Drain/revoke by policy | Unavailable |
| `Lost` | Rejected | Revoked | Lost |

#### LeaseState
```cpp
enum class LeaseState : uint8_t {
    Pending = 0,
    Granted = 1,
    Active = 2,
    Releasing = 3,
    Released = 4,
    Revoked = 5,
    Expired = 6,
};
```

Valid transitions (enforced by ResourceLedger in #160):
```
Pending → Granted
Granted → Active | Released | Expired | Revoked
Active  → Released | Expired | Revoked
Revoked → Released
Expired → Released
```

#### DeviceSelectorKind
```cpp
enum class DeviceSelectorKind : uint8_t {
    Any = 0,
    ExactDevice = 1,
    Kind = 2,
    Vendor = 3,
    LabelMatch = 4,
};
```

#### ResourceAdmissionReason (`: uint16_t` — larger value space)
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
```

Mapping to canonical `FailureReason`:
```cpp
constexpr FailureReason to_failure_reason(ResourceAdmissionReason r) noexcept;
```

| Resource Reason | FailureReason |
|-----------------|---------------|
| `ResourceActorNotReady` | `ActorNotReady` |
| `InvalidLeaseRequest` | `RejectedByPolicy` |
| `NoMatchingDevice` | `NoRoute` |
| `DeviceUnhealthy` | `MemoryPressure` |
| `Insufficient*` | `MemoryPressure` |
| `ExclusiveLeaseConflict` | `RejectedByPolicy` |
| `NodeDraining` | `Draining` |
| `LeaseNotFound` | `NoRoute` |
| `LeaseOwnerMismatch` | `RejectedByPolicy` |
| `LeaseExpired` | `Expired` |
| `DeviceLost` | `NodeUnavailable` |
| `ProbeSnapshotInvalid` | `RejectedByPolicy` |

#### AdmissionPolicyKind
```cpp
enum class AdmissionPolicyKind : uint8_t {
    FirstFit = 0,
    MostFreeMemory = 1,
    ExactDevice = 2,
    CpuFallback = 3,
};
```

### 5.3 Structs

#### ResourceQuantities
```cpp
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
};
```

Public methods:
- `result<ResourceQuantities> add(const ResourceQuantities&) const noexcept` — saturating addition.
- `result<void> subtract(const ResourceQuantities&) const noexcept` — returns error when any field would underflow.
- `bool fits_within(const ResourceQuantities& available) const noexcept` — checks all non-zero requested fields against available. `exclusive_device` is checked separately.

First-milestone enforcement: `device_memory_bytes`, `host_memory_bytes`, `compute_units`, `stream_slots`, `exclusive_device`. Remaining fields are accepted and carried through summaries but default to zero.

#### DeviceId
```cpp
struct DeviceId {
    DeviceLocalId node_local_id{0};
    DeviceKind kind{DeviceKind::Cpu};

    bool operator==(const DeviceId&) const noexcept = default;
};
```

`node_local_id = 0` is invalid. Ids are assigned from a deterministic sort of probe descriptors (kind → backend name → backend ordinal → stable key → name).

#### DeviceLabel
```cpp
struct DeviceLabel {
    std::string key;
    std::string value;
};
```

#### DeviceDescriptor
```cpp
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
    uint64_t capabilities{0};  // DeviceCapabilityFlags bitmask
    DeviceHealth health{DeviceHealth::Unknown};
    std::vector<DeviceLabel> labels;
};
```

Rules:
- `stable_key` is required for mock and real accelerator probes. CPU probe uses `"cpu:host"`.
- `allocatable` ≤ `total`. `allocatable` is the admission budget.
- Descriptor snapshots are immutable after publication.

#### DeviceSelector
```cpp
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
```

#### DeviceLeaseRequest
```cpp
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
```

#### DeviceLease
```cpp
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
```

### 5.4 Config Structs (`accelerator_config.hpp`)

```cpp
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
```

### 5.5 Summary Structs (`node_resource_summary.hpp`)

```cpp
struct DeviceCapacitySummary {
    DeviceId device_id;
    DeviceKind kind;
    DeviceVendor vendor;
    std::string backend;
    DeviceHealth health;
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
```

### 5.6 Validation Helpers

Free functions in `accelerator_types.hpp`, implemented in `accelerator_types.cpp`:

```cpp
/// DeviceLocalId 0 is invalid.
bool is_valid_device_local_id(DeviceLocalId id) noexcept;

/// Check whether ttl falls within [min_lease_ttl_ms, max_lease_ttl_ms].
bool is_valid_lease_ttl(std::chrono::milliseconds ttl,
                        const AcceleratorConfig& cfg) noexcept;

/// Validate a DeviceSelector. Unknown selector kind → false.
/// ExactDevice with node_local_id=0 → false.
bool is_valid_selector(const DeviceSelector& sel) noexcept;

/// Map ResourceAdmissionReason to canonical FailureReason.
constexpr FailureReason to_failure_reason(ResourceAdmissionReason reason) noexcept;

/// Human-readable snake_case string for ResourceAdmissionReason (e.g.
/// "insufficient_device_memory"). Never returns nullptr.
const char* to_reason_string(ResourceAdmissionReason reason) noexcept;
```

## 6. Protobuf Contract

### 6.1 `protos/hpactor/ai_resource.proto`

```proto
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

### 6.2 Design Decisions

- **Enums on the wire are `uint32` fields, not proto enums.** Proto enums default
  to 0 on unrecognized values, silently masking corruption. Explicit `uint32`
  fields force the receiver to validate and reject unknown values.
- **Labels encoded as parallel `repeated string` arrays** (`label_keys[i]` ↔
  `label_values[i]`) rather than a repeated sub-message. Avoids adding a
  `PbDeviceLabel` message type that carries no independent semantic weight.
- **`PbDeviceLeaseReply` uses a flat `granted` bool** + union of grant/reject
  fields rather than `oneof`. Simpler C++ consumption; the `granted` flag
  disambiguates the interpretation.
- **`PbResourceSnapshotReply` does not embed leases** in this milestone.
  Lease serialization lands in #160 with the ResourceLedger.
- **Field numbers are explicit and stable.** Never renumber.

### 6.3 TypeTag Assignments

| Constant | Value | Message |
|----------|-------|---------|
| `kAiLeaseRequestTag` | `0x80` | `PbDeviceLeaseRequest` |
| `kAiLeaseReplyTag` | `0x81` | `PbDeviceLeaseReply` |
| `kAiLeaseActivateTag` | `0x82` | `PbDeviceLeaseActivate` |
| `kAiLeaseRenewTag` | `0x83` | `PbDeviceLeaseRenew` |
| `kAiLeaseReleaseTag` | `0x84` | `PbDeviceLeaseRelease` |
| `kAiLeaseRevokedTag` | `0x85` | `PbDeviceLeaseRevoked` |
| `kAiResourceSnapshotRequestTag` | `0x86` | `PbResourceSnapshotRequest` |
| `kAiResourceSnapshotReplyTag` | `0x87` | `PbResourceSnapshotReply` |
| `kAiDeviceSnapshotUpdateTag` | `0x88` | `PbDeviceSnapshotUpdate` |
| `kAiNodeResourceSummaryTag` | `0x89` | `PbNodeResourceSummary` |

## 7. TypeTag Extension Mechanism

### 7.1 Problem

The existing pattern requires every subsystem that adds system-range message
types to edit three core framework files:

1. `include/hpactor/msg/type_tag.hpp` — add enum values to the central `TypeTag` enum.
2. `include/hpactor/core/proto_type_registry.hpp` — add forward declarations + `HPACTOR_SYSTEM_MESSAGE` specializations.
3. `src/core/proto_type_registry.cpp` — add `register_type()` calls in `register_system_types()`.

This does not scale. The AI subsystem is the first of potentially many subsystems
(distributed training, tensor cache, model placement, security) that need system-
range TypeTags.

### 7.2 Solution

Three one-time core changes that enable subsystems to self-register:

#### 7.2.1 `type_tag.hpp` — Subsystem Extension Range

Add a comment block reserving 0x80–0xFF for subsystem extension via `inline
constexpr TypeTag` variables (not new enum members), plus a `consteval` helper:

```cpp
// After BackpressureSignalTag = 0x70:

    // ── Subsystem extension range (0x80–0xFF) ──────────────────────────────
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

    User = 0x00001000,
};

/// Construct a TypeTag from a subsystem-range value (0x80–0xFF).
consteval TypeTag make_subsystem_tag(uint32_t value) {
    return static_cast<TypeTag>(value);
}
```

The `consteval` function ensures tag construction is always compile-time and
the value is visible in debug symbols.

#### 7.2.2 `proto_type_registry.hpp` — Subsystem Registrar Chain

Add a function-pointer registrar chain and a static registration method:

```cpp
class ProtoTypeRegistry {
  public:
    /// Function pointer type for subsystem message registration.
    using SubsystemRegistrar = void (*)(ProtoTypeRegistry&);

    /// Register a subsystem's message types.
    /// Thread-safe. May be called during static initialization.
    /// Order between subsystems is unspecified.
    static void register_subsystem(SubsystemRegistrar registrar);

    /// Register core system types, then call all subsystem registrars.
    void register_system_types();

    // ... existing API unchanged ...
};
```

#### 7.2.3 `proto_type_registry.cpp` — Registrar Chain Implementation

```cpp
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

void ProtoTypeRegistry::register_system_types() {
    // ── Core system types (0x00–0x7F) ──────────────────────────────
    register_type<DownMessage>(TypeTag::DownMsg, "hpactor.DownMessage");
    // ... existing core registrations unchanged ...

    // ── Subsystem types ────────────────────────────────────────────
    for (auto fn : subsystem_registrars()) {
        fn(*this);
    }
}
```

#### 7.2.4 Subsystem Pattern (`ai_type_tags.hpp` + `ai_message_registry.cpp`)

Each subsystem creates two files:

**`include/hpactor/<subsys>/<subsys>_type_tags.hpp`** — tag constants + MessageTraits:
```cpp
#pragma once
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/core/proto_type_registry.hpp>

namespace hpactor::ai {

inline constexpr TypeTag kAiLeaseRequestTag = make_subsystem_tag(0x80);
inline constexpr TypeTag kAiLeaseReplyTag   = make_subsystem_tag(0x81);
// ... through 0x89 ...

} // namespace hpactor::ai

// MessageTraits specializations
namespace hpactor {
class PbDeviceLeaseRequest;
template <> struct MessageTraits<PbDeviceLeaseRequest> {
    static constexpr TypeTag tag() { return ai::kAiLeaseRequestTag; }
};
// ... repeated for each AI proto message ...
} // namespace hpactor
```

**`src/<subsys>/<subsys>_message_registry.cpp`** — self-registering registrar:
```cpp
#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/ai/ai_type_tags.hpp>
#include <hpactor/ai_resource.pb.h>

namespace hpactor::ai {
namespace {

void register_ai_message_types(ProtoTypeRegistry& reg) {
    reg.register_type<PbDeviceLeaseRequest>(kAiLeaseRequestTag,
        "hpactor.PbDeviceLeaseRequest");
    // ... all 10 AI messages ...
}

const bool kAiTypesRegistered = [] {
    ProtoTypeRegistry::register_subsystem(&register_ai_message_types);
    return true;
}();

} // namespace
} // namespace hpactor::ai
```

### 7.3 Outcome

After these one-time core changes, adding a new subsystem (e.g., distributed
training occupying 0x90–0x9F) requires zero modifications to `type_tag.hpp`,
`proto_type_registry.hpp`, or `proto_type_registry.cpp`. Each subsystem owns its
tag constants, MessageTraits specializations, and registrar function in its own
files.

## 8. Build Wiring

### `cmake/dependencies.cmake`

Add to the `PROTOBUF_GENERATE_CPP` call:
```cmake
${CMAKE_SOURCE_DIR}/protos/hpactor/ai_resource.proto
```

### `src/CMakeLists.txt`

Add to the `hpactor_lib` source list:
```cmake
ai/accelerator_types.cpp
ai/ai_message_registry.cpp
```

### `tests/unit/CMakeLists.txt`

Add:
```cmake
add_subdirectory(ai)
```

### `tests/unit/ai/CMakeLists.txt`

```cmake
add_executable(test_accelerator_types test_accelerator_types.cpp)
target_link_libraries(test_accelerator_types PRIVATE hpactor GTest::gtest_main)
gtest_discover_tests(test_accelerator_types)
```

## 9. Test Plan

Single test binary: `tests/unit/ai/test_accelerator_types.cpp`

| Test Suite | What It Verifies |
|-----------|-----------------|
| `DeviceKindString` | `to_string()` round-trips all 5 values |
| `DeviceVendorString` | `to_string()` round-trips all 6 values |
| `DeviceHealthString` | `to_string()` round-trips all 5 values |
| `LeaseStateString` | `to_string()` round-trips all 7 values |
| `DeviceSelectorKindString` | `to_string()` round-trips all 5 values |
| `ResourceAdmissionReasonString` | `to_string()` + `to_failure_reason()` mapping for all 18 values |
| `AdmissionPolicyKindString` | `to_string()` round-trips all 4 values |
| `ResourceQuantitiesArithmetic` | `fits_within()` — empty request fits, exact match fits, partial fits, each field individually exceeds, zero-field request semantics |
| `ResourceQuantitiesAdd` | Saturating add on each field, normal add |
| `ResourceQuantitiesSubtract` | Normal subtract, underflow on each individual field returns error |
| `ExclusiveDeviceSemantics` | `exclusive_device` set → `fits_within` checks, conflict detection |
| `DeviceSelectorValidation` | `is_valid_selector()` — ExactDevice with non-zero id = valid, ExactDevice with zero id = invalid, Kind/Vendor/LabelMatch/Any combos, unknown kind = invalid |
| `DeviceIdValidation` | `is_valid_device_local_id(0) == false`, non-zero = valid |
| `AcceleratorConfigDefaults` | Default-constructed: `enabled == false`, `lease_ttl_ms == 30000`, `min_lease_ttl_ms == 1000`, `max_lease_ttl_ms == 300000`, `admission_policy == MostFreeMemory`, `mock_devices.empty()` |
| `NodeResourceSummaryDefaults` | Default summary: `ready == false`, `draining == false`, `devices.empty()`, `ledger_epoch == 0` |
| `TypeTagValues` | All 10 AI tags are unique, in range `[0x80, 0x89]` |
| `MessageTraitsTags` | `MessageTraits<PbDeviceLeaseRequest>::tag() == kAiLeaseRequestTag`, etc. for all 10 messages |

## 10. Acceptance Criteria

- [ ] `include/hpactor/ai/accelerator_types.hpp` compiles without MLX, Metal, CUDA, ROCm, exception, or RTTI dependencies.
- [ ] `include/hpactor/ai/accelerator_config.hpp` compiles standalone.
- [ ] `include/hpactor/ai/node_resource_summary.hpp` compiles standalone.
- [ ] `include/hpactor/ai/ai_type_tags.hpp` defines all 10 `inline constexpr TypeTag` constants.
- [ ] `protos/hpactor/ai_resource.proto` generates `.pb.h` and `.pb.cc` successfully.
- [ ] TypeTag extension mechanism is implemented (3 core files modified once; future subsystems never touch them).
- [ ] `ProtoTypeRegistry::register_system_types()` calls AI subsystem registrar.
- [ ] All enum `to_string()` functions return non-null `const char*` for every defined value.
- [ ] `to_failure_reason()` maps all 18 `ResourceAdmissionReason` values.
- [ ] `ResourceQuantities::fits_within()`, `add()`, `subtract()` behave as specified.
- [ ] `is_valid_selector()` validates all selector kinds.
- [ ] All unit tests pass.
- [ ] Existing non-AI tests continue to pass (no regression).
- [ ] No public header exposes `toml++`, protobuf internals, or vendor SDK types.
