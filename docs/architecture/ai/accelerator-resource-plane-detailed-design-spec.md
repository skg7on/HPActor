# AI-ACC-001 Accelerator Resource Plane Detailed Design Spec

**Status:** Detailed design spec; implementation not started
**Requirement ID:** AI-ACC-001
**Source Architecture Goal:** [Accelerator Resource Plane Design](accelerator-resource-plane-design.md)
**Parent Architecture:** [Distributed AI Model Inference and Training Architecture](distributed-ai-model-inference-training-architecture.md)
**Related Requirements:** [AI-ACC-002](accelerator-observability-telemetry-design.md), [AI-MLX-002](mlx-device-probe-unified-memory-design.md), [AI-DIST-001](model-placement-coordinator-design.md), [AI-DIST-002](model-shard-group-readiness-stale-routes-design.md), [AI-TRN-001](training-job-worker-group-lifecycle-design.md), [AI-TRN-002](training-rank-rendezvous-checkpoint-design.md), [AI-OPS-001](ai-admin-cli-operations-design.md), [AI-TST-001](ai-fault-injection-chaos-testing-design.md)

## 1. Purpose

This document expands `AI-ACC-001` into an implementation-ready design spec for
HPActor's node-local accelerator resource plane. The source architecture design
defines the goal: discover local devices, reserve resources before model or
training allocation, keep vendor APIs out of HPActor public core, and expose the
resource state through placement and operations surfaces.

This detailed spec defines the concrete runtime contracts, file layout, C++
types, protobuf messages, actor flows, failure semantics, configuration,
testing seams, and acceptance evidence required to implement the first
milestone. It remains a design document only. It must not be described as
implemented behavior until the corresponding code and tests exist.

The first implementation target is CPU-only and mock accelerators so that the
AI runtime can be built and tested deterministically on CI and developer
machines without GPU hardware. The first real accelerator integration remains
the MLX/Metal probe described by `AI-MLX-002`.

## 2. Design Summary

The resource plane is a node-local, actor-owned inventory and reservation
ledger. `AcceleratorResourceActor` owns all mutable resource state. Probe
components publish immutable snapshots. Model replicas, training ranks, and
future tensor cache actors request leases before allocating backend resources.
The actor returns explicit grant, reject, revoke, renew, and release messages
with stable reason codes.

The recommended first implementation is deliberately local:

- no cluster-wide scheduler
- no vendor SDK dependency in public HPActor headers
- no direct kernel, stream, or allocator management
- no durable lease recovery across process restart
- deterministic CPU and mock-device behavior

Cluster placement later consumes `NodeResourceSummary` snapshots from this
actor. AI telemetry later consumes the same inventory and lease state but owns
high-frequency pressure and runtime memory export through `AI-ACC-002`.

## 3. Required Outcomes

AI-ACC-001 is complete when HPActor can:

1. Start an optional node-local `AcceleratorResourceActor`.
2. Report CPU and configured mock accelerator inventory.
3. Grant, reject, activate, renew, release, expire, and revoke device leases.
4. Enforce device memory, host memory, compute units, stream slots, and
   exclusive-device reservations through one ledger API.
5. Emit stable structured rejection and revocation reasons.
6. Reconcile repeated probe snapshots without corrupting active leases.
7. Expose bounded snapshots for placement, CLI/admin, metrics, logs, and
   health/readiness.
8. Configure the subsystem through a self-registering TOML parser using
   `TomlTableView`, not public `toml++` APIs.
9. Preserve source compatibility for existing non-AI actor APIs.
10. Pass deterministic unit and integration tests using CPU and mock devices.

## 4. Non-Goals And Explicit Deferrals

The first milestone does not implement:

- CUDA, ROCm, DCGM, NVML, Metal, or MLX vendor probes in HPActor core.
- Model loading, inference execution, tensor handle ownership, or training
  collectives.
- Cluster-wide resource allocation or gang scheduling.
- Lease durability across process restart.
- Priority preemption.
- Per-request, per-token, or per-tenant device metrics.
- Automatic remediation such as killing actors or clearing backend caches.
- Transparent migration of live device memory.

Deferred designs must still be anticipated by stable protocol fields:

- placement epoch and node endpoint can wrap a node-local lease later
- lease `priority` exists before preemption is implemented
- `tenant_id` and `workload_id` exist before quota enforcement is implemented
- `topology_group` and labels exist before topology-aware placement is
  implemented

## 5. Constraints

HPActor-wide constraints apply:

- C++20.
- No exceptions in HPActor public core APIs.
- No RTTI, `dynamic_cast`, or `typeid`.
- No public headers that require MLX, Metal, CUDA, ROCm, DCGM, or vendor SDK
  headers.
- Blocking probe calls must not run on event-loop or cooperative scheduler hot
  paths.
- Actor boundaries stay explicit. Mutable resource state is owned by
  `AcceleratorResourceActor`.
- Control messages use protobuf `TypedMessage` tags.
- Config parsing uses self-registering subsystem parsers and opaque
  `TomlTableView`.
- Tests must avoid timing assumptions and use deterministic mock devices.

## 6. Proposed File Layout

Base resource-plane code should live under a new AI namespace and directory.
The base CPU/mock implementation has no vendor dependency.

Public headers:

| File | Purpose |
|------|---------|
| `include/hpactor/ai/accelerator_types.hpp` | Stable public value types: device ids, descriptors, quantities, selectors, lease ids, reason enums. |
| `include/hpactor/ai/accelerator_config.hpp` | Runtime config structs for probes, mock devices, budgets, TTLs, and admission policy. |
| `include/hpactor/ai/device_probe.hpp` | No-throw probe interface and immutable snapshot types. |
| `include/hpactor/ai/accelerator_resource_actor.hpp` | Actor declaration and public snapshot request helpers. |
| `include/hpactor/ai/node_resource_summary.hpp` | Compact placement/admin summary model. |

Internal headers:

| File | Purpose |
|------|---------|
| `src/ai/resource_ledger.hpp` | Actor-owned ledger implementation details. |
| `src/ai/device_snapshot_reconciler.hpp` | Deterministic descriptor matching and health transition rules. |
| `src/ai/probes/cpu_device_probe.hpp` | CPU/host-memory probe. |
| `src/ai/probes/mock_device_probe.hpp` | Configured mock-device probe. |

Source files:

| File | Purpose |
|------|---------|
| `src/ai/accelerator_resource_actor.cpp` | Message handlers, lease lifecycle, snapshot replies. |
| `src/ai/resource_ledger.cpp` | Admission, reservations, expiry, release, summary generation. |
| `src/ai/device_snapshot_reconciler.cpp` | Probe snapshot reconciliation. |
| `src/ai/device_probe_actor.cpp` | Polling/daemon actor for probe execution. |
| `src/ai/probes/cpu_device_probe.cpp` | CPU probe implementation. |
| `src/ai/probes/mock_device_probe.cpp` | Mock probe implementation. |
| `src/config/parsers/ai_accelerator_config_parser.cpp` | Self-registering TOML parser. |

Protobuf:

| File | Purpose |
|------|---------|
| `protos/hpactor/ai_resource.proto` | Lease, probe, summary, and admin snapshot control messages. |

Tests:

| File | Purpose |
|------|---------|
| `tests/ai/test_accelerator_types.cpp` | Enum values, string conversion, quantity math. |
| `tests/ai/test_resource_ledger.cpp` | Admission, exclusive leases, release, expiry, summary. |
| `tests/ai/test_device_snapshot_reconciler.cpp` | Device identity matching and health transitions. |
| `tests/ai/test_mock_device_probe.cpp` | Mock snapshot generation from config. |
| `tests/ai/test_ai_accelerator_config.cpp` | TOML parser defaults, validation, mock devices. |
| `tests/ai/test_accelerator_resource_actor.cpp` | Actor message protocol with scheduler disabled or controlled. |
| `tests/ai/test_accelerator_resource_integration.cpp` | ActorSystem startup, lease request, release, shutdown behavior. |

Build integration:

- Add `ENABLE_AI_ACCELERATORS` CMake option, default `ON`.
- Runtime stays disabled unless `[system.ai.accelerators]` enables it.
- Add `HPACTOR_ENABLE_AI_ACCELERATORS` to `hpactor_config.hpp.in`.
- MLX and other real vendor probes use separate future build flags.

## 7. Component Ownership

| Component | Owns | Does Not Own |
|-----------|------|--------------|
| `AcceleratorResourceActor` | inventory, health state, leases, ledger epochs, summaries, admission decisions | vendor SDK calls, model memory, runtime telemetry sampling, cluster placement |
| `ResourceLedger` | deterministic capacity accounting inside the actor | locks shared with other actors, external mutation |
| `DeviceProbeActor` | scheduling probe calls and sending snapshot updates | lease state, placement, admin policy |
| `DeviceProbe` implementations | building immutable snapshots for one backend | actor state, admission decisions |
| `DeviceTelemetryActor` from AI-ACC-002 | pressure derivation and metric export snapshots | lease mutation |
| `ModelPlacementCoordinator` from AI-DIST-001 | distributed placement plans and epochs | node-local capacity mutation |

The important invariant is simple: only `AcceleratorResourceActor` mutates the
ledger. Everything else sends messages or reads immutable snapshots.

## 8. Data Model

### 8.1 Device Identity

Device ids are node-local. They are stable within one process generation but
must be rebuilt after restart from fresh probe snapshots.

```cpp
namespace hpactor::ai {

using DeviceLocalId = uint64_t;
using LeaseId = uint64_t;
using ProbeEpoch = uint64_t;
using LedgerEpoch = uint64_t;

enum class DeviceKind : uint8_t {
    Cpu = 0,
    Gpu = 1,
    Npu = 2,
    Accelerator = 3,
    Mock = 4,
};

enum class DeviceVendor : uint8_t {
    Unknown = 0,
    Nvidia = 1,
    Amd = 2,
    Apple = 3,
    Intel = 4,
    Mock = 5,
};

struct DeviceId {
    DeviceLocalId node_local_id{0};
    DeviceKind kind{DeviceKind::Cpu};
};

} // namespace hpactor::ai
```

`node_local_id = 0` is invalid. The actor assigns ids from a deterministic sort
of probe descriptors:

1. kind
2. backend name
3. backend ordinal
4. vendor uuid, PCI id, or configured mock id
5. human-readable name

Vendor-specific identifiers remain metadata, not public type dependencies.

### 8.2 Device Descriptor

```cpp
struct DeviceDescriptor {
    DeviceId id;
    DeviceKind kind{DeviceKind::Cpu};
    DeviceVendor vendor{DeviceVendor::Unknown};
    std::string backend;          // "cpu", "mock", "mlx", "cuda", ...
    std::string name;
    std::string stable_key;       // probe-owned identity key
    uint32_t backend_ordinal{0};
    uint64_t parent_node_local_id{0};
    int32_t numa_node{-1};
    std::string topology_group;
    ResourceQuantities total;
    ResourceQuantities allocatable;
    DeviceCapabilityFlags capabilities;
    DeviceHealth health{DeviceHealth::Unknown};
    std::vector<DeviceLabel> labels;
};
```

Rules:

- `stable_key` is required for mock devices and real accelerator probes.
- CPU probe may use a deterministic key such as `cpu:host`.
- Label keys and values are bounded strings.
- Descriptor snapshots are immutable after publication.
- `allocatable` can be lower than `total` and is the only capacity used for
  admission.

### 8.3 Resource Quantities

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

Quantity arithmetic is saturating and no-throw:

- addition returns `result<ResourceQuantities>`
- subtraction returns an error when any field would underflow
- `fits_within(requested, available)` checks all non-zero requested fields
- `exclusive_device` is a boolean constraint, not a count

First milestone required enforcement:

- `device_memory_bytes`
- `host_memory_bytes`
- `compute_units`
- `stream_slots`
- `exclusive_device`

The remaining fields are accepted and carried through summaries but can default
to zero until later AI requirements use them.

### 8.4 Health State

```cpp
enum class DeviceHealth : uint8_t {
    Unknown = 0,
    Healthy = 1,
    Degraded = 2,
    Unavailable = 3,
    Lost = 4,
};
```

Health semantics:

| State | New Leases | Existing Leases | Placement Summary |
|-------|------------|-----------------|-------------------|
| `Unknown` | reject unless config permits startup fallback | continue | not ready |
| `Healthy` | allowed | continue | available |
| `Degraded` | avoided unless request allows degraded | continue | degraded |
| `Unavailable` | rejected | drain or revoke by policy | unavailable |
| `Lost` | rejected | revoked | lost |

### 8.5 Selectors

Selectors are intentionally simple in the first milestone.

```cpp
enum class DeviceSelectorKind : uint8_t {
    Any = 0,
    ExactDevice = 1,
    Kind = 2,
    Vendor = 3,
    LabelMatch = 4,
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
```

Selection rules:

- `ExactDevice` ignores fallback unless the exact device is CPU.
- `Kind` with `Gpu` can use CPU only when `allow_cpu_fallback` is true and
  system config also permits CPU fallback.
- label matching is exact key/value matching.
- unknown selector values reject with `InvalidLeaseRequest`.

### 8.6 Lease Request

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
    TraceContext trace;
};
```

Request validation:

- `requester` must resolve to a local actor address for the first milestone.
- `ttl` must be within configured min and max.
- at least one requested quantity or `exclusive` must be present.
- strings are truncated or rejected according to config bounds.
- `exclusive = true` also sets `requested.exclusive_device = true`.

### 8.7 Lease Record

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

State transitions:

```text
Pending -> Granted
Granted -> Active
Granted -> Released
Granted -> Expired
Granted -> Revoked
Active  -> Released
Active  -> Expired
Active  -> Revoked
Revoked -> Released
Expired -> Released
```

Illegal transitions are rejected, logged, and counted.

## 9. Stable Reason Codes

AI resource-specific reason codes are separate from the generic
`FailureReason` enum but map into it for logs, traces, and future failure
envelopes.

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

Mapping to `FailureReason`:

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

Every reject, revoke, and failed lifecycle operation includes the resource
reason. No production path should return only a free-form string.

## 10. Protobuf Contract

Add `protos/hpactor/ai_resource.proto`. It should import `common.proto` for
actor addresses and endpoints.

System `TypeTag` allocation should use the reserved system range:

| Tag | Value | Message |
|-----|-------|---------|
| `AiLeaseRequestTag` | `0x80` | `PbDeviceLeaseRequest` |
| `AiLeaseReplyTag` | `0x81` | `PbDeviceLeaseReply` |
| `AiLeaseActivateTag` | `0x82` | `PbDeviceLeaseActivate` |
| `AiLeaseRenewTag` | `0x83` | `PbDeviceLeaseRenew` |
| `AiLeaseReleaseTag` | `0x84` | `PbDeviceLeaseRelease` |
| `AiLeaseRevokedTag` | `0x85` | `PbDeviceLeaseRevoked` |
| `AiResourceSnapshotRequestTag` | `0x86` | `PbResourceSnapshotRequest` |
| `AiResourceSnapshotReplyTag` | `0x87` | `PbResourceSnapshotReply` |
| `AiDeviceSnapshotUpdateTag` | `0x88` | `PbDeviceSnapshotUpdate` |
| `AiNodeResourceSummaryTag` | `0x89` | `PbNodeResourceSummary` |

The exact protobuf should follow these principles:

- encode enum values explicitly and never renumber
- keep request/reply messages small
- use repeated fields only for bounded snapshots
- include `ledger_epoch` on replies and summaries
- include `probe_epoch` on snapshot updates
- include reason code and human-readable detail on every rejection
- do not encode raw tensors, prompts, completion text, or backend SDK data

Example shape:

```proto
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
```

`TraceContext` is already carried as a `TypedMessage` sidecar for local and
remote sends. The protobuf does not need to duplicate it in the first
milestone.

## 11. Resource Ledger Contract

`ResourceLedger` is an internal helper owned by `AcceleratorResourceActor`.
It is not thread-safe by design because only the actor mutates it.

Required API:

```cpp
class ResourceLedger {
  public:
    result<void> reconcile_snapshot(const DeviceSnapshot& snapshot,
                                    const ReconcilePolicy& policy) noexcept;

    LeaseDecision try_grant(const DeviceLeaseRequest& request,
                            AdmissionPolicyKind policy,
                            Clock& clock) noexcept;

    result<void> activate(LeaseId lease_id, const ActorAddress& owner,
                          Clock& clock) noexcept;

    result<void> renew(LeaseId lease_id, const ActorAddress& owner,
                       std::chrono::milliseconds ttl, Clock& clock) noexcept;

    ReleaseDecision release(LeaseId lease_id,
                            const ActorAddress& owner) noexcept;

    std::vector<DeviceLeaseRevocation>
    mark_device_health(DeviceId id, DeviceHealth health,
                       ResourceAdmissionReason reason) noexcept;

    std::vector<DeviceLeaseExpiration>
    expire_due_leases(int64_t now_ns) noexcept;

    NodeResourceSummary summary() const;
    ResourceInventorySnapshot snapshot(uint32_t limit) const;
};
```

Ledger invariants:

- `reserved <= allocatable` for every numeric quantity on every device.
- one active exclusive lease prevents any other lease on that device.
- any existing lease prevents a new exclusive lease on that device.
- released, revoked, and expired leases do not contribute to reserved totals.
- lease ids are monotonically increasing and never reused within one process.
- ledger epoch increments for every inventory or lease mutation.
- all public snapshots are copies, not references to ledger internals.

Admission algorithm:

1. Reject when node is draining or the resource actor is not ready.
2. Validate request fields and TTL.
3. Build candidate device list from selector and CPU fallback rules.
4. Filter by health and degraded-device policy.
5. Filter by exclusive conflict.
6. Filter by quantity fit.
7. Score candidates by configured policy.
8. Reserve quantities on the selected device.
9. Return a granted lease with `LeaseState::Granted`.

Initial policies:

| Policy | Behavior |
|--------|----------|
| `FirstFit` | deterministic sorted candidate order |
| `MostFreeMemory` | largest available `device_memory_bytes`, then `host_memory_bytes`, then lowest id |
| `ExactDevice` | exact selector only |
| `CpuFallback` | accelerator first, CPU fallback when allowed |

The admission result must include a selected-device explanation field for logs
and tests.

## 12. Probe Contract

### 12.1 Interface

```cpp
class DeviceProbe {
  public:
    virtual ~DeviceProbe() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual result<DeviceSnapshot> snapshot() noexcept = 0;
};
```

`DeviceSnapshot` contains:

- probe name
- probe epoch
- generated timestamp
- descriptors
- source health
- optional error reason

`snapshot()` must not throw. Vendor probe translation units may catch
dependency-specific exceptions internally if a vendor library requires them,
but no exception crosses the HPActor interface.

### 12.2 CPU Probe

`CpuDeviceProbe` always exists when the resource plane is compiled.

Required behavior:

- reports one CPU descriptor
- `kind = Cpu`
- `backend = "cpu"`
- stable key `cpu:host`
- host memory budget from config when set
- otherwise conservative host memory estimate when platform-safe
- compute units from configured value or hardware concurrency
- health `Healthy` unless required platform information is unavailable

The CPU probe should not block on expensive system calls.

### 12.3 Mock Probe

`MockDeviceProbe` is configured entirely from TOML and tests.

Required behavior:

- creates deterministic descriptors in config order
- supports mock CPU, GPU, NPU, and accelerator devices
- supports health overrides
- supports labels and topology groups
- supports fault-injection hooks later without changing the probe interface

Mock probe is the default integration test backend.

### 12.4 Probe Actor

Probe calls run in `DeviceProbeActor`, a `PollingActor` or `DaemonActor`.

Lifecycle:

1. start after logging, metrics, tracing, and resource actor construction
2. run an immediate startup probe
3. send `DeviceSnapshotUpdate` to `AcceleratorResourceActor`
4. sleep or poll according to `probe_interval_ms`
5. coalesce repeated identical snapshots
6. stop during shutdown after sending no further updates

The probe actor must not mutate the ledger. It only sends snapshots.

### 12.5 Snapshot Reconciliation

Reconciliation rules:

1. Match existing devices by `stable_key`.
2. Preserve `DeviceId` for matched devices.
3. Assign new `DeviceId` values for new devices.
4. Mark new devices `Unknown` until the next healthy snapshot unless config
   allows immediate use.
5. Mark missing devices `Degraded` during `missing_device_grace_ms`.
6. Mark missing devices `Lost` after grace expires.
7. Recompute available capacity after preserving active lease reservations.
8. If new allocatable capacity is below current reservations, mark the device
   `Degraded`, reject new leases, and emit a reconciliation conflict.
9. If health becomes `Lost`, revoke active leases.

Reconciliation is idempotent by `(probe_name, probe_epoch)`. Older epochs from
the same probe are ignored.

## 13. Actor Behavior

### 13.1 Startup

Startup states:

```cpp
enum class ResourceActorState : uint8_t {
    Disabled,
    Starting,
    Ready,
    Degraded,
    Draining,
    Stopped,
};
```

Startup flow:

1. `ActorSystem` checks `Config::ai_accelerators.enabled`.
2. If disabled, no AI resource actors are spawned.
3. If enabled, spawn `AcceleratorResourceActor` as a system actor.
4. Spawn `DeviceProbeActor` when at least one probe is configured.
5. Resource actor enters `Starting`.
6. First valid CPU or mock snapshot transitions it to `Ready`.
7. If required probes fail and `allow_empty_inventory = false`, transition to
   `Degraded` and reject lease requests.
8. If `allow_empty_inventory = true`, transition to `Ready` with empty
   accelerator inventory and CPU-only behavior if configured.

Readiness:

- node readiness must be false when AI accelerators are enabled,
  `require_resource_plane_ready = true`, and resource actor is not `Ready`.
- non-AI traffic readiness is unaffected unless a deployment explicitly ties it
  to AI readiness.

### 13.2 Lease Grant

Request flow:

1. owner sends `PbDeviceLeaseRequest`
2. actor validates message
3. actor calls `ResourceLedger::try_grant`
4. granted reply includes lease id, device id, ledger epoch, and expiry
5. rejected reply includes stable reason and detail
6. actor emits metric, structured log, and trace attributes

Requesters must not allocate model weights, KV cache, training buffers, or
backend runtime memory before a lease grant when the deployment requires the
resource plane.

### 13.3 Activation

Activation flow:

1. owner receives a grant
2. owner allocates backend resources
3. owner sends `PbDeviceLeaseActivate`
4. resource actor transitions `Granted -> Active`

If backend allocation fails, owner sends `PbDeviceLeaseRelease` with a failure
reason. The resource actor records the failed activation and releases capacity.

Activation is idempotent:

- activating an already active lease returns success
- activating a released/revoked/expired lease returns the current terminal state

### 13.4 Renewal And Expiry

Leases have TTLs to prevent leaked reservations.

Rules:

- default TTL is `lease_ttl_ms`
- renewal must arrive before `expires_at_ns`
- renewal extends expiry from the actor's current clock, not from the previous
  expiry
- expired leases release capacity immediately
- owners renewing an expired lease receive `LeaseExpired`

Implementation note:

- The first implementation can check expiry on incoming messages and with a
  scheduler timer.
- Tests should use a controllable clock or direct ledger calls rather than
  relying on sleeps.

### 13.5 Release

Release is best-effort and idempotent:

- owner releases by lease id
- owner mismatch rejects with `LeaseOwnerMismatch`
- unknown lease returns `LeaseNotFound`
- already terminal leases return success with their current state
- release frees reserved capacity and increments ledger epoch

### 13.6 Revocation

Revocation can be triggered by:

- device becomes `Lost`
- device becomes `Unavailable` and policy requires drain
- admin drain command
- node shutdown
- future priority preemption

Flow:

1. lease transitions to `Revoked`
2. capacity is released or marked pending according to policy
3. owner receives `PbDeviceLeaseRevoked`
4. owner drains, fails, or reacquires
5. owner acknowledges by sending release or activation failure
6. timeout completes cleanup when owner does not respond

First milestone revocation can release capacity immediately because the resource
plane is admission control, not backend memory ownership. Owners still must
handle the revoke signal and unload backend resources.

### 13.7 Owner Death

The resource actor should monitor or link lease owners when possible.

When owner death is observed:

- active and granted leases for the owner are released
- reason `LeaseOwnerDied` can be logged as detail while the stable admission
  reason remains `LeaseExpired` or `RejectedByPolicy` until a dedicated enum is
  added
- summary and metrics update

If monitoring is not available for a path, TTL expiry is the fallback cleanup
mechanism.

### 13.8 Shutdown

Shutdown behavior:

1. new lease requests reject with `NodeDraining`
2. existing owners are expected to release while draining
3. resource actor revokes remaining leases after shutdown drain timeout
4. probe actor stops sending updates
5. final summary is emitted before telemetry flush when logging/metrics are
   enabled

The resource actor is a system actor and should drain after user AI actors.

## 14. Configuration

Add `ai::AcceleratorConfig` to `hpactor::Config` and `SystemDef`.

```cpp
namespace hpactor::ai {

enum class AdmissionPolicyKind : uint8_t {
    FirstFit,
    MostFreeMemory,
    ExactDevice,
    CpuFallback,
};

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

TOML:

```toml
[system.ai.accelerators]
enabled = true
enable_cpu_probe = true
probe_interval_ms = 1000
lease_ttl_ms = 30000
min_lease_ttl_ms = 1000
max_lease_ttl_ms = 300000
admission_policy = "most_free_memory"
allow_cpu_fallback = true
allow_empty_inventory = false
require_resource_plane_ready = false
missing_device_grace_ms = 5000
cpu_host_memory_mb = 8192
cpu_compute_units = 8

[[system.ai.accelerators.mock_device]]
id = "mock-gpu-0"
kind = "gpu"
vendor = "mock"
name = "Mock GPU 0"
memory_mb = 24576
host_memory_mb = 0
compute_units = 100
stream_slots = 32
exclusive_only = false
health = "healthy"
labels = { backend = "mock", precision = "fp16,bf16" }
```

Parser rules:

- parser file self-registers with `TomlSystemParserRegistration`
- public parser interfaces continue to use `TomlTableView`
- invalid enum strings return `result<void>` error
- numeric MB values convert to bytes with overflow checks
- mock device ids must be unique
- unknown keys can be ignored initially, but invalid values cannot silently
  fall back to defaults

Defaults:

- base resource plane runtime disabled
- CPU probe enabled when resource plane is enabled
- mock devices only enabled when explicitly configured
- `MostFreeMemory` admission
- 30 second lease TTL
- empty inventory not ready unless explicitly allowed

## 15. ActorSystem Integration

`ActorSystem` should gain:

- `Config::ai_accelerators`
- optional `std::shared_ptr<ai::AcceleratorResourceActor>` or actor handle
- accessor for resource actor address when enabled
- startup/shutdown wiring for resource and probe actors
- config bootstrap from TOML into `Config`

Public API sketch:

```cpp
class ActorSystem {
  public:
    Actor accelerator_resource_actor() const noexcept;
    bool ai_accelerators_enabled() const noexcept;
};
```

Integration rules:

- no existing constructor behavior changes when config is disabled
- no existing actor API return types change
- resource actor spawn failure is fatal only when
  `require_resource_plane_ready = true`
- the actor is registered by a stable system name such as
  `"system.ai.accelerators"`
- probe actor is registered as `"system.ai.accelerators.probe"`

## 16. Metrics, Logs, Traces, And Snapshots

### 16.1 Metric Events

The existing `MetricEvent` is 32 bytes and has constrained fields. The first
resource-plane implementation can use either:

1. new `MetricEventType` values with compact codes and actor id, or
2. a low-frequency metrics snapshot path consumed by the metrics subsystem.

Recommended split:

- use event types for counts: grant, reject, release, revoke, probe error
- use snapshot path for gauges: total memory, reserved memory, lease count

Proposed event types:

| Event | `code` |
|-------|--------|
| `kAiLeaseGranted` | device kind |
| `kAiLeaseRejected` | `ResourceAdmissionReason` truncated/validated |
| `kAiLeaseReleased` | lease terminal state |
| `kAiLeaseRevoked` | `ResourceAdmissionReason` |
| `kAiProbeError` | source-specific compact code |

AI-ACC-002 owns final OpenMetrics family names for detailed telemetry. AI-ACC-001
must emit enough state for those families to be derived later.

### 16.2 Structured Logs

Required log events:

- `ai_resource_actor_started`
- `ai_device_discovered`
- `ai_device_health_transition`
- `ai_device_reconciliation_conflict`
- `ai_lease_granted`
- `ai_lease_rejected`
- `ai_lease_activated`
- `ai_lease_renewed`
- `ai_lease_released`
- `ai_lease_revoked`
- `ai_probe_failed`

Fields:

- node endpoint
- actor id
- ledger epoch
- probe epoch when relevant
- device id
- device kind
- backend
- lease id
- workload id
- model or job
- tenant id when available and allowed
- stable reason code
- trace id when available

### 16.3 Trace Attributes

Lease request spans should include:

- `ai.lease.id`
- `ai.device.id`
- `ai.device.kind`
- `ai.device.vendor`
- `ai.resource.device_memory_bytes`
- `ai.resource.host_memory_bytes`
- `ai.lease.rejection_reason`
- `ai.lease.state`

Traces must not include unbounded labels, prompts, completion text, dataset
rows, or artifact paths by default.

### 16.4 Snapshots

`ResourceInventorySnapshot` and `NodeResourceSummary` are immutable copies.

Snapshot rules:

- bounded by caller-provided `limit`
- includes `generated_at_ns`
- includes `ledger_epoch`
- marks `truncated = true` when limit is hit
- includes freshness state from the latest probe epoch
- does not expose mutable internal containers

## 17. CLI And Admin Integration

AI-OPS-001 defines the eventual `AiAdminActor` facade. AI-ACC-001 should expose
message-based snapshot and mutation primitives that the facade can call later.

Initial internal operations:

| Operation | Mutating | Required Authorization Later |
|-----------|----------|------------------------------|
| list devices | no | no or read-only auth |
| show device | no | no or read-only auth |
| list leases | no | no or read-only auth |
| show lease | no | no or read-only auth |
| resource summary | no | no or read-only auth |
| drain device | yes | yes |
| force revoke lease | yes | yes |

Direct CLI commands can be deferred until `AiAdminActor` exists. The resource
actor should still provide typed messages so tests and future admin code do not
need to read actor memory.

## 18. Placement Integration

`NodeResourceSummary` is the handoff contract to AI-DIST-001.

Required fields:

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

Placement rules:

- the summary is advisory until a lease is granted
- placement must reserve leases before committing a model placement plan
- old summaries can cause stale placement attempts, but lease grant remains
  authoritative
- summary freshness should be visible to placement and operations surfaces

## 19. Concurrency And Memory Contract

Concurrency:

- `ResourceLedger` is single-owner and not thread-safe.
- probe actors communicate by message only.
- snapshot publication copies data into bounded containers.
- no blocking probe call runs on event-loop or cooperative scheduler workers.
- no shared mutable state exists between model actors and the ledger.

Memory:

- ledger containers must be bounded by config and observed inventory size.
- strings from config and requests are size-limited.
- request storms must be constrained by the resource actor mailbox capacity.
- repeated snapshots should reuse memory where local patterns allow but must not
  expose references outside the actor.
- large telemetry samples belong to AI-ACC-002 and should not be sent through
  this actor at high frequency.

Timer behavior:

- lease expiry should use HPActor scheduler timers or explicit periodic checks.
- tests should not assume timer firing within a small wall-clock interval.
- ledger expiry unit tests should inject time directly.

## 20. Security And Multi-Tenant Hooks

First milestone records `tenant_id` and `workload_id` but does not enforce
tenant quota. Enforcement belongs to AI-SEC-001 and future quota requirements.

Rules now:

- tenant and workload strings are bounded and redacted in logs when policy
  requires it
- admin mutations must route through the future policy actor before exposure
  outside tests
- no raw prompts, completions, tensors, dataset paths, or secrets appear in
  resource snapshots
- mock-device config must not be accepted from untrusted runtime input without
  normal config authorization

## 21. Testing Strategy

### 21.1 Unit Tests

Required:

- device kind/vendor/health string conversion
- quantity arithmetic and overflow/underflow behavior
- selector matching
- admission policy tie-breakers
- exclusive lease conflict
- insufficient memory and compute rejection
- TTL validation
- lease state transitions
- ledger epoch increments
- release idempotency
- snapshot summary generation
- descriptor stable-key reconciliation
- missing device grace and lost transition
- mock probe descriptor generation
- config parser defaults and validation

### 21.2 Actor Tests

Required:

- resource actor starts disabled and enabled
- startup snapshot transitions actor to ready
- lease request grant/reject replies
- activate/renew/release protocol
- revoke message delivered to owner actor
- owner death releases or expires leases
- node drain rejects new leases
- snapshot request returns bounded copy

Use scheduler disabled or deterministic worker control when intermediate actor
state must be inspected.

### 21.3 Integration Tests

Required:

- `ActorSystem` starts resource actor from TOML mock-device config
- model-like test actor acquires, activates, and releases lease
- memory exhaustion rejects second model-like actor
- exclusive lease blocks shared lease
- health update to lost revokes active leases
- shutdown drains/revokes leases deterministically

### 21.4 Stress And Reliability Tests

Required before production use:

- concurrent lease request storm
- repeated probe snapshots while leases are active
- owner death without release
- lost device during activation
- long-running renewal soak using mock devices

### 21.5 Test Data

Add TOML fixtures:

- `tests/data/toml/ai_accelerators_disabled.toml`
- `tests/data/toml/ai_accelerators_cpu.toml`
- `tests/data/toml/ai_accelerators_mock.toml`
- `tests/data/toml/ai_accelerators_invalid.toml`

## 22. Acceptance Evidence Checklist

Implementation is acceptable when all of the following evidence exists:

- CMake option and generated config macro compile in default build.
- Public AI resource headers compile without vendor SDK headers.
- `ai_resource.proto` generates and registers message tags.
- TOML parser is self-registering and covered by parser tests.
- CPU and mock probes produce deterministic snapshots.
- `ResourceLedger` unit tests cover all grant/reject/release/revoke paths.
- `AcceleratorResourceActor` actor tests cover message protocol.
- `ActorSystem` integration test starts the resource plane from config.
- Metrics/log hooks are exercised or covered with test sinks.
- Snapshot APIs return bounded immutable copies.
- Existing non-AI tests continue to pass with the subsystem disabled.
- Documentation states that real MLX/Metal probing remains in AI-MLX-002.

## 23. Implementation Phases

This is a design spec, not the final implementation plan, but the work should
land in these dependency-safe phases.

### Phase 1: Types, Config, And Protobuf

Deliver:

- `include/hpactor/ai/accelerator_types.hpp`
- `include/hpactor/ai/accelerator_config.hpp`
- `protos/hpactor/ai_resource.proto`
- TypeTag reservations
- CMake option and config macro
- parser skeleton and config tests

No actor behavior yet.

### Phase 2: Ledger And Mock/CPU Probes

Deliver:

- `ResourceLedger`
- quantity arithmetic
- selector matching
- admission policies
- `CpuDeviceProbe`
- `MockDeviceProbe`
- reconciliation helper
- unit tests

No `ActorSystem` startup wiring yet.

### Phase 3: AcceleratorResourceActor Protocol

Deliver:

- lease request/reply handlers
- activate/renew/release handlers
- snapshot request/reply handlers
- expiry handling
- structured reason mapping
- actor tests

### Phase 4: ActorSystem And TOML Integration

Deliver:

- optional system actor startup
- probe actor startup
- shutdown ordering
- health/readiness state
- integration tests with TOML fixtures

### Phase 5: Observability And Admin Hooks

Deliver:

- metric events or snapshot path
- structured logs
- trace attributes
- `NodeResourceSummary`
- admin-ready snapshot messages
- tests with memory or mock sinks

### Phase 6: MLX Probe Adapter Boundary

Deliver only the base integration seam needed by AI-MLX-002:

- probe registration hook
- backend name and descriptor normalization compatibility
- no direct MLX dependency in AI-ACC-001 base files

Actual MLX/Metal probing remains in AI-MLX-002.

## 24. Open Questions

1. Should `ENABLE_AI_ACCELERATORS` default to `ON` for base CPU/mock support, or
   should all AI resource-plane code be opt-in at compile time?
2. Should resource actor readiness affect whole-node readiness by default when
   enabled, or only when AI deployments declare accelerator dependency?
3. Should owner death cleanup use monitoring immediately, or should the first
   implementation rely on TTL and add monitoring in a follow-up?
4. Should `ResourceAdmissionReason` become part of the global
   `FailureReason` namespace later, or remain an AI-specific projection?
5. Should mock-device fault injection live in `MockDeviceProbe` or wait for
   AI-TST-001's fault injection actor?
