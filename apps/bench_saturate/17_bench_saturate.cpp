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
// HPActor App 17: Bench Saturate -- Actor System Saturation Benchmark
// =============================================================================

#include "actors/saturate_collector_actor.hpp"
#include "actors/saturate_coordinator_actor.hpp"
#include "actors/saturate_receiver_actor.hpp"
#include "actors/saturate_sender_actor.hpp"

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/cli_local_actor.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace bench_saturate = hpactor::apps::bench_saturate;

// =============================================================================
// System Probe
// =============================================================================

static void probe_system() {
    // Collect system info.
    unsigned int logical_cores = std::thread::hardware_concurrency();
    std::string cpu_detail;
#if defined(__APPLE__)
    int perf_cores = 0, eff_cores = 0;
    FILE* fp = popen("sysctl -n hw.perflevel0.logicalcpu 2>/dev/null", "r");
    if (fp) {
        char buf[32];
        if (fgets(buf, sizeof(buf), fp))
            perf_cores = atoi(buf);
        pclose(fp);
    }
    fp = popen("sysctl -n hw.perflevel1.logicalcpu 2>/dev/null", "r");
    if (fp) {
        char buf[32];
        if (fgets(buf, sizeof(buf), fp))
            eff_cores = atoi(buf);
        pclose(fp);
    }
    if (perf_cores > 0 && eff_cores > 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), " (%dP + %dE)", perf_cores, eff_cores);
        cpu_detail = buf;
    }

    int l1d = 0, l2 = 0;
    fp = popen("sysctl -n hw.l1dcachesize 2>/dev/null", "r");
    if (fp) {
        char buf[32];
        if (fgets(buf, sizeof(buf), fp))
            l1d = atoi(buf);
        pclose(fp);
    }
    fp = popen("sysctl -n hw.l2cachesize 2>/dev/null", "r");
    if (fp) {
        char buf[32];
        if (fgets(buf, sizeof(buf), fp))
            l2 = atoi(buf);
        pclose(fp);
    }
#endif

    long page_size = sysconf(_SC_PAGESIZE);
    long phys_pages = sysconf(_SC_PHYS_PAGES);
    uint64_t total_mem = 0;
    if (phys_pages > 0 && page_size > 0)
        total_mem =
            static_cast<uint64_t>(phys_pages) * static_cast<uint64_t>(page_size);

    // Render with fixed-width columns so values and descriptions align.
    constexpr int kW = 62; // inner box width
    char buf[256];

    auto hr = [] { std::cout << "+" << std::string(kW, '-') << "+\n"; };
    auto blank = [] { std::cout << "|" << std::string(kW, ' ') << "|\n"; };
    // inner box = 62.  row: 2 + 18 + 1 + 41 = 62 between borders.
    auto row = [](const char* label, const char* value) {
        printf("|  %-18s %-41s|\n", label, value);
    };
    // presets: 4 + 18 + 1 + 39 = 62 between borders.
    auto presets = [](const char* name, const char* desc) {
        printf("|    %-18s %-39s|\n", name, desc);
    };

    std::cout << "\n";
    hr();
    printf("| %-61s|\n", "HPActor App 17 -- Bench Saturate");
    printf("| %-61s|\n", "Actor System Saturation Benchmark");
    hr();
    blank();

    snprintf(buf, sizeof(buf), "%u logical%s", logical_cores, cpu_detail.c_str());
    row("CPU Cores:", buf);

#if defined(__APPLE__)
    if (l1d > 0) {
        snprintf(buf, sizeof(buf), "%d KB", l1d / 1024);
        row("L1d Cache:", buf);
    }
    if (l2 > 0) {
        snprintf(buf, sizeof(buf), "%d KB", l2 / 1024);
        row("L2 Cache:", buf);
    }
#endif

    if (total_mem > 0) {
        snprintf(buf, sizeof(buf), "%llu GB",
                 static_cast<unsigned long long>(total_mem >> 30));
        row("Memory:", buf);
    }

    blank();
    printf("| %-61s|\n", "Presets:");
    presets("quick-saturate", "100->10, 16B, fast ceiling find ~30s");
    presets("deep-saturate", "1000->100, 16B, thorough curve ~60s");
    presets("alloc-stress", "500->50, 1KB-64KB junk, alloc pressure");
    presets("mixed-load", "500->50, 80/20 mixed, realistic");
    presets("fan-in-extreme", "5000->1, 16B, extreme contention");
    presets("fan-out-burst", "10->1000, 1KB-16KB junk, broad fan-out");
    blank();

    printf("| %-61s|\n", "Try:");
    presets("/saturate list", "see all presets");
    presets("/saturate start", "start <preset> run");
    presets("/saturate status", "check progress");
    presets("/saturate report", "view results");
    presets("/quit", "exit");
    blank();

    hr();
    std::cout << std::endl;
}

// =============================================================================
// Headless mode
// =============================================================================

static int run_headless(const std::string& preset_name, const std::string& format,
                        const std::string& output_path) {
    hpactor::Config config;
    config.scheduler_threads = 8;
    config.max_queue_depth = 4096;
    config.mailbox.default_capacity = 16384;
    config.cli = hpactor::cli::CliConfig{};

    hpactor::ActorSystem system(config);

    auto coordinator = system.spawn<bench_saturate::SaturateCoordinatorActor>();
    auto collector = system.spawn<bench_saturate::SaturateCollectorActor>();

    auto* coord_raw = static_cast<bench_saturate::SaturateCoordinatorActor*>(
        coordinator.get().get());
    auto* coll_raw = static_cast<bench_saturate::SaturateCollectorActor*>(
        collector.get().get());

    const auto& presets = coord_raw->presets();
    const bench_saturate::SaturatePreset* preset = nullptr;
    for (auto& p : presets) {
        if (p.name == preset_name) {
            preset = &p;
            break;
        }
    }
    if (!preset) {
        std::cerr << "Error: unknown preset '" << preset_name << "'\n";
        return 1;
    }

    uint32_t num_senders = preset->num_senders;
    uint32_t num_receivers = preset->num_receivers;
    auto collector_addr = collector.address();

    std::vector<hpactor::ActorAddress> receiver_addrs;
    receiver_addrs.reserve(num_receivers);
    for (uint32_t i = 0; i < num_receivers; ++i) {
        auto r =
            system.spawn<bench_saturate::SaturateReceiverActor>(collector_addr, i);
        receiver_addrs.push_back(r.address());
    }

    std::vector<hpactor::ActorAddress> sender_addrs;
    sender_addrs.reserve(num_senders);
    for (uint32_t i = 0; i < num_senders; ++i) {
        auto s = system.spawn<bench_saturate::SaturateSenderActor>(
            collector_addr, receiver_addrs, i);
        sender_addrs.push_back(s.address());
    }

    coord_raw->set_sender_addrs(sender_addrs);
    coord_raw->set_receiver_addrs(receiver_addrs);
    coord_raw->set_collector_addr(collector_addr);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Start the preset run
    hpactor::StreamBuffer payload(
        reinterpret_cast<const uint8_t*>(preset_name.data()),
        reinterpret_cast<const uint8_t*>(preset_name.data() + preset_name.size()));
    system.deliver_local(coordinator.id(),
                         hpactor::TypedMessage(hpactor::TypeTag{0x00010200},
                                               std::move(payload)));

    // Wait for the coordinator to start processing
    auto start_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (!coord_raw->is_running() &&
           std::chrono::steady_clock::now() < start_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Wait for completion
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(preset->duration_max_ms + 5000);
    while (coord_raw->is_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    // Let stop messages propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto state = coll_raw->serialize_state();
    std::string state_str(state.begin(), state.end());

    std::map<std::string, std::string> kv;
    {
        std::istringstream iss(state_str);
        std::string line;
        while (std::getline(iss, line)) {
            auto eq = line.find('=');
            if (eq != std::string::npos)
                kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }

    std::string output;
    if (format == "csv") {
        std::ostringstream csv;
        csv << "total_sent,total_received,total_dropped,drop_rate_pct,"
            << "p50_us,p99_us,p999_us,throughput_msgps,elapsed_ms\n";
        csv << kv["total_sent"] << "," << kv["total_received"] << ","
            << kv["total_dropped"] << "," << kv["drop_rate_pct"] << ","
            << kv["p50_us"] << "," << kv["p99_us"] << "," << kv["p999_us"]
            << "," << kv["throughput_msgps"] << "," << kv["elapsed_ms"] << "\n";
        output = csv.str();
    } else {
        std::ostringstream json;
        json << "{\n";
        bool first = true;
        for (auto& [k, v] : kv) {
            if (!first)
                json << ",\n";
            json << "  \"" << k << "\": \"" << v << "\"";
            first = false;
        }
        json << "\n}\n";
        output = json.str();
    }

    if (!output_path.empty()) {
        std::ofstream out(output_path);
        out << output;
    } else {
        std::cout << output;
    }

    system.shutdown();
    return 0;
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    std::string headless_preset;
    std::string headless_format = "json";
    std::string headless_output;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0 && i + 1 < argc) {
            headless_preset = argv[++i];
        } else if (std::strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            headless_format = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            headless_output = argv[++i];
        }
    }

    if (!headless_preset.empty()) {
        return run_headless(headless_preset, headless_format, headless_output);
    }

    probe_system();

    hpactor::Config config;
    config.scheduler_threads = 8;
    config.max_queue_depth = 4096;

    config.cli = hpactor::cli::CliConfig{};
    config.cli.enabled = true;
    config.cli.listen_path = "";
    config.cli.tcp_port = 0;
    config.cli.default_format = "pretty";
    config.cli.page_size = 20;
    config.cli.history_path = "";
    config.cli.history_max = 1000;

    config.mailbox.default_capacity = 16384;

    config.shutdown_drain = hpactor::DrainConfig{
        hpactor::DrainPolicy::Drain, std::chrono::milliseconds{10'000}};

    hpactor::ActorSystem system(config);

    auto coordinator = system.spawn<bench_saturate::SaturateCoordinatorActor>();
    auto collector = system.spawn<bench_saturate::SaturateCollectorActor>();

    auto* coord_raw = static_cast<bench_saturate::SaturateCoordinatorActor*>(
        coordinator.get().get());

    constexpr uint32_t kMaxSenders = 5000;
    constexpr uint32_t kMaxReceivers = 1000;

    auto collector_addr = collector.address();

    // Pre-create the maximum pool size so that actors exist before the
    // scheduler begins steady-state operation.  The coordinator gates
    // which subset participates via active_senders_/active_receivers_
    // so only the preset's count actually sends/receives messages.
    std::vector<hpactor::ActorAddress> receiver_addrs;
    receiver_addrs.reserve(kMaxReceivers);
    for (uint32_t i = 0; i < kMaxReceivers; ++i) {
        auto r =
            system.spawn<bench_saturate::SaturateReceiverActor>(collector_addr, i);
        receiver_addrs.push_back(r.address());
    }

    std::vector<hpactor::ActorAddress> sender_addrs;
    sender_addrs.reserve(kMaxSenders);
    for (uint32_t i = 0; i < kMaxSenders; ++i) {
        auto s = system.spawn<bench_saturate::SaturateSenderActor>(
            collector_addr, receiver_addrs, i);
        sender_addrs.push_back(s.address());
    }

    coord_raw->set_sender_addrs(sender_addrs);
    coord_raw->set_receiver_addrs(receiver_addrs);
    coord_raw->set_collector_addr(collector_addr);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    while (system.cli_actor() && system.cli_actor()->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nInitiating graceful shutdown..." << std::endl;
    auto shutdown_result = system.shutdown();
    if (shutdown_result.has_value()) {
        std::cout << "Shutdown complete." << std::endl;
    } else {
        std::cout << "Shutdown timed out -- forcing exit." << std::endl;
    }

    std::cout << "=== Bench Saturate Complete ===" << std::endl;
    return 0;
}
