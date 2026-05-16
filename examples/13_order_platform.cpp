// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <examples/order_platform/messages.hpp>

#include <hpactor/core/actor_system.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace order = hpactor::examples::order_platform;

namespace {

struct Options {
    std::string mode = "--help";
    order::ScenarioKind scenario = order::ScenarioKind::HappyPath;
    std::string host = "127.0.0.1";
    std::string registrar_host = "127.0.0.1";
    std::string payment_endpoint;
    uint16_t actor_port = 17130;
    uint16_t http_port = 18130;
    uint16_t registrar_port = 19153;
    uint16_t gateway_port = 18130;
    bool submit_demo_order = false;
};

std::atomic<bool> shutdown_requested{false};

void sigint_handler(int) {
    shutdown_requested.store(true, std::memory_order_release);
}

bool parse_port(const std::string& value, uint16_t& port) {
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0 || parsed > 65535)
        return false;
    port = static_cast<uint16_t>(parsed);
    return true;
}

void print_usage(const char* argv0) {
    std::cout
        << "HPActor Example 13: Multi-Role Order Platform\n\n"
        << "Quickstart:\n"
        << "  " << argv0 << " --all-in-one --scenario happy-path\n\n"
        << "Distributed:\n"
        << "  " << argv0 << " --payment --actor-port 17132\n"
        << "  " << argv0 << " --gateway --actor-port 17130 --http-port 18130 "
        << "--payment 127.0.0.1:17132\n"
        << "  " << argv0 << " --query --gateway-port 18130 --submit demo-order\n\n"
        << "Failure scenarios:\n"
        << "  " << argv0 << " --all-in-one --scenario overload\n"
        << "  " << argv0 << " --all-in-one --scenario payment-decline\n";
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

        if (arg == "--payment" && opts.mode == "--gateway" && i + 1 < argc &&
            argv[i + 1][0] != '-') {
            opts.payment_endpoint = argv[++i];
        } else if (arg == "--all-in-one" || arg == "--gateway" ||
                   arg == "--inventory" || arg == "--payment" ||
                   arg == "--fulfillment" || arg == "--ops" ||
                   arg == "--query" || arg == "--help") {
            opts.mode = arg;
        } else if (arg == "--scenario") {
            const char* value = need_value("--scenario");
            if (value == nullptr)
                return std::nullopt;
            opts.scenario = order::scenario_from_string(value);
        } else if (arg == "--actor-port") {
            const char* value = need_value("--actor-port");
            if (value == nullptr || !parse_port(value, opts.actor_port))
                return std::nullopt;
        } else if (arg == "--http-port") {
            const char* value = need_value("--http-port");
            if (value == nullptr || !parse_port(value, opts.http_port))
                return std::nullopt;
        } else if (arg == "--gateway-port") {
            const char* value = need_value("--gateway-port");
            if (value == nullptr || !parse_port(value, opts.gateway_port))
                return std::nullopt;
        } else if (arg == "--registrar-port") {
            const char* value = need_value("--registrar-port");
            if (value == nullptr || !parse_port(value, opts.registrar_port))
                return std::nullopt;
        } else if (arg == "--registrar-host") {
            const char* value = need_value("--registrar-host");
            if (value == nullptr)
                return std::nullopt;
            opts.registrar_host = value;
        } else if (arg == "--submit") {
            const char* value = need_value("--submit");
            if (value == nullptr)
                return std::nullopt;
            opts.submit_demo_order = std::string(value) == "demo-order";
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return std::nullopt;
        }
    }
    return opts;
}

hpactor::Config make_base_config(const Options& opts, uint16_t actor_port) {
    hpactor::Config config;
    config.scheduler_threads = 4;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint(
        opts.host + ":" + std::to_string(actor_port));
    config.tcp_port = actor_port;
    config.enable_network = actor_port != 0;
    config.registrar.tcp_port = opts.registrar_port;
    config.registrar.udp_port = opts.registrar_port;
    config.mailbox.default_capacity =
        opts.scenario == order::ScenarioKind::Overload ? 2 : 1024;
    config.dead_letters.enabled = true;
    config.dead_letters.capacity = 128;
    config.tracing.enabled = true;
    config.tracing.exporter = hpactor::tracing::TraceExporterKind::kJsonFile;
    config.tracing.json_file_path = "build/order-platform-traces.jsonl";
    config.cli.enabled = false;
    return config;
}

void run_until_signal(const char* role) {
    std::signal(SIGINT, sigint_handler);
    std::cout << role << " running. Press Ctrl-C to stop.\n";
    while (!shutdown_requested.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

int run_all_in_one(const Options& opts) {
    hpactor::Config config = make_base_config(opts, 0);
    config.enable_network = false;
    hpactor::ActorSystem system(config);
    std::cout << "ALL-IN-ONE scenario=" << order::to_string(opts.scenario) << "\n";
    std::cout << "actor_count=" << system.actor_count() << "\n";
    return 0;
}

int run_long_role(const Options& opts, const char* role) {
    hpactor::ActorSystem system(make_base_config(opts, opts.actor_port));
    std::cout << role << " endpoint="
              << hpactor::endpoint_ops::to_string(system.endpoint()) << "\n";
    run_until_signal(role);
    return 0;
}

int run_query(const Options& opts) {
    if (!opts.submit_demo_order) {
        std::cout << "QUERY requires --submit demo-order\n";
        return 1;
    }
    std::cout << "QUERY would submit demo-order to HTTP port "
              << opts.gateway_port << "\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    auto opts = parse_args(argc, argv);
    if (!opts.has_value() || opts->mode == "--help") {
        print_usage(argv[0]);
        return opts.has_value() ? 0 : 1;
    }

    if (opts->mode == "--all-in-one")
        return run_all_in_one(*opts);
    if (opts->mode == "--query")
        return run_query(*opts);
    if (opts->mode == "--gateway")
        return run_long_role(*opts, "GATEWAY");
    if (opts->mode == "--inventory")
        return run_long_role(*opts, "INVENTORY");
    if (opts->mode == "--payment")
        return run_long_role(*opts, "PAYMENT");
    if (opts->mode == "--fulfillment")
        return run_long_role(*opts, "FULFILLMENT");
    if (opts->mode == "--ops")
        return run_long_role(*opts, "OPS");

    print_usage(argv[0]);
    return 1;
}
