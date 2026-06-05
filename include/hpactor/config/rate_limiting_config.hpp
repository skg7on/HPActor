#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::config {

/// \brief Per-actor rate limit specification from TOML config.
struct PerActorRateLimitSpec {
    bool enabled{false};
    double rate_per_sec{0.0};
    uint32_t burst{0};
};

/// \brief Admission rule specification from TOML config.
struct AdmissionRuleSpec {
    bool enabled{false};
    std::vector<uint32_t> type_allowed_tags;
    std::vector<uint32_t> type_blocked_tags;
    std::vector<uint64_t> sender_blocked_ids;
    uint32_t priority_min{0};
    uint64_t size_max_bytes{0};
    double per_sender_rate{0.0};
    uint32_t per_sender_burst{0};
    uint32_t max_senders{256};
};

/// \brief System-level rate limiting and admission configuration.
struct RateLimitingConfig {
    PerActorRateLimitSpec system_default;
    AdmissionRuleSpec system_admission;
};

} // namespace hpactor::config
