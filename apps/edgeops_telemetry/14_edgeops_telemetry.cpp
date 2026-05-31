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

// HPActor Example 14: EdgeOps Telemetry Platform — IoT edge telemetry demo app.

#include <apps/edgeops_telemetry/scenario.hpp>

#include <hpactor/core/actor_system.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace edgeops = hpactor::apps::edgeops_telemetry;

namespace {

struct Options {
    std::string mode = "--help";
    edgeops::ScenarioKind scenario = edgeops::ScenarioKind::HappyPath;
    std::string host = "127.0.0.1";
    uint16_t actor_port = 17230;
    uint16_t registrar_port = 19154;
    uint32_t devices = 0;
    uint32_t readings_per_device = 0;
    uint32_t rate_per_second = 0;
    uint32_t storage_capacity = 0;
    bool query_fleet = false;
    bool query_device = false;
    bool query_alerts = false;
    bool query_storage = false;
    std::string query_device_id = "device-1";
};

std::atomic<bool> shutdown_requested{false};

void sigint_handler(int) {
    shutdown_requested.store(true, std::memory_order_release);
}

bool parse_u16(const std::string& value, uint16_t& out) {
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 || parsed > 65535)
        return false;
    out = static_cast<uint16_t>(parsed);
    return true;
}

bool parse_u32(const std::string& value, uint32_t& out) {
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed > UINT32_MAX)
        return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

void print_usage(const char* argv0) {
    std::cout << "HPActor Example 14: EdgeOps Telemetry Platform\n\n"
              << "All-in-one scenarios:\n"
              << "  " << argv0 << " --all-in-one --scenario happy-path\n"
              << "  " << argv0 << " --all-in-one --scenario overload\n"
              << "  " << argv0 << " --all-in-one --scenario missing-route\n\n"
              << "Role modes:\n"
              << "  " << argv0 << " --gateway --actor-port 17230\n"
              << "  " << argv0 << " --processor --actor-port 17231\n"
              << "  " << argv0 << " --storage --actor-port 17232\n"
              << "  " << argv0 << " --ops --actor-port 17233\n"
              << "  " << argv0
              << " --device-simulator --devices 100 --rate 50 --scenario "
                 "happy-path\n\n"
              << "Query mode:\n"
              << "  " << argv0 << " --query --fleet\n"
              << "  " << argv0 << " --query --device device-1\n"
              << "  " << argv0 << " --query --alerts\n"
              << "  " << argv0 << " --query --query-storage\n";
}

std::optional<Options> parse_args(int argc, char* argv[]) {
    Options opts;
    if (argc <= 1)
        return opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << flag << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--all-in-one" || arg == "--gateway" ||
            arg == "--processor" || arg == "--storage" || arg == "--ops" ||
            arg == "--device-simulator" || arg == "--query" || arg == "--help") {
            opts.mode = arg;
        } else if (arg == "--scenario") {
            const char* value = need_value("--scenario");
            if (value == nullptr)
                return std::nullopt;
            opts.scenario = edgeops::scenario_from_string(value);
        } else if (arg == "--host") {
            const char* value = need_value("--host");
            if (value == nullptr)
                return std::nullopt;
            opts.host = value;
        } else if (arg == "--actor-port") {
            const char* value = need_value("--actor-port");
            if (value == nullptr || !parse_u16(value, opts.actor_port))
                return std::nullopt;
        } else if (arg == "--registrar-port") {
            const char* value = need_value("--registrar-port");
            if (value == nullptr || !parse_u16(value, opts.registrar_port))
                return std::nullopt;
        } else if (arg == "--devices") {
            const char* value = need_value("--devices");
            if (value == nullptr || !parse_u32(value, opts.devices))
                return std::nullopt;
        } else if (arg == "--readings") {
            const char* value = need_value("--readings");
            if (value == nullptr || !parse_u32(value, opts.readings_per_device))
                return std::nullopt;
        } else if (arg == "--rate") {
            const char* value = need_value("--rate");
            if (value == nullptr || !parse_u32(value, opts.rate_per_second))
                return std::nullopt;
        } else if (arg == "--storage-capacity") {
            const char* value = need_value("--storage-capacity");
            if (value == nullptr || !parse_u32(value, opts.storage_capacity))
                return std::nullopt;
        } else if (arg == "--fleet") {
            opts.query_fleet = true;
        } else if (arg == "--alerts") {
            opts.query_alerts = true;
        } else if (arg == "--query-storage") {
            opts.query_storage = true;
        } else if (arg == "--storage") {
            opts.mode = "--storage";
        } else if (arg == "--device") {
            const char* value = need_value("--device");
            if (value == nullptr)
                return std::nullopt;
            opts.query_device = true;
            opts.query_device_id = value;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return std::nullopt;
        }
    }
    return opts;
}

edgeops::ScenarioRunConfig make_scenario_config(const Options& opts) {
    auto config = edgeops::default_scenario_config(opts.scenario);
    if (opts.devices != 0)
        config.device_count = opts.devices;
    if (opts.readings_per_device != 0)
        config.readings_per_device = opts.readings_per_device;
    if (opts.rate_per_second != 0)
        config.rate_per_second = opts.rate_per_second;
    if (opts.storage_capacity != 0)
        config.storage_capacity = opts.storage_capacity;
    return config;
}

void print_summary(const edgeops::ScenarioSummary& summary) {
    std::cout << "EDGEOPS RESULT scenario=" << edgeops::to_string(summary.scenario)
              << " status=" << edgeops::to_string(summary.status)
              << " devices=" << summary.devices_registered
              << " received=" << summary.readings_received
              << " normalized=" << summary.readings_normalized
              << " rejected=" << summary.readings_rejected
              << " stored=" << summary.readings_stored
              << " dropped=" << summary.readings_dropped
              << " rollups=" << summary.rollups_emitted
              << " alerts=" << summary.alerts_raised
              << " storage_peak=" << summary.storage_peak_depth << "/"
              << summary.storage_capacity << " dlq_depth=" << summary.dlq_depth
              << " dlq_pushed=" << summary.dlq_total_pushed
              << " actors=" << summary.actor_count
              << " workers=" << summary.scheduler_workers
              << " drained=" << (summary.drained ? "true" : "false")
              << " elapsed_ms=" << summary.elapsed_ms << "\n";
}

void print_query(const Options& opts, const edgeops::ScenarioSummary& summary) {
    if (opts.query_fleet ||
        (!opts.query_device && !opts.query_alerts && !opts.query_storage)) {
        auto fleet = edgeops::to_fleet_summary(summary);
        std::cout << "FLEET devices=" << fleet.devices_registered
                  << " disconnected=" << fleet.devices_disconnected
                  << " received=" << fleet.readings_received
                  << " normalized=" << fleet.readings_normalized
                  << " rejected=" << fleet.readings_rejected
                  << " stored=" << fleet.readings_stored
                  << " dropped=" << fleet.readings_dropped
                  << " rollups=" << fleet.rollups_emitted
                  << " alerts=" << fleet.alerts_raised << "\n";
    }
    if (opts.query_device) {
        std::cout
            << "DEVICE id=" << opts.query_device_id
            << " status=online scenario=" << edgeops::to_string(summary.scenario)
            << " readings=" << summary.readings_received << "\n";
    }
    if (opts.query_alerts) {
        std::cout << "ALERTS count=" << summary.alerts_raised << "\n";
    }
    if (opts.query_storage) {
        std::cout << "STORAGE stored=" << summary.readings_stored
                  << " dropped=" << summary.readings_dropped
                  << " peak=" << summary.storage_peak_depth << "/"
                  << summary.storage_capacity << "\n";
    }
}

int run_all_in_one(const Options& opts) {
    auto summary = edgeops::run_scenario(make_scenario_config(opts));
    print_summary(summary);
    return 0;
}

int run_query(const Options& opts) {
    auto summary = edgeops::run_scenario(make_scenario_config(opts));
    print_query(opts, summary);
    return 0;
}

hpactor::Config make_role_config(const Options& opts) {
    hpactor::Config config;
    config.enable_network = true;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint(
        opts.host + ":" + std::to_string(opts.actor_port));
    config.tcp_port = opts.actor_port;
    config.registrar.tcp_port = opts.registrar_port;
    config.registrar.udp_port = opts.registrar_port;
    config.dead_letters.enabled = true;
    config.cli.enabled = opts.mode == "--ops";
    return config;
}

int run_role(const Options& opts, edgeops::RoleKind role, const char* role_name) {
    hpactor::ActorSystem system(make_role_config(opts));
    std::signal(SIGINT, sigint_handler);
    std::cout << "EDGEOPS ROLE role=" << role_name << " endpoint="
              << hpactor::endpoint_ops::to_string(system.endpoint())
              << " registrar_port=" << opts.registrar_port << "\n";
    edgeops::spawn_role_actors(
        system, role, opts.storage_capacity == 0 ? 64 : opts.storage_capacity);
    while (!shutdown_requested.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "EDGEOPS ROLE role=" << role_name << " status=stopped\n";
    return 0;
}

int run_device_simulator(const Options& opts) {
    auto summary = edgeops::run_scenario(make_scenario_config(opts));
    std::cout << "EDGEOPS SIMULATOR devices=" << summary.devices_registered
              << " rate=" << opts.rate_per_second << "\n";
    print_summary(summary);
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);
    if (!opts) {
        print_usage(argv[0]);
        return 1;
    }
    if (opts->mode == "--help") {
        print_usage(argv[0]);
        return 0;
    }
    if (opts->mode == "--all-in-one")
        return run_all_in_one(*opts);
    if (opts->mode == "--query")
        return run_query(*opts);
    if (opts->mode == "--device-simulator")
        return run_device_simulator(*opts);
    if (opts->mode == "--gateway")
        return run_role(*opts, edgeops::RoleKind::Gateway, "gateway");
    if (opts->mode == "--processor")
        return run_role(*opts, edgeops::RoleKind::Processor, "processor");
    if (opts->mode == "--storage")
        return run_role(*opts, edgeops::RoleKind::Storage, "storage");
    if (opts->mode == "--ops")
        return run_role(*opts, edgeops::RoleKind::Ops, "ops");

    print_usage(argv[0]);
    return 1;
}
