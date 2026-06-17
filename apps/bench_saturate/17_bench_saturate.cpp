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
// HPActor App 17: Bench Saturate — Actor System Saturation Benchmark
// =============================================================================

#include "actors/saturate_collector_actor.hpp"
#include "actors/saturate_coordinator_actor.hpp"
#include "actors/saturate_receiver_actor.hpp"
#include "actors/saturate_sender_actor.hpp"
#include "messages.hpp"

#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/cli_local_actor.hpp>
#include <hpactor/core/actor_system.hpp>

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
    std::cout << "\n";
    std::cout
        << "+--------------------------------------------------------------+\n";
    std::cout << "|     HPActor App 17 — Bench Saturate                         |\n";
    std::cout << "|     Actor System Saturation Benchmark                       |\n";
    std::cout
        << "+--------------------------------------------------------------+\n";
    std::cout
        << "|                                                              |\n";

    unsigned int logical_cores = std::thread::hardware_concurrency();
    std::cout << "|  CPU Cores:     " << logical_cores << " logical";

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
    if (perf_cores > 0 && eff_cores > 0)
        std::cout << " (" << perf_cores << "P + " << eff_cores << "E)";
#endif
    std::cout << "\n";

#if defined(__APPLE__)
    fp = popen("sysctl -n hw.l1dcachesize 2>/dev/null", "r");
    int l1d = 0;
    if (fp) {
        char buf[32];
        if (fgets(buf, sizeof(buf), fp))
            l1d = atoi(buf);
        pclose(fp);
    }
    fp = popen("sysctl -n hw.l2cachesize 2>/dev/null", "r");
    int l2 = 0;
    if (fp) {
        char buf[32];
        if (fgets(buf, sizeof(buf), fp))
            l2 = atoi(buf);
        pclose(fp);
    }
    if (l1d > 0)
        std::cout << "|  L1d Cache:     " << (l1d / 1024) << " KB\n";
    if (l2 > 0)
        std::cout << "|  L2 Cache:      " << (l2 / 1024) << " KB\n";
#endif

    long page_size = sysconf(_SC_PAGESIZE);
    long phys_pages = sysconf(_SC_PHYS_PAGES);
    if (phys_pages > 0 && page_size > 0) {
        uint64_t total_mem =
            static_cast<uint64_t>(phys_pages) * static_cast<uint64_t>(page_size);
        std::cout << "|  Memory:        " << (total_mem >> 30) << " GB\n";
    }

    std::cout
        << "|                                                              |\n";
    std::cout
        << "|  Presets:                                                    |\n";
    std::cout << "|    quick-saturate — 100→10, 16B, fast ceiling find ~30s     |\n";
    std::cout << "|    deep-saturate  — 1000→100, 16B, thorough curve ~60s     |\n";
    std::cout << "|    alloc-stress   — 500→50, 1KB-64KB junk, alloc pressure   |\n";
    std::cout
        << "|    mixed-load     — 500→50, 80/20 mixed, realistic           |\n";
    std::cout << "|    fan-in-extreme — 5000→1, 16B, extreme contention         |\n";
    std::cout << "|    fan-out-burst  — 10→1000, 1KB-16KB junk, broad fan-out   |\n";
    std::cout
        << "|                                                              |\n";
    std::cout
        << "|  Try:                                                        |\n";
    std::cout << "|    /saturate list              — see all presets            |\n";
    std::cout << "|    /saturate start quick-saturate — fast ceiling find       |\n";
    std::cout << "|    /saturate status            — check progress             |\n";
    std::cout << "|    /saturate report            — view results               |\n";
    std::cout << "|    /quit                       — exit                       |\n";
    std::cout
        << "|                                                              |\n";
    std::cout
        << "+--------------------------------------------------------------+\n";
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
        std::cout << "Shutdown timed out — forcing exit." << std::endl;
    }

    std::cout << "=== Bench Saturate Complete ===" << std::endl;
    return 0;
}
