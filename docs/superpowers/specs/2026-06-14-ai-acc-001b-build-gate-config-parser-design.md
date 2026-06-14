# AI-ACC-001B: AI Accelerator Build Gate and TOML Configuration Parser — Design Spec

**Status:** Proposed design; implementation not started
**Requirement ID:** AI-ACC-001B
**Parent Issue:** [#159](https://github.com/skg7on/HPActor/issues/159)
**Source Spec:** [Accelerator Resource Plane Detailed Design Spec](../../architecture/ai/accelerator-resource-plane-detailed-design-spec.md) §14
**Prerequisite:** [#158](https://github.com/skg7on/HPActor/issues/158) — AI-ACC-001A Accelerator public types and protobuf contract
**Parent Backlog:** [#157](https://github.com/skg7on/HPActor/issues/157) — AI-ACC-001 Accelerator resource plane implementation backlog

## 1. Purpose

Issue #159 completes the build-plumbing and config-parsing phase of the AI
accelerator resource plane. It adds the compile-time build gate
(`ENABLE_AI_ACCELERATORS` / `HPACTOR_ENABLE_AI_ACCELERATORS`), wires
`ai::AcceleratorConfig` into both the runtime `Config` struct and the TOML-parsed
`SystemDef`, and ships a self-registering TOML subsystem parser with
comprehensive validation. After this issue lands, AI accelerator code is compiled
by default but remains runtime-disabled unless explicitly enabled in TOML config.

## 2. Scope

### In Scope

- Add `ENABLE_AI_ACCELERATORS` CMake option (default `ON`) and generated
  `HPACTOR_ENABLE_AI_ACCELERATORS` compile-time macro in
  `hpactor_config.hpp.in`.
- Add `ai::AcceleratorConfig ai_accelerators;` field to `SystemDef` in
  `topology_model.hpp`, guarded by `#if HPACTOR_ENABLE_AI_ACCELERATORS`.
- Add `ai::AcceleratorConfig ai_accelerators;` field to the runtime `Config`
  struct in `actor_system.hpp`, guarded by the same macro.
- Wire the field mapping in `ActorSystem::load_topology()` in
  `src/actor/actor_system.cpp`.
- Create `src/config/parsers/ai_accelerator_config_parser.cpp` — a
  self-registering `ITomlSystemConfigParser` that reads
  `[system.ai.accelerators]` using opaque `TomlTableView`.
- Add `src/ai/` to the `hpactor_lib` source list in `src/CMakeLists.txt` if
  not already present (AI-ACC-001A sources live there).
- Add TOML test fixtures for disabled, CPU-only, mock-device, and invalid
  configs under `tests/data/toml/ai/`.
- Add unit tests in `tests/unit/config/test_ai_accelerator_config.cpp`
  covering defaults, CPU-only config, mock devices, and rejection of invalid
  fixtures.
- Parser must validate enum strings, TTL bounds, unique mock-device ids, and
  numeric overflow; unknown keys may be ignored but invalid values must never
  silently fall back to defaults.

### Out of Scope (Deferred)

- `ResourceLedger`, `DeviceProbe`, `AcceleratorResourceActor`,
  `DeviceProbeActor` — lands in #160–#164.
- Integration or actor tests.
- MLX, Metal, CUDA, ROCm, or any vendor SDK dependency.
- Runtime behavior other than config parsing and struct population.

## 3. Design Constraints

- C++20, no exceptions, no RTTI, no `dynamic_cast`/`typeid`.
- All AI-conditional code is guarded by `#if HPACTOR_ENABLE_AI_ACCELERATORS`.
- When `ENABLE_AI_ACCELERATORS=OFF`, zero AI headers are compiled into
  `hpactor_lib`, and existing non-AI tests pass with no source changes.
- Public parser interfaces use opaque `TomlTableView`; no `toml++` types
  appear in any public header included by this work.
- The parser self-registers via `TomlSystemParserRegistration<T>` and follows
  existing parser conventions (static `kName`, `kOrder`, file-scope registrar).
- Parser validation errors must produce explicit `result<void>` failures; no
  invalid value may silently fall back to a default.
- MB-to-byte conversion must check for overflow (MB value > `UINT64_MAX / (1024*1024)`).
- Mock-device `id` fields must be unique within a single TOML document.
- Preserve source compatibility with existing non-AI actor APIs.

## 4. Proposed File Layout

```
New files:
  tests/data/toml/ai/ai_accelerators_disabled.toml
  tests/data/toml/ai/ai_accelerators_cpu.toml
  tests/data/toml/ai/ai_accelerators_mock.toml
  tests/data/toml/ai/ai_accelerators_invalid_enum.toml
  tests/data/toml/ai/ai_accelerators_invalid_ttl.toml
  tests/data/toml/ai/ai_accelerators_invalid_duplicate_id.toml
  tests/unit/config/test_ai_accelerator_config.cpp

Modified files:
  CMakeLists.txt                                    # Add ENABLE_AI_ACCELERATORS option + HPACTOR_ENABLE_AI_ACCELERATORS
  include/hpactor/hpactor_config.hpp.in             # Add #cmakedefine01 HPACTOR_ENABLE_AI_ACCELERATORS
  include/hpactor/core/actor_system.hpp             # Add ai::AcceleratorConfig to Config struct
  include/hpactor/config/topology_model.hpp         # Add ai::AcceleratorConfig to SystemDef
  src/actor/actor_system.cpp                        # Wire ai_accelerators in load_topology()
  src/config/parsers/ai_accelerator_config_parser.cpp  # NEW: self-registering parser
  src/CMakeLists.txt                                # Add config/parsers/ai_accelerator_config_parser.cpp
  tests/unit/config/CMakeLists.txt                  # Add test_ai_accelerator_config target
```

The AI public headers (`include/hpactor/ai/accelerator_types.hpp`,
`include/hpactor/ai/accelerator_config.hpp`, etc.) were already created by
AI-ACC-001A (#158) and require no modifications in this issue.

## 5. CMake Build Gate

### 5.1 CMake Option

Add to `CMakeLists.txt` alongside existing `ENABLE_*` options:

```cmake
option(ENABLE_AI_ACCELERATORS "Enable AI accelerator resource plane (CPU/mock baseline, runtime opt-in)" ON)
```

### 5.2 Generated Macro

In the `# =============================================================================
# Generated configuration header` section of `CMakeLists.txt`, add:

```cmake
set(HPACTOR_ENABLE_AI_ACCELERATORS ${ENABLE_AI_ACCELERATORS})
```

In `include/hpactor/hpactor_config.hpp.in`, add:

```cpp
#cmakedefine01 HPACTOR_ENABLE_AI_ACCELERATORS
```

### 5.3 Source File Guarding

When `ENABLE_AI_ACCELERATORS=OFF`:
- The AI public headers (`accelerator_types.hpp`, `accelerator_config.hpp`,
  `ai_type_tags.hpp`, `node_resource_summary.hpp`) are **not** compiled into
  any translation unit.
- `src/ai/accelerator_types.cpp` and `src/ai/ai_message_registry.cpp` are
  excluded from `hpactor_lib` via a conditional in `src/CMakeLists.txt`.
- The new parser `src/config/parsers/ai_accelerator_config_parser.cpp` is
  excluded from `hpactor_lib` via the same conditional.
- The `ai::AcceleratorConfig` fields in `Config` and `SystemDef` are elided
  by the `#if` guard, and the `load_topology()` mapping is guarded out.

When `ENABLE_AI_ACCELERATORS=ON` (default):
- All AI sources are compiled.
- Runtime behavior is still **disabled by default** — `AcceleratorConfig::enabled`
  defaults to `false`. Only explicit `[system.ai.accelerators] enabled = true`
  in TOML activates the resource plane.

### 5.4 Interaction with Other Build Flags

`ENABLE_AI_ACCELERATORS` is independent of `ENABLE_ACTOR_METRICS`,
`ENABLE_ACTOR_LOGGING`, `ENABLE_ACTOR_TRACING`, `ENABLE_CLI`, and
`ENABLE_FAULT_INJECTION`. When the AI subsystem is compiled but disabled at
runtime, it emits no metrics, logs, traces, or CLI commands. Future phases
(#160–#164) will add observability hooks gated on both the AI build flag and
the relevant subsystem flag.

## 6. Config Struct Integration

### 6.1 SystemDef (`topology_model.hpp`)

Add to `SystemDef` near the existing subsystem config fields (e.g., after
`quarantine_defaults`):

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
    /// \brief AI accelerator resource plane configuration.
    hpactor::ai::AcceleratorConfig ai_accelerators;
#endif
```

The include for `accelerator_config.hpp` is already available transitively
or must be added explicitly:

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
#include <hpactor/ai/accelerator_config.hpp>
#endif
```

Since `topology_model.hpp` currently has no conditional includes, prefer adding
a direct include guard:

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
#include <hpactor/ai/accelerator_config.hpp>
#endif
```

### 6.2 Runtime Config (`actor_system.hpp`)

Add to the `Config` struct near the existing subsystem config fields (e.g.,
after `tracing`):

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
    /// \brief AI accelerator resource plane configuration.
    ///        Runtime-disabled by default; enable via [system.ai.accelerators].
    ai::AcceleratorConfig ai_accelerators;
#endif
```

Add the include guard near the top of `actor_system.hpp`:

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
#include <hpactor/ai/accelerator_config.hpp>
#endif
```

### 6.3 Bootstrap Wiring (`actor_system.cpp`)

In `ActorSystem::load_topology()`, after the existing tracing config line
(`apply_tracing_config(model.system.tracing)`), add:

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
    config_.ai_accelerators = model.system.ai_accelerators;
#endif
```

This is the canonical pattern used by `logging_config_`,
`apply_tracing_config()`, and `config_.dead_letters` in the same function.

## 7. Self-Registering TOML Parser

### 7.1 Parser Design

File: `src/config/parsers/ai_accelerator_config_parser.cpp`

The parser:
1. Implements `ITomlSystemConfigParser`.
2. Self-registers via `TomlSystemParserRegistration<AiAcceleratorConfigParser>`.
3. Parses the `[system.ai.accelerators]` subsection.
4. Returns early (no-op) when the `ai.accelerators` table is absent — the
   subsystem stays at its compile-time defaults (disabled).
5. Returns `result<void>` error for any invalid value.

Registration order: `kOrder = 75` — after `system.quarantine` (70) and before
`system.metrics` (100). This ensures AI config is parsed before any subsystem
that might consume AI resource state.

### 7.2 Parser Implementation Sketch

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/ai/accelerator_config.hpp>
#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>

#include <unordered_set>

namespace hpactor::config {
namespace {

class AiAcceleratorConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.ai.accelerators";
    static constexpr int kOrder = 75;

    std::string_view name() const noexcept override { return kName; }
    int order() const noexcept override { return kOrder; }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& ctx) const override {
        auto ai = system.table("ai");
        if (!ai.valid()) return result<void>::make();

        auto accel = ai.table("accelerators");
        if (!accel.valid()) return result<void>::make();

        auto& cfg = out.ai_accelerators;

        // ── Top-level booleans ─────────────────────────────────────
        cfg.enabled = accel.read_bool("enabled", false);
        if (!cfg.enabled) return result<void>::make();  // nothing else matters

        cfg.enable_cpu_probe = accel.read_bool("enable_cpu_probe", true);
        cfg.allow_cpu_fallback = accel.read_bool("allow_cpu_fallback", true);
        cfg.allow_empty_inventory = accel.read_bool("allow_empty_inventory", false);
        cfg.require_resource_plane_ready =
            accel.read_bool("require_resource_plane_ready", false);

        // ── TTL values with bounds validation ─────────────────────
        cfg.lease_ttl_ms = accel.read_uint32("lease_ttl_ms", 30000);
        cfg.min_lease_ttl_ms = accel.read_uint32("min_lease_ttl_ms", 1000);
        cfg.max_lease_ttl_ms = accel.read_uint32("max_lease_ttl_ms", 300000);

        if (cfg.min_lease_ttl_ms > cfg.max_lease_ttl_ms) {
            return result<void>::make(error{
                errors::invalid_argument,
                "ai.accelerators: min_lease_ttl_ms must be <= max_lease_ttl_ms"});
        }
        if (cfg.lease_ttl_ms < cfg.min_lease_ttl_ms ||
            cfg.lease_ttl_ms > cfg.max_lease_ttl_ms) {
            return result<void>::make(error{
                errors::invalid_argument,
                "ai.accelerators: lease_ttl_ms must be within [min_lease_ttl_ms, max_lease_ttl_ms]"});
        }

        // ── Other scalar values ────────────────────────────────────
        cfg.probe_interval_ms = accel.read_uint32("probe_interval_ms", 1000);
        cfg.missing_device_grace_ms =
            accel.read_uint32("missing_device_grace_ms", 5000);

        // ── CPU budget (MB → bytes with overflow check) ──────────
        uint32_t cpu_mem_mb = accel.read_uint32("cpu_host_memory_mb", 0);
        if (cpu_mem_mb > 0) {
            constexpr uint64_t kMbToBytes = 1024ULL * 1024ULL;
            if (cpu_mem_mb > UINT64_MAX / kMbToBytes) {
                return result<void>::make(error{
                    errors::invalid_argument,
                    "ai.accelerators: cpu_host_memory_mb overflow"});
            }
            cfg.cpu_host_memory_budget_bytes =
                static_cast<uint64_t>(cpu_mem_mb) * kMbToBytes;
        }
        cfg.cpu_compute_units = accel.read_uint32("cpu_compute_units", 0);

        // ── Admission policy (enum string → validated) ────────────
        auto policy_str = accel.read_string("admission_policy", "most_free_memory");
        auto policy = parse_admission_policy(policy_str);
        if (!policy.has_value()) {
            return result<void>::make(error{
                errors::invalid_argument,
                "ai.accelerators: unknown admission_policy '" + policy_str + "'"});
        }
        cfg.admission_policy = policy.value();

        // ── Mock devices ───────────────────────────────────────────
        std::unordered_set<std::string> seen_ids;
        accel.for_each_table_array("mock_device", [&](TomlTableView md) {
            MockDeviceConfig dev;
            dev.id = md.read_string("id", "");
            if (dev.id.empty()) return;  // skip entries with empty id

            if (!seen_ids.insert(dev.id).second) {
                // Duplicate id — flag via context; parser can't
                // return error from inside a visitor, so accumulate
                // and check after the loop.
                ctx.add_error("ai.accelerators.mock_device: duplicate id '" +
                              dev.id + "'");
                return;
            }

            dev.kind = parse_device_kind(md.read_string("kind", "mock"))
                           .value_or(DeviceKind::Mock);
            dev.vendor = parse_device_vendor(md.read_string("vendor", "mock"))
                             .value_or(DeviceVendor::Mock);
            dev.name = md.read_string("name", dev.id);

            uint32_t mem_mb = md.read_uint32("memory_mb", 0);
            if (mem_mb > 0) {
                constexpr uint64_t kMbToBytes = 1024ULL * 1024ULL;
                if (mem_mb > UINT64_MAX / kMbToBytes) {
                    ctx.add_error(
                        "ai.accelerators.mock_device." + dev.id +
                        ": memory_mb overflow");
                    return;
                }
                dev.device_memory_bytes =
                    static_cast<uint64_t>(mem_mb) * kMbToBytes;
            }

            uint32_t host_mb = md.read_uint32("host_memory_mb", 0);
            if (host_mb > 0) {
                constexpr uint64_t kMbToBytes = 1024ULL * 1024ULL;
                if (host_mb > UINT64_MAX / kMbToBytes) {
                    ctx.add_error(
                        "ai.accelerators.mock_device." + dev.id +
                        ": host_memory_mb overflow");
                    return;
                }
                dev.host_memory_bytes =
                    static_cast<uint64_t>(host_mb) * kMbToBytes;
            }

            dev.compute_units = md.read_uint32("compute_units", 0);
            dev.stream_slots = md.read_uint32("stream_slots", 0);
            dev.exclusive_only = md.read_bool("exclusive_only", false);

            auto health_str = md.read_string("health", "healthy");
            dev.health = parse_device_health(health_str)
                             .value_or(DeviceHealth::Healthy);

            // labels
            auto labels_table = md.table("labels");
            if (labels_table.valid()) {
                labels_table.for_each_entry([&](std::string_view key,
                                                TomlValueView val) {
                    DeviceLabel label;
                    label.key = std::string(key);
                    label.value = val.as_string("");
                    dev.labels.push_back(std::move(label));
                });
            }

            cfg.mock_devices.push_back(std::move(dev));
        });

        // Check for accumulated errors from visitor callbacks.
        if (ctx.has_errors()) {
            return result<void>::make(error{
                errors::invalid_argument, "ai.accelerators.mock_device: validation failed"});
        }

        return result<void>::make();
    }

  private:
    static result<AdmissionPolicyKind>
    parse_admission_policy(std::string_view s) noexcept {
        if (s == "first_fit")       return AdmissionPolicyKind::FirstFit;
        if (s == "most_free_memory") return AdmissionPolicyKind::MostFreeMemory;
        if (s == "exact_device")     return AdmissionPolicyKind::ExactDevice;
        if (s == "cpu_fallback")     return AdmissionPolicyKind::CpuFallback;
        return error{errors::invalid_argument};
    }

    static result<DeviceKind>
    parse_device_kind(std::string_view s) noexcept {
        if (s == "cpu")          return DeviceKind::Cpu;
        if (s == "gpu")          return DeviceKind::Gpu;
        if (s == "npu")          return DeviceKind::Npu;
        if (s == "accelerator")  return DeviceKind::Accelerator;
        if (s == "mock")         return DeviceKind::Mock;
        return error{errors::invalid_argument};
    }

    static result<DeviceVendor>
    parse_device_vendor(std::string_view s) noexcept {
        if (s == "unknown") return DeviceVendor::Unknown;
        if (s == "nvidia")  return DeviceVendor::Nvidia;
        if (s == "amd")     return DeviceVendor::Amd;
        if (s == "apple")   return DeviceVendor::Apple;
        if (s == "intel")   return DeviceVendor::Intel;
        if (s == "mock")    return DeviceVendor::Mock;
        return error{errors::invalid_argument};
    }

    static result<DeviceHealth>
    parse_device_health(std::string_view s) noexcept {
        if (s == "unknown")      return DeviceHealth::Unknown;
        if (s == "healthy")      return DeviceHealth::Healthy;
        if (s == "degraded")     return DeviceHealth::Degraded;
        if (s == "unavailable")  return DeviceHealth::Unavailable;
        if (s == "lost")         return DeviceHealth::Lost;
        return error{errors::invalid_argument};
    }
};

const TomlSystemParserRegistration<AiAcceleratorConfigParser>
    kRegisterAiAcceleratorConfigParser;

} // namespace
} // namespace hpactor::config
```

### 7.3 Validation Rules Summary

| Rule | Validation | Error Behavior |
|------|-----------|----------------|
| `enabled = false` | Skips remaining parsing | No error — subsystem stays at defaults |
| `admission_policy` | Must be one of: `first_fit`, `most_free_memory`, `exact_device`, `cpu_fallback` | `result<void>` error, parse fails |
| `min_lease_ttl_ms` ≤ `max_lease_ttl_ms` | Numeric comparison | `result<void>` error, parse fails |
| `lease_ttl_ms` ∈ `[min, max]` | Numeric range check | `result<void>` error, parse fails |
| `cpu_host_memory_mb` → bytes | MB × 1024² ≤ `UINT64_MAX` | `result<void>` error, parse fails |
| Mock device `kind` | Must be one of: `cpu`, `gpu`, `npu`, `accelerator`, `mock` | Invalid → defaults to `Mock` (non-fatal for unknown keys rule; but invalid enum string is a validation failure) |
| Mock device `vendor` | Must be one of: `unknown`, `nvidia`, `amd`, `apple`, `intel`, `mock` | Invalid → defaults to `Mock` (same principle as kind) |
| Mock device `health` | Must be one of: `unknown`, `healthy`, `degraded`, `unavailable`, `lost` | Invalid → defaults to `Healthy` |
| Mock device `id` uniqueness | All `id` values within one document must be distinct | Parse error via context accumulation |
| Mock device `memory_mb` → bytes | MB × 1024² ≤ `UINT64_MAX` | Parse error via context accumulation |
| Mock device `host_memory_mb` → bytes | MB × 1024² ≤ `UINT64_MAX` | Parse error via context accumulation |
| Unknown keys | Ignored | No error — forward-compatible |
| Invalid numeric values (e.g., string where uint32 expected) | `TomlTableView` read methods silently return fallback | Acceptable — the TOML type system prevents this at the parse layer before our parser runs. `toml++` itself rejects type mismatches during file parsing. |

**Important design decision on enum validation**: The acceptance criteria say
"invalid values never silently fall back to defaults." For enum strings, an
unknown value returns a `result<>` parse error. However, there is a trade-off:
if we fail the entire parse on one bad mock-device `kind` string, that prevents
all other valid config from loading. The implementation should fail the parse
for top-level fields (`admission_policy`) and accumulate errors for array
elements (mock devices) so the operator sees all validation failures at once.

### 7.4 Parser Ordering

| Order | Parser | Rationale |
|-------|--------|-----------|
| 0 | `system.core` | Must parse first — version, imports, etc. |
| 70 | `system.quarantine` | Actor lifecycle config |
| **75** | **`system.ai.accelerators`** | **After core, before metrics/tracing/logging that may consume AI state** |
| 100 | `system.metrics` | Metrics subsystem |
| 110 | `system.shutdown` | Shutdown/drain config |

## 8. TomlParseContext Error Accumulation

The current `TomlParseContext` interface (`toml_parse_context.hpp`) may not
support `add_error()` / `has_errors()`. If not, the parser has two options:

**Option A (preferred)**: Extend `TomlParseContext` with a minimal error
accumulation API:
```cpp
class TomlParseContext {
  public:
    // ... existing ...
    void add_error(std::string msg);
    bool has_errors() const noexcept;
    const std::vector<std::string>& errors() const noexcept;
  private:
    std::vector<std::string> errors_;
};
```

**Option B (fallback)**: Fail on the first invalid mock-device field rather
than accumulating. This is simpler but gives operators a worse experience
(one error per parse attempt).

Recommendation: implement Option A. The change is small (two methods + one
member on `TomlParseContext`) and benefits all future parsers that iterate
over table arrays.

## 9. TOML Fixture Design

### 9.1 Disabled Config (`ai_accelerators_disabled.toml`)

```toml
[system]
scheduler_threads = 1

[system.ai.accelerators]
enabled = false
```

Expectation: parser returns success, `AcceleratorConfig::enabled == false`,
all other fields at defaults.

### 9.2 CPU-Only Config (`ai_accelerators_cpu.toml`)

```toml
[system]
scheduler_threads = 1

[system.ai.accelerators]
enabled = true
enable_cpu_probe = true
allow_cpu_fallback = true
probe_interval_ms = 2000
lease_ttl_ms = 60000
min_lease_ttl_ms = 5000
max_lease_ttl_ms = 120000
admission_policy = "first_fit"
cpu_host_memory_mb = 16384
cpu_compute_units = 16
```

Expectation: parser returns success, CPU probe enabled, no mock devices,
TTL values matched, admission policy is `FirstFit`, memory budget =
16,384 × 1024² bytes.

### 9.3 Mock Devices Config (`ai_accelerators_mock.toml`)

```toml
[system]
scheduler_threads = 1

[system.ai.accelerators]
enabled = true
enable_cpu_probe = true
admission_policy = "most_free_memory"
lease_ttl_ms = 30000

[[system.ai.accelerators.mock_device]]
id = "mock-gpu-0"
kind = "gpu"
vendor = "mock"
name = "Mock GPU 0"
memory_mb = 24576
compute_units = 100
stream_slots = 32
health = "healthy"
labels = { backend = "mock", precision = "fp16,bf16" }

[[system.ai.accelerators.mock_device]]
id = "mock-gpu-1"
kind = "gpu"
vendor = "mock"
name = "Mock GPU 1"
memory_mb = 12288
compute_units = 50
stream_slots = 16
health = "healthy"
```

Expectation: parser returns success, two mock devices with correct fields,
label parsing produces correct key-value pairs.

### 9.4 Invalid Enum Config (`ai_accelerators_invalid_enum.toml`)

```toml
[system]
scheduler_threads = 1

[system.ai.accelerators]
enabled = true
admission_policy = "round_robin"
```

Expectation: parser returns error. `"round_robin"` is not a valid
`AdmissionPolicyKind`.

### 9.5 Invalid TTL Config (`ai_accelerators_invalid_ttl.toml`)

```toml
[system]
scheduler_threads = 1

[system.ai.accelerators]
enabled = true
lease_ttl_ms = 5000
min_lease_ttl_ms = 10000
max_lease_ttl_ms = 30000
```

Expectation: parser returns error. `lease_ttl_ms` (5000) < `min_lease_ttl_ms`
(10000).

### 9.6 Invalid Duplicate Mock ID Config (`ai_accelerators_invalid_duplicate_id.toml`)

```toml
[system]
scheduler_threads = 1

[system.ai.accelerators]
enabled = true

[[system.ai.accelerators.mock_device]]
id = "dup-gpu"
kind = "gpu"
memory_mb = 8192

[[system.ai.accelerators.mock_device]]
id = "dup-gpu"
kind = "gpu"
memory_mb = 16384
```

Expectation: parser returns error (duplicate `id`).

## 10. Test Plan

### 10.1 Test Binary

`tests/unit/config/test_ai_accelerator_config.cpp` — single GTest binary.

### 10.2 Test Cases

| Test | What It Verifies |
|------|-----------------|
| `ConfigDefaults` | Default-constructed `AcceleratorConfig`: `enabled == false`, `lease_ttl_ms == 30000`, `min == 1000`, `max == 300000`, `mock_devices.empty()`, `admission_policy == MostFreeMemory` |
| `ParserDisabled` | Parser returns success; `enabled == false`; no fields modified beyond default |
| `ParserAbsent` | When `[system.ai]` table is absent, parser returns success; config at defaults |
| `ParserCpuOnly` | CPU-only TOML: all scalar fields parsed correctly; MB→bytes conversion correct; `admission_policy == FirstFit` |
| `ParserMockDevices` | Mock config: two devices parsed; id, kind, vendor, name, memory, compute, stream_slots, health, labels all correct |
| `ParserMockDeviceLabels` | Labels table parsed into correct `DeviceLabel` key-value pairs |
| `ParserInvalidAdmissionPolicy` | Unknown enum string → parse error |
| `ParserInvalidDeviceKind` | Unknown device kind in mock_device → parse error |
| `ParserTtlBounds` | `lease_ttl_ms` outside `[min, max]` → parse error |
| `ParserMinGtMax` | `min_lease_ttl_ms > max_lease_ttl_ms` → parse error |
| `ParserDuplicateMockId` | Two mock devices with same `id` → parse error |
| `ParserMemoryMbOverflow` | `cpu_host_memory_mb` > `UINT64_MAX / (1024*1024)` → parse error |
| `ParserMockMemoryMbOverflow` | Mock device `memory_mb` overflow → parse error |
| `ParserUnknownKeysIgnored` | Unknown key in `[system.ai.accelerators]` → parse succeeds |
| `BuildGateOff` | (Manual or CI-only) When `ENABLE_AI_ACCELERATORS=OFF`, `Config` has no `ai_accelerators` field; existing non-AI tests pass |

Tests that require actually invoking the TOML parser should use the
`TomlParser::parse()` entry point with inline TOML strings or fixture files.
Follow the pattern established by existing parser tests in
`tests/unit/config/`.

### 10.3 CMake Wiring

```cmake
# tests/unit/config/CMakeLists.txt addition (or update existing)
add_executable(test_ai_accelerator_config test_ai_accelerator_config.cpp)
target_link_libraries(test_ai_accelerator_config PRIVATE hpactor GTest::gtest_main)
target_compile_definitions(test_ai_accelerator_config PRIVATE
    HPACTOR_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
gtest_discover_tests(test_ai_accelerator_config)
```

## 11. Source File CMake Integration

### `src/CMakeLists.txt`

The `src/CMakeLists.txt` needs to conditionally add the parser source:

```cmake
if(ENABLE_AI_ACCELERATORS)
    target_sources(hpactor_lib PRIVATE
        ai/accelerator_types.cpp
        ai/ai_message_registry.cpp
        config/parsers/ai_accelerator_config_parser.cpp
    )
endif()
```

Note: `ai/accelerator_types.cpp` and `ai/ai_message_registry.cpp` are from
AI-ACC-001A (#158). If they are not yet in `src/CMakeLists.txt`, add them
here (they are in scope as a dependency). If they were already added in #158,
this issue only adds the parser .cpp.

## 12. Acceptance Criteria

- [ ] Default build (`cmake -S . -B build -GNinja && ninja -C build`) succeeds
  with `ENABLE_AI_ACCELERATORS=ON`; AI code compiled but runtime disabled.
- [ ] Build with `ENABLE_AI_ACCELERATORS=OFF` succeeds; no AI headers
  compiled into `hpactor_lib`; existing non-AI tests pass.
- [ ] `HPACTOR_ENABLE_AI_ACCELERATORS` is 1 in generated `hpactor_config.hpp`
  when the CMake option is ON.
- [ ] `SystemDef::ai_accelerators` and `Config::ai_accelerators` are present
  when the macro is 1, absent when 0.
- [ ] `load_topology()` correctly copies `SystemDef::ai_accelerators` →
  `Config::ai_accelerators`.
- [ ] Parser self-registers and is discovered by `TomlParserRegistry`.
- [ ] Parser validates `admission_policy` enum strings — unknown values
  produce parse error.
- [ ] Parser validates `min_lease_ttl_ms ≤ max_lease_ttl_ms` and
  `lease_ttl_ms ∈ [min, max]`.
- [ ] Parser validates unique mock-device ids.
- [ ] Parser validates MB-to-byte overflow for `cpu_host_memory_mb` and mock
  device `memory_mb`/`host_memory_mb`.
- [ ] Parser ignores unknown keys (forward-compatible).
- [ ] Parser returns `result<void>::make()` when `enabled = false` or table
  is absent (graceful no-op).
- [ ] All parser unit tests pass (disabled, CPU, mock, invalid-enum,
  invalid-ttl, duplicate-id, overflow).
- [ ] Existing non-AI tests continue to pass with AI compiled in but
  runtime-disabled.
- [ ] No public header exposes `toml++`, protobuf internals, or vendor SDK types.
- [ ] Source compatibility preserved for existing actor APIs.

## 13. Dependencies and Sequencing

```
AI-ACC-001A (#158) ──► AI-ACC-001B (#159, this issue)
                               │
                               ├── AcceleratorConfig struct (already exists from #158)
                               ├── CMake build gate (NEW)
                               ├── SystemDef + Config integration (NEW)
                               └── TOML parser + tests (NEW)
                                       │
                                       ▼
                              AI-ACC-001C (#160) — ResourceLedger + probes
```

AI-ACC-001A must be merged before this issue's implementation begins, because:
- `accelerator_config.hpp` (the `AcceleratorConfig` and `MockDeviceConfig`
  structs) must exist.
- `accelerator_types.hpp` (the enums: `AdmissionPolicyKind`, `DeviceKind`,
  `DeviceVendor`, `DeviceHealth`) must exist for the parser's enum validation
  helpers.
- `hpactor_config.hpp.in` with `HPACTOR_ENABLE_AI_ACCELERATORS` must be
  consumable by the include guards in `topology_model.hpp` and
  `actor_system.hpp`.

## 14. Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| `TomlParseContext` lacks error accumulation | Implement Option A (extend `TomlParseContext`) if feasible; otherwise use Option B (fail-fast on first error) |
| `for_each_table_array` visitor can't return errors | Accumulate errors in `TomlParseContext` and check after the loop |
| MB-to-byte overflow edge case | Use compile-time constant `1024ULL * 1024ULL` and compare against `UINT64_MAX / kMbToBytes` before multiplication |
| Duplicate mock-device id across large configs | `std::unordered_set<std::string>` for O(1) duplicate detection |
| Conditional compilation breaks IDE navigation | `#if HPACTOR_ENABLE_AI_ACCELERATORS` guards are minimal and follow existing patterns (`HPACTOR_ENABLE_CLI`, etc.) |

## 15. Open Questions

1. **Should `TomlParseContext` be extended with error accumulation?** The
   mock-device table array visitor cannot return `result<void>` errors directly.
   Recommendation: extend `TomlParseContext` with `add_error()`/`has_errors()`.
   This is a small, backward-compatible change that benefits all future parsers.

2. **Should mock-device enum validation be fatal or best-effort?** If a single
   mock device has an invalid `kind` string, should the entire parse fail or
   should that device be skipped with a warning? The acceptance criteria say
   "invalid values never silently fall back to defaults." Recommendation:
   accumulate all validation errors and fail the parse with a combined error
   message listing each failure. This gives the operator a complete picture.

3. **Should `ENABLE_AI_ACCELERATORS` default to ON?** The detailed design spec
   suggests ON for base CPU/mock support. The AI subsystem has no vendor
   dependencies in the base implementation, so ON is safe. Confirm this
   matches the project's stance on default-enabled optional subsystems.
