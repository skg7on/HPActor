#include <apps/name_resolution_mesh/scenario.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --role <gateway|payment|inventory> [options]\n"
              << "Options:\n"
              << "  --role ROLE           Node role (required)\n"
              << "  --base-port PORT      Starting port (default: 10001)\n"
              << "  --scenario SCENARIO   Run a single scenario (1-7). "
                 "Default: run all\n"
              << "  --delay-ms MS         Advance delay between scenarios "
                 "(default: 2500)\n"
              << "  --help                Show this help\n";
}

apps::name_resolution_mesh::NodeRole parse_role(const char* s) {
    if (std::strcmp(s, "gateway") == 0)
        return apps::name_resolution_mesh::NodeRole::Gateway;
    if (std::strcmp(s, "payment") == 0)
        return apps::name_resolution_mesh::NodeRole::Payment;
    if (std::strcmp(s, "inventory") == 0)
        return apps::name_resolution_mesh::NodeRole::Inventory;
    std::cerr << "Invalid role: " << s
              << " (expected gateway|payment|inventory)\n";
    std::exit(1);
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace apps::name_resolution_mesh;

    ScenarioRunConfig config;
    bool has_role = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            config.role = parse_role(argv[++i]);
            has_role = true;
        } else if (std::strcmp(argv[i], "--base-port") == 0 && i + 1 < argc) {
            config.base_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            config.single_scenario = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
            config.advance_delay_ms = std::atoi(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!has_role) {
        std::cerr << "Error: --role is required\n";
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "Name Resolution Mesh — Node: " << to_string(config.role)
              << ", Port: " << (config.base_port
                        + static_cast<uint16_t>(config.role))
              << std::endl;

    return run_mesh_node(config);
}
