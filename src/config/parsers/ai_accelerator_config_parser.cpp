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
    static constexpr int kOrder = 15;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto ai = system.table("ai");
        if (!ai.valid())
            return result<void>::make();

        auto accel = ai.table("accelerators");
        if (!accel.valid())
            return result<void>::make();

        auto& cfg = out.ai_accelerators;

        // ── enabled gate ──────────────────────────────────────────
        cfg.enabled = accel.read_bool("enabled", false);
        if (!cfg.enabled)
            return result<void>::make();

        // ── Top-level booleans ────────────────────────────────────
        cfg.enable_cpu_probe = accel.read_bool("enable_cpu_probe", true);
        cfg.allow_cpu_fallback = accel.read_bool("allow_cpu_fallback", true);
        cfg.allow_empty_inventory = accel.read_bool("allow_empty_inventory", false);
        cfg.require_resource_plane_ready =
            accel.read_bool("require_resource_plane_ready", false);

        // ── TTL values with bounds validation ────────────────────
        cfg.lease_ttl_ms = accel.read_uint32("lease_ttl_ms", 30000);
        cfg.min_lease_ttl_ms = accel.read_uint32("min_lease_ttl_ms", 1000);
        cfg.max_lease_ttl_ms = accel.read_uint32("max_lease_ttl_ms", 300000);

        if (cfg.min_lease_ttl_ms > cfg.max_lease_ttl_ms) {
            return result<void>::make(
                error(errors::invalid_argument,
                      "ai.accelerators: min_lease_ttl_ms (" +
                          std::to_string(cfg.min_lease_ttl_ms) +
                          ") must be <= max_lease_ttl_ms (" +
                          std::to_string(cfg.max_lease_ttl_ms) + ")"));
        }
        if (cfg.lease_ttl_ms < cfg.min_lease_ttl_ms ||
            cfg.lease_ttl_ms > cfg.max_lease_ttl_ms) {
            return result<void>::make(error(
                errors::invalid_argument,
                "ai.accelerators: lease_ttl_ms (" + std::to_string(cfg.lease_ttl_ms) +
                    ") must be within [min_lease_ttl_ms (" +
                    std::to_string(cfg.min_lease_ttl_ms) + "), max_lease_ttl_ms (" +
                    std::to_string(cfg.max_lease_ttl_ms) + ")]"));
        }

        // ── Other scalar values ───────────────────────────────────
        cfg.probe_interval_ms = accel.read_uint32("probe_interval_ms", 1000);
        cfg.missing_device_grace_ms =
            accel.read_uint32("missing_device_grace_ms", 5000);

        // ── CPU budget (MB → bytes, no overflow possible for uint32_t) ─
        uint32_t cpu_mem_mb = accel.read_uint32("cpu_host_memory_mb", 0);
        if (cpu_mem_mb > 0) {
            constexpr uint64_t kMbToBytes = 1024ULL * 1024ULL;
            cfg.cpu_host_memory_budget_bytes =
                static_cast<uint64_t>(cpu_mem_mb) * kMbToBytes;
        }
        cfg.cpu_compute_units = accel.read_uint32("cpu_compute_units", 0);

        // ── Admission policy (enum string → validated) ───────────
        auto policy_str = accel.read_string("admission_policy", "most_free_memory");
        auto policy = parse_admission_policy(policy_str);
        if (!policy.has_value()) {
            return result<void>::make(error(
                errors::invalid_argument,
                "ai.accelerators: unknown admission_policy '" + policy_str + "'"));
        }
        cfg.admission_policy = policy.value();

        // ── Mock devices ───────────────────────────────────────────
        std::unordered_set<std::string> seen_ids;
        std::vector<std::string> invalid_fields;
        std::vector<std::string> duplicate_ids;

        accel.for_each_table_array("mock_device", [&](TomlTableView md) {
            ai::MockDeviceConfig dev;

            dev.id = md.read_string("id", "");
            if (dev.id.empty())
                return;

            if (!seen_ids.insert(dev.id).second) {
                duplicate_ids.push_back(dev.id);
                return;
            }

            // Device kind
            auto kind_str = md.read_string("kind", "mock");
            auto kind = parse_device_kind(kind_str);
            if (!kind.has_value()) {
                invalid_fields.push_back("mock_device." + dev.id +
                                         ": unknown kind '" + kind_str + "'");
                return;
            }
            dev.kind = kind.value();

            // Device vendor
            auto vendor_str = md.read_string("vendor", "mock");
            auto vendor = parse_device_vendor(vendor_str);
            if (!vendor.has_value()) {
                invalid_fields.push_back("mock_device." + dev.id +
                                         ": unknown vendor '" + vendor_str + "'");
                return;
            }
            dev.vendor = vendor.value();

            dev.name = md.read_string("name", dev.id);

            // Memory MB → bytes (no overflow possible for uint32_t)
            constexpr uint64_t kMbToBytes = 1024ULL * 1024ULL;
            uint32_t mem_mb = md.read_uint32("memory_mb", 0);
            if (mem_mb > 0)
                dev.device_memory_bytes = static_cast<uint64_t>(mem_mb) * kMbToBytes;

            uint32_t host_mb = md.read_uint32("host_memory_mb", 0);
            if (host_mb > 0)
                dev.host_memory_bytes = static_cast<uint64_t>(host_mb) * kMbToBytes;

            dev.compute_units = md.read_uint32("compute_units", 0);
            dev.stream_slots = md.read_uint32("stream_slots", 0);
            dev.exclusive_only = md.read_bool("exclusive_only", false);

            // Health
            auto health_str = md.read_string("health", "healthy");
            auto health = parse_device_health(health_str);
            if (!health.has_value()) {
                invalid_fields.push_back("mock_device." + dev.id +
                                         ": unknown health '" + health_str + "'");
                return;
            }
            dev.health = health.value();

            // Labels
            auto labels_table = md.table("labels");
            if (labels_table.valid()) {
                labels_table.for_each_entry(
                    [&](std::string_view key, TomlValueView val) {
                        ai::DeviceLabel label;
                        label.key = std::string(key);
                        label.value = val.as_string("");
                        dev.labels.push_back(std::move(label));
                    });
            }

            cfg.mock_devices.push_back(std::move(dev));
        });

        // Check for accumulated errors
        if (!duplicate_ids.empty()) {
            std::string msg = "ai.accelerators.mock_device: duplicate id(s):";
            for (auto& id : duplicate_ids)
                msg += " " + id;
            return result<void>::make(error(errors::invalid_argument, msg));
        }
        if (!invalid_fields.empty()) {
            std::string msg = "ai.accelerators.mock_device: invalid fields:";
            for (auto& f : invalid_fields)
                msg += " [" + f + "]";
            return result<void>::make(error(errors::invalid_argument, msg));
        }

        return result<void>::make();
    }

  private:
    static result<ai::AdmissionPolicyKind>
    parse_admission_policy(std::string_view s) noexcept {
        if (s == "first_fit")
            return result<ai::AdmissionPolicyKind>::make(
                ai::AdmissionPolicyKind::FirstFit);
        if (s == "most_free_memory")
            return result<ai::AdmissionPolicyKind>::make(
                ai::AdmissionPolicyKind::MostFreeMemory);
        if (s == "exact_device")
            return result<ai::AdmissionPolicyKind>::make(
                ai::AdmissionPolicyKind::ExactDevice);
        if (s == "cpu_fallback")
            return result<ai::AdmissionPolicyKind>::make(
                ai::AdmissionPolicyKind::CpuFallback);
        return result<ai::AdmissionPolicyKind>::make(error(errors::invalid_argument));
    }

    static result<ai::DeviceKind> parse_device_kind(std::string_view s) noexcept {
        if (s == "cpu")
            return result<ai::DeviceKind>::make(ai::DeviceKind::Cpu);
        if (s == "gpu")
            return result<ai::DeviceKind>::make(ai::DeviceKind::Gpu);
        if (s == "npu")
            return result<ai::DeviceKind>::make(ai::DeviceKind::Npu);
        if (s == "accelerator")
            return result<ai::DeviceKind>::make(ai::DeviceKind::Accelerator);
        if (s == "mock")
            return result<ai::DeviceKind>::make(ai::DeviceKind::Mock);
        return result<ai::DeviceKind>::make(error(errors::invalid_argument));
    }

    static result<ai::DeviceVendor>
    parse_device_vendor(std::string_view s) noexcept {
        if (s == "unknown")
            return result<ai::DeviceVendor>::make(ai::DeviceVendor::Unknown);
        if (s == "nvidia")
            return result<ai::DeviceVendor>::make(ai::DeviceVendor::Nvidia);
        if (s == "amd")
            return result<ai::DeviceVendor>::make(ai::DeviceVendor::Amd);
        if (s == "apple")
            return result<ai::DeviceVendor>::make(ai::DeviceVendor::Apple);
        if (s == "intel")
            return result<ai::DeviceVendor>::make(ai::DeviceVendor::Intel);
        if (s == "mock")
            return result<ai::DeviceVendor>::make(ai::DeviceVendor::Mock);
        return result<ai::DeviceVendor>::make(error(errors::invalid_argument));
    }

    static result<ai::DeviceHealth>
    parse_device_health(std::string_view s) noexcept {
        if (s == "unknown")
            return result<ai::DeviceHealth>::make(ai::DeviceHealth::Unknown);
        if (s == "healthy")
            return result<ai::DeviceHealth>::make(ai::DeviceHealth::Healthy);
        if (s == "degraded")
            return result<ai::DeviceHealth>::make(ai::DeviceHealth::Degraded);
        if (s == "unavailable")
            return result<ai::DeviceHealth>::make(ai::DeviceHealth::Unavailable);
        if (s == "lost")
            return result<ai::DeviceHealth>::make(ai::DeviceHealth::Lost);
        return result<ai::DeviceHealth>::make(error(errors::invalid_argument));
    }
};

const TomlSystemParserRegistration<AiAcceleratorConfigParser> kRegisterAiAcceleratorConfigParser;

} // namespace
} // namespace hpactor::config
