#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace apps::name_resolution_mesh {

enum class NodeRole : uint8_t {
    Gateway   = 0,
    Payment   = 1,
    Inventory = 2,
};

inline const char* to_string(NodeRole role) {
    switch (role) {
        case NodeRole::Gateway:   return "gateway";
        case NodeRole::Payment:   return "payment";
        case NodeRole::Inventory: return "inventory";
    }
    return "unknown";
}

inline const char* service_name_for(NodeRole role) {
    switch (role) {
        case NodeRole::Gateway:   return "auth";
        case NodeRole::Payment:   return "payment";
        case NodeRole::Inventory: return "inventory";
    }
    return "unknown";
}

struct ScenarioRunConfig {
    NodeRole    role{NodeRole::Gateway};
    uint16_t    base_port{10001};
    int         single_scenario{0};  // 0 = run all, 1-7 = run one
    int         advance_delay_ms{2500};
};

struct ScenarioSummary {
    int  scenarios_run{0};
    int  scenarios_passed{0};
    int  scenarios_failed{0};
    bool all_passed() const { return scenarios_failed == 0; }
};

/// Main entry point for a single mesh node process.
/// Returns 0 on success, non-zero on failure.
int run_mesh_node(const ScenarioRunConfig& config);

}  // namespace apps::name_resolution_mesh
