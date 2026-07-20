#pragma once

#include <cstdint>

namespace apps::gossip_mesh {

enum class NodeRole : uint8_t {
    Alpha = 0,
    Beta = 1,
    Gamma = 2,
};

inline const char* to_string(NodeRole role) {
    switch (role) {
        case NodeRole::Alpha:
            return "alpha";
        case NodeRole::Beta:
            return "beta";
        case NodeRole::Gamma:
            return "gamma";
    }
    return "unknown";
}

struct ScenarioRunConfig {
    NodeRole role{NodeRole::Alpha};
    uint16_t base_gossip_port{15354};
    int single_scenario{0}; // 0 = run all, 1–7 = run one
    int advance_delay_ms{2500};
};

struct ScenarioSummary {
    int scenarios_run{0};
    int scenarios_passed{0};
    int scenarios_failed{0};
    bool all_passed() const {
        return scenarios_failed == 0;
    }
};

/// Main entry point for a single gossip mesh node process.
/// Returns 0 on success, non-zero on failure.
int run_mesh_node(const ScenarioRunConfig& config);

} // namespace apps::gossip_mesh
