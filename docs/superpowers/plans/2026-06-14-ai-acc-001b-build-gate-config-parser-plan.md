# AI-ACC-001B: Build Gate & TOML Config Parser — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the compile-time build gate (`ENABLE_AI_ACCELERATORS` / `HPACTOR_ENABLE_AI_ACCELERATORS`), wire `ai::AcceleratorConfig` into `Config` and `SystemDef`, and ship a self-registering TOML subsystem parser with comprehensive validation.

**Architecture:** Seven TDD-driven tasks in dependency order. The AI types, protos, and message registry already exist from AI-ACC-001A (#158). This plan adds the build plumbing, config struct integration, parser, fixtures, and tests. Each task follows RED-GREEN-REFACTOR: write a failing test, implement the minimum to pass, verify, commit.

**Tech Stack:** C++20, CMake/Ninja, Google Test, TomlTableView (opaque TOML wrapper)

**Design Spec:** `docs/superpowers/specs/2026-06-14-ai-acc-001b-build-gate-config-parser-design.md`

**Worktree:** `.worktrees/ai-acc-001b-config-parser` (branch `worktree-ai-acc-001b-config-parser`)

**Prerequisite files (already exist from #158):**
- `include/hpactor/ai/accelerator_types.hpp` — enums, structs, validation
- `include/hpactor/ai/accelerator_config.hpp` — AcceleratorConfig, MockDeviceConfig
- `include/hpactor/ai/ai_type_tags.hpp` — TypeTag constants + MessageTraits
- `include/hpactor/ai/node_resource_summary.hpp` — summary structs
- `protos/hpactor/ai_resource.proto` — protobuf contract
- `src/ai/accelerator_types.cpp` — to_string + validation impls
- `src/ai/ai_message_registry.cpp` — ProtoTypeRegistry registrar
- `tests/unit/ai/CMakeLists.txt` + `test_accelerator_types.cpp` — ~44 tests
- `cmake/dependencies.cmake` — ai_resource.proto in codegen
- `src/CMakeLists.txt` — ai/ sources in hpactor_lib (unconditional currently)

---

### Task 1: CMake Build Gate

**Files:**
- Modify: `CMakeLists.txt` — add `ENABLE_AI_ACCELERATORS` option + generated macro
- Modify: `include/hpactor/hpactor_config.hpp.in` — add `#cmakedefine01`
- Modify: `src/CMakeLists.txt` — guard AI sources behind `ENABLE_AI_ACCELERATORS`

**Purpose:** Add the compile-time build gate. When OFF, zero AI code is compiled. When ON (default), AI code is compiled but runtime-disabled by default.

- [ ] **Step 1: Add CMake option**

In `CMakeLists.txt`, after the existing `ENABLE_*` options block (after `ENABLE_ACTOR_PASSIVATION`), add:

```cmake
option(ENABLE_AI_ACCELERATORS "Enable AI accelerator resource plane (CPU/mock baseline, runtime opt-in)" ON)
```

- [ ] **Step 2: Add generated macro**

In the `# Generated configuration header` section of `CMakeLists.txt` (after `set(HPACTOR_ENABLE_PASSIVATION ...)`), add:

```cmake
set(HPACTOR_ENABLE_AI_ACCELERATORS ${ENABLE_AI_ACCELERATORS})
```

In `include/hpactor/hpactor_config.hpp.in`, after `#cmakedefine01 HPACTOR_ENABLE_FAULT_INJECTION`, add:

```cpp
#cmakedefine01 HPACTOR_ENABLE_AI_ACCELERATORS
```

- [ ] **Step 3: Guard AI sources in src/CMakeLists.txt**

In `src/CMakeLists.txt`, change the unconditional AI source entries (currently around line 131-132):

```cmake
# Before (unconditional):
    ai/accelerator_types.cpp
    ai/ai_message_registry.cpp

# After (guarded):
if(ENABLE_AI_ACCELERATORS)
    ai/accelerator_types.cpp
    ai/ai_message_registry.cpp
endif()
```

- [ ] **Step 4: Build and verify — default ON**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build hpactor_lib
```

Expected: builds successfully. `HPACTOR_ENABLE_AI_ACCELERATORS` is 1 in generated `hpactor_config.hpp`.

- [ ] **Step 5: Build and verify — OFF**

```bash
cmake -S . -B build-off -GNinja -DENABLE_AI_ACCELERATORS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-off hpactor_lib
```

Expected: builds successfully. AI sources excluded. `HPACTOR_ENABLE_AI_ACCELERATORS` is 0.

- [ ] **Step 6: Run existing AI tests with ON**

```bash
ninja -C build tests/unit/ai/test_accelerator_types
./build/tests/unit/ai/test_accelerator_types
```

Expected: all ~44 tests pass (AI-ACC-001A baseline unchanged).

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt include/hpactor/hpactor_config.hpp.in src/CMakeLists.txt
git commit -m "build: add ENABLE_AI_ACCELERATORS CMake option and HPACTOR_ENABLE_AI_ACCELERATORS macro

Add compile-time build gate for the AI accelerator resource plane.
Default ON — AI code compiles but remains runtime-disabled unless
[system.ai.accelerators] enabled = true in TOML config.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: SystemDef + Config Struct Integration

**Files:**
- Modify: `include/hpactor/config/topology_model.hpp` — add `ai::AcceleratorConfig ai_accelerators;` to `SystemDef`
- Modify: `include/hpactor/core/actor_system.hpp` — add `ai::AcceleratorConfig ai_accelerators;` to `Config`

**Purpose:** Add the AI config field to both the TOML-parsed struct (`SystemDef`) and the runtime struct (`Config`), guarded by the build macro.

- [ ] **Step 1: Add to SystemDef**

In `include/hpactor/config/topology_model.hpp`:

Add the include guard near the top (after the existing `#include` block):

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
#include <hpactor/ai/accelerator_config.hpp>
#endif
```

Add the field inside `SystemDef`, after `quarantine_defaults` and before `transport_outbound_limits`:

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
    /// \brief AI accelerator resource plane configuration.
    hpactor::ai::AcceleratorConfig ai_accelerators;
#endif
```

Note: `SystemDef` currently has no conditional includes. We need the `#include` of `hpactor_config.hpp` (which provides the macro) to be available. Verify the include chain: `topology_model.hpp` includes other hpactor headers that ultimately pull in `hpactor_config.hpp`. If not, add `#include <hpactor/hpactor_config.hpp>` explicitly.

- [ ] **Step 2: Add to Config**

In `include/hpactor/core/actor_system.hpp`:

Add the include guard near the top (after the existing includes):

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
#include <hpactor/ai/accelerator_config.hpp>
#endif
```

Add the field inside `Config`, after `tracing` and before the closing `};`:

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
    /// \brief AI accelerator resource plane configuration.
    ///        Runtime-disabled by default; enable via [system.ai.accelerators].
    ai::AcceleratorConfig ai_accelerators;
#endif
```

- [ ] **Step 3: Build and verify**

```bash
ninja -C build hpactor_lib
```

Expected: builds with `HPACTOR_ENABLE_AI_ACCELERATORS=1`. Both structs have the new field.

```bash
cmake -S . -B build-off -GNinja -DENABLE_AI_ACCELERATORS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-off hpactor_lib
```

Expected: builds with `HPACTOR_ENABLE_AI_ACCELERATORS=0`. Neither struct has the field.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/config/topology_model.hpp include/hpactor/core/actor_system.hpp
git commit -m "feat(ai): add ai::AcceleratorConfig to SystemDef and Config structs

Guarded by HPACTOR_ENABLE_AI_ACCELERATORS. Default-constructed
AcceleratorConfig has enabled=false — runtime remains disabled
until explicitly enabled via TOML config.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Bootstrap Wiring in load_topology()

**Files:**
- Modify: `src/actor/actor_system.cpp` — wire `ai_accelerators` mapping

**Purpose:** In `ActorSystem::load_topology()`, copy `SystemDef::ai_accelerators` → `Config::ai_accelerators`, following the same pattern used by `dead_letters`, `logging`, and `tracing`.

- [ ] **Step 1: Add the mapping**

In `src/actor/actor_system.cpp`, in the `load_topology()` function, after the `apply_tracing_config(model.system.tracing);` line, add:

```cpp
#if HPACTOR_ENABLE_AI_ACCELERATORS
    config_.ai_accelerators = model.system.ai_accelerators;
#endif
```

- [ ] **Step 2: Build and verify**

```bash
ninja -C build hpactor_lib
```

Expected: builds successfully. The `load_topology()` function now copies AI config when the macro is enabled.

- [ ] **Step 3: Commit**

```bash
git add src/actor/actor_system.cpp
git commit -m "feat(ai): wire ai::AcceleratorConfig from SystemDef to Config in load_topology()

Copy SystemDef::ai_accelerators → Config::ai_accelerators during
TOML topology bootstrap, following the same pattern as dead_letters
and tracing config.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: TOML Parser — Scalar Fields and Validation

**Files:**
- Create: `src/config/parsers/ai_accelerator_config_parser.cpp`
- Modify: `src/CMakeLists.txt` — add parser source

**Purpose:** Create the self-registering parser that reads `[system.ai.accelerators]`. This task handles scalar fields (booleans, integers, strings) and their validation. Mock device array parsing is deferred to Task 5.

- [ ] **Step 1: RED — Write a minimal test that requires the parser to exist**

Create `tests/unit/config/test_ai_accelerator_config.cpp` (placeholder — will be expanded in Tasks 7-9):

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

TEST(AiAcceleratorConfig, Placeholder) {
    EXPECT_EQ(1, 1);
}
```

Add the test target in `tests/unit/config/CMakeLists.txt`. Check if the file already has multiple test targets; if not, add:

```cmake
if(ENABLE_AI_ACCELERATORS)
    add_executable(test_ai_accelerator_config test_ai_accelerator_config.cpp)
    target_link_libraries(test_ai_accelerator_config PRIVATE hpactor GTest::gtest_main)
    target_compile_definitions(test_ai_accelerator_config PRIVATE
        HPACTOR_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
    gtest_discover_tests(test_ai_accelerator_config)
endif()
```

If `tests/unit/config/CMakeLists.txt` uses a different pattern (e.g., adding sources to a combined test binary), follow the existing pattern.

- [ ] **Step 2: RED — Build test, verify it runs**

```bash
ninja -C build tests/unit/config/test_ai_accelerator_config
./build/tests/unit/config/test_ai_accelerator_config
```

Expected: 1/1 test passes (placeholder).

- [ ] **Step 3: GREEN — Create the self-registering parser**

Create `src/config/parsers/ai_accelerator_config_parser.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/ai/accelerator_config.hpp>
#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/types/types.hpp>

#include <string>
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
                       TomlParseContext& /*ctx*/) const override {
        auto ai = system.table("ai");
        if (!ai.valid()) return result<void>::make();

        auto accel = ai.table("accelerators");
        if (!accel.valid()) return result<void>::make();

        auto& cfg = out.ai_accelerators;

        // ── enabled gate ──────────────────────────────────────────
        cfg.enabled = accel.read_bool("enabled", false);
        if (!cfg.enabled) return result<void>::make();

        // ── Top-level booleans ────────────────────────────────────
        cfg.enable_cpu_probe = accel.read_bool("enable_cpu_probe", true);
        cfg.allow_cpu_fallback = accel.read_bool("allow_cpu_fallback", true);
        cfg.allow_empty_inventory =
            accel.read_bool("allow_empty_inventory", false);
        cfg.require_resource_plane_ready =
            accel.read_bool("require_resource_plane_ready", false);

        // ── TTL values with bounds validation ────────────────────
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
                "ai.accelerators: lease_ttl_ms must be within "
                "[min_lease_ttl_ms, max_lease_ttl_ms]"});
        }

        // ── Other scalar values ───────────────────────────────────
        cfg.probe_interval_ms = accel.read_uint32("probe_interval_ms", 1000);
        cfg.missing_device_grace_ms =
            accel.read_uint32("missing_device_grace_ms", 5000);

        // ── CPU budget (MB → bytes with overflow check) ─────────
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

        // ── Admission policy (enum string → validated) ───────────
        auto policy_str =
            accel.read_string("admission_policy", "most_free_memory");
        auto policy = parse_admission_policy(policy_str);
        if (!policy.has_value()) {
            return result<void>::make(error{
                errors::invalid_argument,
                "ai.accelerators: unknown admission_policy '" +
                    policy_str + "'"});
        }
        cfg.admission_policy = policy.value();

        return result<void>::make();
    }

  private:
    static ai::AdmissionPolicyKind
    parse_admission_policy(std::string_view s) noexcept {
        if (s == "first_fit")        return ai::AdmissionPolicyKind::FirstFit;
        if (s == "most_free_memory") return ai::AdmissionPolicyKind::MostFreeMemory;
        if (s == "exact_device")     return ai::AdmissionPolicyKind::ExactDevice;
        if (s == "cpu_fallback")     return ai::AdmissionPolicyKind::CpuFallback;
        return ai::AdmissionPolicyKind::FirstFit; // sentinel; caller checks
    }

    // parse_admission_policy returns a value; the caller checks for validity
    // by requiring an exact match. The returned kind on unknown string is
    // FirstFit — the caller validates by comparing the input string.
    //
    // NOTE: The function above needs to return result<AdmissionPolicyKind>
    // for proper validation. See REFACTOR note below.
};

const TomlSystemParserRegistration<AiAcceleratorConfigParser>
    kRegisterAiAcceleratorConfigParser;

} // namespace
} // namespace hpactor::config
```

**REFACTOR note:** The `parse_admission_policy` helper should return `result<ai::AdmissionPolicyKind>` to properly distinguish "valid enum string" from "unknown string." Adjust during the GREEN phase:

```cpp
  private:
    static result<ai::AdmissionPolicyKind>
    parse_admission_policy(std::string_view s) noexcept {
        if (s == "first_fit")        return ai::AdmissionPolicyKind::FirstFit;
        if (s == "most_free_memory") return ai::AdmissionPolicyKind::MostFreeMemory;
        if (s == "exact_device")     return ai::AdmissionPolicyKind::ExactDevice;
        if (s == "cpu_fallback")     return ai::AdmissionPolicyKind::CpuFallback;
        return error{errors::invalid_argument};
    }
```

The `error` type and `errors::invalid_argument` — verify the exact error construction API from `include/hpactor/types/types.hpp`. The project uses `result<T>` which may use `tl::expected` or a custom type. Use the existing pattern found in other parsers or error-returning code.

- [ ] **Step 4: Add parser source to build**

In `src/CMakeLists.txt`, within the `if(ENABLE_AI_ACCELERATORS)` block added in Task 1, add the parser:

```cmake
if(ENABLE_AI_ACCELERATORS)
    ai/accelerator_types.cpp
    ai/ai_message_registry.cpp
    config/parsers/ai_accelerator_config_parser.cpp
endif()
```

- [ ] **Step 5: GREEN — Build and verify**

```bash
ninja -C build hpactor_lib
```

Expected: builds successfully. Parser self-registers at static init time.

- [ ] **Step 6: Verify parser is registered**

The parser should appear in `TomlParserRegistry`. This will be confirmed in Task 7 when we write parser tests that actually invoke parsing. For now, verify no link errors and the build is clean.

- [ ] **Step 7: Commit**

```bash
git add src/config/parsers/ai_accelerator_config_parser.cpp src/CMakeLists.txt \
        tests/unit/config/test_ai_accelerator_config.cpp tests/unit/config/CMakeLists.txt
git commit -m "feat(ai): add self-registering TOML parser for [system.ai.accelerators]

Implement AiAcceleratorConfigParser at order 75 (after quarantine,
before metrics). Parses scalar fields: enabled, booleans, TTL values
with bounds validation, CPU memory budget with MB-to-bytes overflow
check, and admission_policy enum string validation. Returns early
(no-op) when table is absent or enabled=false.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: TOML Parser — Mock Device Array Parsing

**Files:**
- Modify: `src/config/parsers/ai_accelerator_config_parser.cpp` — add mock device parsing

**Purpose:** Extend the parser to handle `[[system.ai.accelerators.mock_device]]` table arrays with enum string validation, MB-to-bytes conversion, label parsing, and duplicate-id detection.

- [ ] **Step 1: RED — Add a test requiring mock device parsing**

In `tests/unit/config/test_ai_accelerator_config.cpp`, add:

```cpp
#include <hpactor/ai/accelerator_config.hpp>

TEST(MockDeviceConfig, Defaults) {
    hpactor::ai::MockDeviceConfig dev;
    EXPECT_EQ(dev.kind, hpactor::ai::DeviceKind::Mock);
    EXPECT_EQ(dev.vendor, hpactor::ai::DeviceVendor::Mock);
    EXPECT_EQ(dev.health, hpactor::ai::DeviceHealth::Healthy);
    EXPECT_FALSE(dev.exclusive_only);
    EXPECT_TRUE(dev.device_memory_bytes == 0u);
}
```

This test already passes (the types exist from AI-ACC-001A), but we'll add the parser-integration tests in Tasks 7-8. The RED step here is about having a test that will exercise the mock-device parser path.

- [ ] **Step 2: GREEN — Add mock device parsing to the parser**

In `src/config/parsers/ai_accelerator_config_parser.cpp`, at the end of the `parse()` method (before `return result<void>::make();` at the end of the enabled path), add:

```cpp
        // ── Mock devices ───────────────────────────────────────────
        std::unordered_set<std::string> seen_ids;
        accel.for_each_table_array("mock_device", [&](TomlTableView md) {
            ai::MockDeviceConfig dev;

            dev.id = md.read_string("id", "");
            if (dev.id.empty()) return;  // skip entries with empty id

            // Duplicate id detection
            if (!seen_ids.insert(dev.id).second) {
                // Duplicate — accumulate for post-loop error reporting.
                // Since for_each_table_array cannot return errors directly,
                // we mark the error and continue.
                duplicate_ids_.push_back(dev.id);
                return;
            }

            // Device kind
            auto kind_str = md.read_string("kind", "mock");
            auto kind = parse_device_kind(kind_str);
            if (!kind.has_value()) {
                invalid_fields_.push_back("mock_device." + dev.id +
                                          ": unknown kind '" + kind_str + "'");
                return;
            }
            dev.kind = kind.value();

            // Device vendor
            auto vendor_str = md.read_string("vendor", "mock");
            auto vendor = parse_device_vendor(vendor_str);
            if (!vendor.has_value()) {
                invalid_fields_.push_back("mock_device." + dev.id +
                                          ": unknown vendor '" + vendor_str + "'");
                return;
            }
            dev.vendor = vendor.value();

            dev.name = md.read_string("name", dev.id);

            // Memory MB → bytes
            uint32_t mem_mb = md.read_uint32("memory_mb", 0);
            if (mem_mb > 0) {
                constexpr uint64_t kMbToBytes = 1024ULL * 1024ULL;
                if (mem_mb > UINT64_MAX / kMbToBytes) {
                    invalid_fields_.push_back("mock_device." + dev.id +
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
                    invalid_fields_.push_back("mock_device." + dev.id +
                                              ": host_memory_mb overflow");
                    return;
                }
                dev.host_memory_bytes =
                    static_cast<uint64_t>(host_mb) * kMbToBytes;
            }

            dev.compute_units = md.read_uint32("compute_units", 0);
            dev.stream_slots = md.read_uint32("stream_slots", 0);
            dev.exclusive_only = md.read_bool("exclusive_only", false);

            // Health
            auto health_str = md.read_string("health", "healthy");
            auto health = parse_device_health(health_str);
            if (!health.has_value()) {
                invalid_fields_.push_back("mock_device." + dev.id +
                                          ": unknown health '" + health_str + "'");
                return;
            }
            dev.health = health.value();

            // Labels
            auto labels_table = md.table("labels");
            if (labels_table.valid()) {
                labels_table.for_each_entry([&](std::string_view key,
                                                TomlValueView val) {
                    ai::DeviceLabel label;
                    label.key = std::string(key);
                    label.value = val.as_string("");
                    dev.labels.push_back(std::move(label));
                });
            }

            cfg.mock_devices.push_back(std::move(dev));
        });

        // Check for accumulated errors from visitor callbacks
        if (!duplicate_ids_.empty()) {
            std::string msg = "ai.accelerators.mock_device: duplicate id(s):";
            for (auto& id : duplicate_ids_) msg += " " + id;
            return result<void>::make(error{errors::invalid_argument, msg});
        }
        if (!invalid_fields_.empty()) {
            std::string msg = "ai.accelerators.mock_device: invalid fields:";
            for (auto& f : invalid_fields_) {
                msg += " [" + f + "]";
            }
            return result<void>::make(error{errors::invalid_argument, msg});
        }

        return result<void>::make();
```

**Important:** This requires adding error-accumulation members to the parser class. Since the parser is stateless across calls (one `parse()` call per TOML document), we can use local variables. The `duplicate_ids_` and `invalid_fields_` should be local `std::vector<std::string>` variables inside `parse()`, not class members.

Add the helper methods to the parser class:

```cpp
  private:
    static result<ai::DeviceKind>
    parse_device_kind(std::string_view s) noexcept {
        if (s == "cpu")          return ai::DeviceKind::Cpu;
        if (s == "gpu")          return ai::DeviceKind::Gpu;
        if (s == "npu")          return ai::DeviceKind::Npu;
        if (s == "accelerator")  return ai::DeviceKind::Accelerator;
        if (s == "mock")         return ai::DeviceKind::Mock;
        return error{errors::invalid_argument};
    }

    static result<ai::DeviceVendor>
    parse_device_vendor(std::string_view s) noexcept {
        if (s == "unknown") return ai::DeviceVendor::Unknown;
        if (s == "nvidia")  return ai::DeviceVendor::Nvidia;
        if (s == "amd")     return ai::DeviceVendor::Amd;
        if (s == "apple")   return ai::DeviceVendor::Apple;
        if (s == "intel")   return ai::DeviceVendor::Intel;
        if (s == "mock")    return ai::DeviceVendor::Mock;
        return error{errors::invalid_argument};
    }

    static result<ai::DeviceHealth>
    parse_device_health(std::string_view s) noexcept {
        if (s == "unknown")      return ai::DeviceHealth::Unknown;
        if (s == "healthy")      return ai::DeviceHealth::Healthy;
        if (s == "degraded")     return ai::DeviceHealth::Degraded;
        if (s == "unavailable")  return ai::DeviceHealth::Unavailable;
        if (s == "lost")         return ai::DeviceHealth::Lost;
        return error{errors::invalid_argument};
    }
```

- [ ] **Step 3: GREEN — Build and verify**

```bash
ninja -C build hpactor_lib
```

Expected: builds successfully with mock device parsing.

- [ ] **Step 4: Commit**

```bash
git add src/config/parsers/ai_accelerator_config_parser.cpp
git commit -m "feat(ai): add mock device array parsing to AI accelerator TOML parser

Parse [[system.ai.accelerators.mock_device]] table arrays with:
- Device kind, vendor, health enum string validation
- MB-to-bytes conversion with overflow checks
- Label key-value pair parsing
- Duplicate id detection (fail the parse)

Unknown enum strings, overflow, and duplicate ids all produce
explicit parse errors — no silent fallback to defaults.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: TOML Test Fixtures

**Files:**
- Create: `tests/data/toml/ai/ai_accelerators_disabled.toml`
- Create: `tests/data/toml/ai/ai_accelerators_cpu.toml`
- Create: `tests/data/toml/ai/ai_accelerators_mock.toml`
- Create: `tests/data/toml/ai/ai_accelerators_invalid_enum.toml`
- Create: `tests/data/toml/ai/ai_accelerators_invalid_ttl.toml`
- Create: `tests/data/toml/ai/ai_accelerators_invalid_duplicate_id.toml`

**Purpose:** Provide TOML fixture files for parser tests. These are data files, not code — no TDD needed. Create all fixtures in one task.

- [ ] **Step 1: Create directory and disabled fixture**

```bash
mkdir -p tests/data/toml/ai
```

`tests/data/toml/ai/ai_accelerators_disabled.toml`:

```toml
[system]
scheduler_threads = 1

[system.ai.accelerators]
enabled = false
```

- [ ] **Step 2: Create CPU-only fixture**

`tests/data/toml/ai/ai_accelerators_cpu.toml`:

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

- [ ] **Step 3: Create mock devices fixture**

`tests/data/toml/ai/ai_accelerators_mock.toml`:

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

- [ ] **Step 4: Create invalid enum fixture**

`tests/data/toml/ai/ai_accelerators_invalid_enum.toml`:

```toml
[system]
scheduler_threads = 1

[system.ai.accelerators]
enabled = true
admission_policy = "round_robin"
```

- [ ] **Step 5: Create invalid TTL fixture**

`tests/data/toml/ai/ai_accelerators_invalid_ttl.toml`:

```toml
[system]
scheduler_threads = 1

[system.ai.accelerators]
enabled = true
lease_ttl_ms = 5000
min_lease_ttl_ms = 10000
max_lease_ttl_ms = 30000
```

- [ ] **Step 6: Create duplicate mock ID fixture**

`tests/data/toml/ai/ai_accelerators_invalid_duplicate_id.toml`:

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

- [ ] **Step 7: Commit**

```bash
git add tests/data/toml/ai/
git commit -m "test: add TOML fixtures for AI accelerator config parser

Six fixtures covering disabled, CPU-only, mock devices, invalid
admission_policy enum, invalid TTL bounds, and duplicate mock
device ids. Follows existing tests/data/toml/ conventions.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Parser Unit Tests — Disabled and CPU Configs

**Files:**
- Modify: `tests/unit/config/test_ai_accelerator_config.cpp` — full test implementation

**Purpose:** Implement parser tests for: disabled config (graceful no-op), absent table, CPU-only config with all scalar fields verified. Use the actual `TomlParser::parse()` entry point to exercise the full parse pipeline.

Note: Before writing tests, investigate how existing parser tests (e.g., `tests/unit/config/test_config_parser.cpp` or similar) invoke the parser. Determine:
1. Whether tests use inline TOML strings or fixture files
2. How `TomlParser::parse()` is called (static method? instance?)
3. How `TopologyModel` / `SystemDef` is accessed after parsing
4. What error types are returned

- [ ] **Step 1: RED — Write tests for disabled and CPU configs**

```cpp
// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/ai/accelerator_config.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/config/topology_model.hpp>

#include <gtest/gtest.h>
#include <cstdlib>
#include <string>

namespace {

std::string fixture_path(const std::string& name) {
    const char* data_dir = std::getenv("HPACTOR_TEST_DATA_DIR");
    if (!data_dir) data_dir = "tests/data";
    return std::string(data_dir) + "/toml/ai/" + name;
}

class AiAcceleratorConfigTest : public ::testing::Test {
  protected:
    hpactor::config::TopologyModel parse_fixture(const std::string& name) {
        auto result = hpactor::config::TomlParser::parse(fixture_path(name));
        EXPECT_TRUE(result.has_value()) << "Parse failed for: " << name;
        return std::move(result.value());
    }

    bool parse_fails(const std::string& name) {
        auto result = hpactor::config::TomlParser::parse(fixture_path(name));
        return !result.has_value();
    }
};

// ── Defaults ─────────────────────────────────────────────────────────

TEST_F(AiAcceleratorConfigTest, ConfigDefaults) {
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
    EXPECT_EQ(cfg.admission_policy,
              hpactor::ai::AdmissionPolicyKind::MostFreeMemory);
    EXPECT_EQ(cfg.cpu_host_memory_budget_bytes, 0u);
    EXPECT_EQ(cfg.cpu_compute_units, 0u);
    EXPECT_TRUE(cfg.mock_devices.empty());
}

// ── Disabled config ──────────────────────────────────────────────────

TEST_F(AiAcceleratorConfigTest, ParseDisabled) {
    auto model = parse_fixture("ai_accelerators_disabled.toml");
    EXPECT_FALSE(model.system.ai_accelerators.enabled);
    // All other fields at compile-time defaults
    EXPECT_TRUE(model.system.ai_accelerators.enable_cpu_probe);
    EXPECT_EQ(model.system.ai_accelerators.lease_ttl_ms, 30000u);
}

// ── CPU-only config ──────────────────────────────────────────────────

TEST_F(AiAcceleratorConfigTest, ParseCpuOnly) {
    auto model = parse_fixture("ai_accelerators_cpu.toml");
    auto& cfg = model.system.ai_accelerators;
    EXPECT_TRUE(cfg.enabled);
    EXPECT_TRUE(cfg.enable_cpu_probe);
    EXPECT_TRUE(cfg.allow_cpu_fallback);
    EXPECT_EQ(cfg.probe_interval_ms, 2000u);
    EXPECT_EQ(cfg.lease_ttl_ms, 60000u);
    EXPECT_EQ(cfg.min_lease_ttl_ms, 5000u);
    EXPECT_EQ(cfg.max_lease_ttl_ms, 120000u);
    EXPECT_EQ(cfg.admission_policy,
              hpactor::ai::AdmissionPolicyKind::FirstFit);
    EXPECT_EQ(cfg.cpu_host_memory_budget_bytes,
              16384ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(cfg.cpu_compute_units, 16u);
    EXPECT_TRUE(cfg.mock_devices.empty());
}

} // namespace
```

- [ ] **Step 2: RED — Build, verify tests fail on parsing issues**

```bash
ninja -C build tests/unit/config/test_ai_accelerator_config
```

Expected: builds. Tests may fail or pass depending on parser state. The `ConfigDefaults` test should pass (types exist from AI-ACC-001A). The parse tests exercise the new parser.

- [ ] **Step 3: GREEN — Iterate on parser until tests pass**

Run the tests:
```bash
./build/tests/unit/config/test_ai_accelerator_config
```

Fix any issues discovered:
- Parser not registered → verify `kOrder` and `kName` are correct
- Parse errors → verify TOML key paths are correct
- Wrong values → verify field mappings
- Build errors → verify includes and the `result<T>` / `error` API

- [ ] **Step 4: GREEN — All disabled and CPU tests pass**

Expected: 3/3 tests pass. `ConfigDefaults` verifies compile-time defaults. `ParseDisabled` verifies graceful no-op when `enabled = false`. `ParseCpuOnly` verifies all scalar fields parsed correctly including MB-to-bytes conversion.

- [ ] **Step 5: Commit**

```bash
git add tests/unit/config/test_ai_accelerator_config.cpp
git commit -m "test: add AI accelerator config parser tests — defaults, disabled, CPU

Verify compile-time defaults, graceful no-op when enabled=false,
and CPU-only config with all scalar fields including MB-to-bytes
conversion and admission_policy enum parsing.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: Parser Unit Tests — Mock Devices and Invalid Configs

**Files:**
- Modify: `tests/unit/config/test_ai_accelerator_config.cpp` — add more tests

**Purpose:** Test mock device parsing (including labels) and all invalid-input rejection paths.

- [ ] **Step 1: RED — Add mock device and invalid config tests**

Append to `tests/unit/config/test_ai_accelerator_config.cpp`:

```cpp
// ── Mock devices config ──────────────────────────────────────────────

TEST_F(AiAcceleratorConfigTest, ParseMockDevices) {
    auto model = parse_fixture("ai_accelerators_mock.toml");
    auto& cfg = model.system.ai_accelerators;
    EXPECT_TRUE(cfg.enabled);
    EXPECT_TRUE(cfg.enable_cpu_probe);
    EXPECT_EQ(cfg.admission_policy,
              hpactor::ai::AdmissionPolicyKind::MostFreeMemory);
    EXPECT_EQ(cfg.lease_ttl_ms, 30000u);

    ASSERT_EQ(cfg.mock_devices.size(), 2u);

    // First mock device
    auto& dev0 = cfg.mock_devices[0];
    EXPECT_EQ(dev0.id, "mock-gpu-0");
    EXPECT_EQ(dev0.kind, hpactor::ai::DeviceKind::Gpu);
    EXPECT_EQ(dev0.vendor, hpactor::ai::DeviceVendor::Mock);
    EXPECT_EQ(dev0.name, "Mock GPU 0");
    EXPECT_EQ(dev0.device_memory_bytes,
              24576ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(dev0.compute_units, 100u);
    EXPECT_EQ(dev0.stream_slots, 32u);
    EXPECT_EQ(dev0.health, hpactor::ai::DeviceHealth::Healthy);
    EXPECT_FALSE(dev0.exclusive_only);
    ASSERT_EQ(dev0.labels.size(), 2u);
    EXPECT_EQ(dev0.labels[0].key, "backend");
    EXPECT_EQ(dev0.labels[0].value, "mock");
    EXPECT_EQ(dev0.labels[1].key, "precision");
    EXPECT_EQ(dev0.labels[1].value, "fp16,bf16");

    // Second mock device
    auto& dev1 = cfg.mock_devices[1];
    EXPECT_EQ(dev1.id, "mock-gpu-1");
    EXPECT_EQ(dev1.kind, hpactor::ai::DeviceKind::Gpu);
    EXPECT_EQ(dev1.device_memory_bytes,
              12288ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(dev1.compute_units, 50u);
    EXPECT_EQ(dev1.stream_slots, 16u);
}

// ── Invalid configs ──────────────────────────────────────────────────

TEST_F(AiAcceleratorConfigTest, RejectInvalidAdmissionPolicy) {
    EXPECT_TRUE(parse_fails("ai_accelerators_invalid_enum.toml"));
}

TEST_F(AiAcceleratorConfigTest, RejectInvalidTtlBounds) {
    EXPECT_TRUE(parse_fails("ai_accelerators_invalid_ttl.toml"));
}

TEST_F(AiAcceleratorConfigTest, RejectDuplicateMockDeviceIds) {
    EXPECT_TRUE(parse_fails("ai_accelerators_invalid_duplicate_id.toml"));
}
```

- [ ] **Step 2: RED — Build, run tests, observe failures**

```bash
ninja -C build tests/unit/config/test_ai_accelerator_config
./build/tests/unit/config/test_ai_accelerator_config
```

Expected: mock device test may fail if mock device parsing has bugs. Invalid config tests should pass if the parser correctly rejects bad input.

- [ ] **Step 3: GREEN — Fix parser issues until all tests pass**

Common issues to fix:
- `for_each_table_array` key path — verify the TOML key is `"mock_device"` (singular), matching the `[[system.ai.accelerators.mock_device]]` TOML syntax (toml++ normalizes the singular form).
- Label ordering — TOML tables preserve insertion order; verify label key-value pairs match.
- Enum string validation — verify parse helpers return error for unknown strings.
- Duplicate id detection — verify the `unordered_set` correctly catches duplicates.
- MB-to-bytes conversion — verify the overflow guard works.

- [ ] **Step 4: GREEN — All 8 tests pass**

Expected output:
```
[==========] 8 tests from 1 test suite ran.
[  PASSED  ] 8 tests.
```

- [ ] **Step 5: Commit**

```bash
git add tests/unit/config/test_ai_accelerator_config.cpp
git commit -m "test: add mock device parsing and invalid config rejection tests

Verify mock device fields (kind, vendor, memory, compute, health,
labels), and confirm that invalid admission_policy, TTL bounds, and
duplicate mock device ids all produce parse errors.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: Integration Verification

**Files:**
- None new — verify all existing artifacts.

**Purpose:** Full build, run all AI tests, verify no regression in existing non-AI tests.

- [ ] **Step 1: Full reconfigure and build (AI ON)**

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build hpactor_lib
```

Expected: builds successfully, no warnings.

- [ ] **Step 2: Run AI unit tests (types from AI-ACC-001A)**

```bash
ninja -C build tests/unit/ai/test_accelerator_types
./build/tests/unit/ai/test_accelerator_types
```

Expected: all ~44 tests pass (AI-ACC-001A baseline unchanged).

- [ ] **Step 3: Run AI config parser tests (new in AI-ACC-001B)**

```bash
ninja -C build tests/unit/config/test_ai_accelerator_config
./build/tests/unit/config/test_ai_accelerator_config
```

Expected: all 8 tests pass.

- [ ] **Step 4: Run existing non-AI tests to verify no regression**

Select a representative set of existing test binaries that exercise the config subsystem and core framework:

```bash
ninja -C build tests/unit/core/test_unit_core
./build/tests/unit/core/test_unit_core
```

Expected: all tests pass — no regression from adding `ai_accelerators` to `Config`/`SystemDef`.

- [ ] **Step 5: Build with AI OFF, verify no breakage**

```bash
cmake -S . -B build-off -GNinja -DENABLE_AI_ACCELERATORS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF
ninja -C build-off hpactor_lib
```

Expected: builds successfully. AI sources excluded. The `#if HPACTOR_ENABLE_AI_ACCELERATORS` guards in `topology_model.hpp` and `actor_system.hpp` correctly elide the `ai_accelerators` field.

- [ ] **Step 6: Run non-AI tests with OFF build**

```bash
ninja -C build-off tests/unit/core/test_unit_core
./build-off/tests/unit/core/test_unit_core
```

Expected: all tests pass.

- [ ] **Step 7: Commit (if any fixups needed)**

```bash
git status
# If clean:
echo "All tasks complete."
```

---

## Post-Implementation Checklist

- [ ] `ENABLE_AI_ACCELERATORS` CMake option defaults to `ON`.
- [ ] `HPACTOR_ENABLE_AI_ACCELERATORS` is 1/0 in generated `hpactor_config.hpp`.
- [ ] `ENABLE_AI_ACCELERATORS=OFF` build succeeds; no AI code compiled.
- [ ] `SystemDef::ai_accelerators` and `Config::ai_accelerators` present when ON, absent when OFF.
- [ ] `load_topology()` copies `SystemDef::ai_accelerators` → `Config::ai_accelerators`.
- [ ] Parser self-registers at `kOrder = 75` via `TomlSystemParserRegistration`.
- [ ] Parser returns `result<void>::make()` when `[system.ai.accelerators]` table is absent.
- [ ] Parser returns `result<void>::make()` when `enabled = false`.
- [ ] Parser validates `admission_policy` enum strings — unknown values → parse error.
- [ ] Parser validates `min_lease_ttl_ms ≤ max_lease_ttl_ms` and `lease_ttl_ms ∈ [min, max]`.
- [ ] Parser validates unique mock-device ids.
- [ ] Parser validates MB-to-byte overflow for `cpu_host_memory_mb` and mock device `memory_mb`/`host_memory_mb`.
- [ ] Parser validates mock device `kind`, `vendor`, `health` enum strings.
- [ ] Parser ignores unknown keys (forward-compatible).
- [ ] Mock device labels parsed correctly as `DeviceLabel` key-value pairs.
- [ ] All 8 config parser tests pass.
- [ ] All ~44 AI-ACC-001A type tests still pass.
- [ ] Existing non-AI core tests pass with AI compiled in.
- [ ] No public header exposes `toml++`, protobuf internals, or vendor SDK types.
- [ ] Source compatibility preserved for existing actor APIs.
