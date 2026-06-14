// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/ai/accelerator_config.hpp>
#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/types/types.hpp>

#include <string>

namespace hpactor::config {
namespace {

class AiAcceleratorConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.ai.accelerators";
    static constexpr int kOrder = 75;

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
};

const TomlSystemParserRegistration<AiAcceleratorConfigParser> kRegisterAiAcceleratorConfigParser;

} // namespace
} // namespace hpactor::config
