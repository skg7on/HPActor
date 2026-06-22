// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// =============================================================================
// HPActor Example 18: Cluster Control Plane Demo
// =============================================================================
//
// A comprehensive demo of HPActor's Cluster subsystem for building a
// distributed, high-reliability control plane.
//
// This app models a distributed rate-limiting control plane where:
//   - ClusterFailureModel tracks node health across simulated nodes
//   - A RateLimitOrchestrator singleton fails over via OldestNodeElection
//   - TenantRateLimiter actors are sharded via RendezvousHash placement
//   - Policy updates are delivered reliably with ACK/NACK via OutboundTracker
//   - Rate-limit counters are durable via EventSourcedBehavior
//   - Partition policies (FailOpen/FailClosed) govern availability
//   - Route invalidation fires on node Down/Quarantined/Removed
//
// Cluster subsystems exercised:
//   CLU-001  Cluster Failure Model & Fencing
//   CLU-002  Cluster Sharding & Placement
//   CLU-003  Cluster Singleton
//   MSG-003  Reliable Messaging (ACK/NACK/Retry)
//   DUR-001  DurableBehavior (snapshot-based)
//   DUR-002  EventSourcedBehavior (event sourcing)
//
// Modes:
//   --all-in-one          Single-process simulated 3-node cluster.
//                         scheduler_start_paused + manual drain.
//   --scenario <name>     happy-path | node-failure | partition | recovery
//
// Tuning:
//   --nodes <N>           Simulated nodes (default 3)
//   --shards <N>          Total shards (default 6)
//   --tenants <N>         Tenant rate-limiters (default 4)
//   --requests <N>        Requests per tenant (default 10)
//
// Quickstart:
//   ./18_cluster_control_plane --all-in-one
//   ./18_cluster_control_plane --all-in-one --scenario node-failure
//   ./18_cluster_control_plane --all-in-one --scenario partition
//   ./18_cluster_control_plane --all-in-one --scenario recovery
// =============================================================================

#include <apps/cluster_control_plane/messages.hpp>
#include <apps/cluster_control_plane/scenario.hpp>

#include <hpactor/cluster/partition_policy.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace cc = hpactor::apps::cluster_control_plane;

// =============================================================================
// Options
// =============================================================================

struct Options {
    std::string mode = "--all-in-one";
    std::string scenario_name = "happy-path";
    uint32_t nodes = 3;
    uint32_t shards = 6;
    uint32_t tenants = 4;
    uint32_t requests = 10;
    std::string policy = "fail-open";
    bool no_durable = false;
    bool no_reliable = false;
    bool help = false;
};

// =============================================================================
// Usage
// =============================================================================

static void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "\n"
        << "HPActor Cluster Control Plane Demo\n"
        << "Demonstrates building a distributed control plane with HPActor's\n"
        << "Cluster subsystem: failure model, sharding, singleton, reliable\n"
        << "messaging, durable state, partition handling.\n"
        << "\n"
        << "Modes:\n"
        << "  --all-in-one         Single-process simulated cluster (default)\n"
        << "\n"
        << "Scenarios:\n"
        << "  --scenario <name>    happy-path | node-failure | partition | recovery\n"
        << "                       (default: happy-path)\n"
        << "\n"
        << "Tuning:\n"
        << "  --nodes <N>          Simulated cluster nodes (default 3)\n"
        << "  --shards <N>         Total shards (default 6)\n"
        << "  --tenants <N>        Tenant rate-limiters to shard (default 4)\n"
        << "  --requests <N>       Rate-limit checks per tenant (default 10)\n"
        << "  --policy <name>      Partition policy: fail-open | fail-closed\n"
        << "                       (default: fail-open)\n"
        << "  --no-durable         Disable durable state via EventSourcedBehavior\n"
        << "  --no-reliable        Disable reliable delivery via OutboundTracker\n"
        << "  --help               Print this help\n"
        << std::endl;
}

// =============================================================================
// Argument parsing
// =============================================================================

static std::optional<Options> parse_args(int argc, char* argv[]) {
    Options opts;
    std::vector<std::string_view> args(argv + 1, argv + argc);

    for (size_t i = 0; i < args.size(); ++i) {
        auto arg = args[i];

        if (arg == "--help" || arg == "-h") {
            opts.help = true;
            return opts;
        }
        if (arg == "--all-in-one") {
            opts.mode = "--all-in-one";
        } else if (arg == "--scenario") {
            if (i + 1 < args.size())
                opts.scenario_name = args[++i];
            else {
                std::cerr << "Error: --scenario requires a value\n";
                return std::nullopt;
            }
        } else if (arg == "--nodes") {
            if (i + 1 < args.size())
                opts.nodes =
                    static_cast<uint32_t>(std::stoul(std::string(args[++i])));
            else {
                std::cerr << "Error: --nodes requires a value\n";
                return std::nullopt;
            }
        } else if (arg == "--shards") {
            if (i + 1 < args.size())
                opts.shards =
                    static_cast<uint32_t>(std::stoul(std::string(args[++i])));
            else {
                std::cerr << "Error: --shards requires a value\n";
                return std::nullopt;
            }
        } else if (arg == "--tenants") {
            if (i + 1 < args.size())
                opts.tenants =
                    static_cast<uint32_t>(std::stoul(std::string(args[++i])));
            else {
                std::cerr << "Error: --tenants requires a value\n";
                return std::nullopt;
            }
        } else if (arg == "--requests") {
            if (i + 1 < args.size())
                opts.requests =
                    static_cast<uint32_t>(std::stoul(std::string(args[++i])));
            else {
                std::cerr << "Error: --requests requires a value\n";
                return std::nullopt;
            }
        } else if (arg == "--policy") {
            if (i + 1 < args.size())
                opts.policy = args[++i];
            else {
                std::cerr << "Error: --policy requires a value\n";
                return std::nullopt;
            }
        } else if (arg == "--no-durable") {
            opts.no_durable = true;
        } else if (arg == "--no-reliable") {
            opts.no_reliable = true;
        } else if (arg.find("--") == 0) {
            std::cerr << "Unknown option: " << arg << "\n";
            return std::nullopt;
        }
    }

    return opts;
}

// =============================================================================
// All-in-one runner
// =============================================================================

static int run_all_in_one(const Options& opts) {
    auto kind = cc::scenario_from_string(opts.scenario_name);

    auto config = cc::default_scenario_config(kind);
    config.num_nodes = opts.nodes;
    config.num_shards = opts.shards;
    config.num_tenants = opts.tenants;
    config.requests_per_tenant = opts.requests;
    config.enable_durable_state = !opts.no_durable;
    config.enable_reliable_delivery = !opts.no_reliable;

    std::cout << "══════════════════════════════════════════════════════\n";
    std::cout << " HPActor Cluster Control Plane Demo\n";
    std::cout << "══════════════════════════════════════════════════════\n";
    std::cout << "  mode:                all-in-one\n";
    std::cout << "  scenario:            " << opts.scenario_name << "\n";
    std::cout << "  nodes:               " << config.num_nodes << "\n";
    std::cout << "  shards:              " << config.num_shards << "\n";
    std::cout << "  tenants:             " << config.num_tenants << "\n";
    std::cout << "  requests/tenant:     " << config.requests_per_tenant << "\n";
    std::cout << "  durable state:       "
              << (config.enable_durable_state ? "on" : "off") << "\n";
    std::cout << "  reliable delivery:   "
              << (config.enable_reliable_delivery ? "on" : "off") << "\n";
    std::cout << "══════════════════════════════════════════════════════\n\n";

    auto summary = cc::run_scenario(config);
    summary.print();

    if (summary.status.find("error") != std::string::npos)
        return 1;
    return 0;
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);

    if (!opts) {
        print_usage(argv[0]);
        return 1;
    }

    if (opts->help) {
        print_usage(argv[0]);
        return 0;
    }

    if (opts->mode == "--all-in-one") {
        return run_all_in_one(*opts);
    }

    std::cerr << "Unknown mode: " << opts->mode << "\n";
    print_usage(argv[0]);
    return 1;
}
