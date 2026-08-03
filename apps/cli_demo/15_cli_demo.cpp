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
// HPActor App 15: CLI Interactive Demo — Comprehensive CLI Command Showcase
// =============================================================================
//
// A full-featured demo app that exercises EVERY CLI command in the HPActor
// interactive CLI by creating actors configured with production features:
//
//   10 actors across 8 types:
//     - 4 x WorkerActor       — periodic task processing (every 100ms)
//     - 1 x AggregatorActor   — collects results, running stats
//     - 1 x HealthCheckActor  — pings workers (every 500ms)
//     - 1 x BroadcastActor    — config broadcasts (every 1s)
//     - 1 x ClockActor        — logical clock, time queries
//     - 1 x LogActor          — ring-buffer event log
//     - 1 x SystemMonitorActor — system-wide stats (every 2s)
//     - 1 x DlqDemoActor      — generates DLQ records (every 3s)
//
//   Production features demonstrated per-actor:
//     Worker-1: rate_limiter=100msg/s
//     Worker-2: rate_limiter=500msg/s
//     Worker-3: circuit_breaker + quarantine enabled
//     Worker-4: delivery failure generation + quarantine enabled
//     DlqDemoActor: DLQ record generation + circuit breaker
//     All: bounded mailboxes (256 msg) — /actor <id> show, /metrics show
//
//   CLI commands exercised (all produce meaningful output):
//     /actor <id> show       — metadata, mailbox, state for any actor
//     /actor <id> circuit    — circuit breaker state (Worker-3, DlqDemoActor)
//     /actor <id> rate       — rate limiter state (Worker-1, Worker-2)
//     /actor <id> admission  — admission policy state
//     /actor <id> delivery   — delivery result counters (Worker-4)
//     /actor <id> delivery-stats — delivery ratios
//     /actor <id> quarantine — manually quarantine an actor
//     /actor <id> unquarantine — release from quarantine
//     /actor <id> kill       — kill a specific actor
//     /actor list            — enumerate all actors with optional filter
//     /dlq list              — list dead-letter records
//     /dlq show              — inspect a specific DLQ record
//     /dlq replay            — replay a DLQ record
//     /dlq export            — export DLQ records (text or JSON)
//     /fault status          — show fault injection status
//     /fault list            — list all registered fault injection points
//     /fault clear           — clear fault schedule
//     /failure reasons       — list all canonical failure reasons
//     /failure summary       — failure subsystem status
//     /system uptime         — show actor system uptime
//     /system stats          — system-wide statistics
//     /system memory         — memory subsystem status
//     /system list           — list all system actors
//     /system drain          — initiate graceful node shutdown
//     /system drain/status   — show shutdown progress
//     /system stop <id>      — gracefully stop a specific actor
//     /system endpoints      — list known endpoints
//     /system endpoint/<ep>/show — endpoint detail
//     /system endpoint/<ep>/circuit/reset — reset endpoint circuit breaker
//     /ask pending           — list in-flight ask requests
//     /ask cancel            — cancel an ask request
//     /ask stats             — ask manager statistics
//     /metrics show          — metrics snapshot (hpactor_mailbox_* gauges)
//     /topology show         — topology tree
//     /help                  — show available commands
//     /quit                  — exit CLI and trigger graceful shutdown
//
// =============================================================================

#include "actors/aggregator_actor.hpp"
#include "actors/broadcast_actor.hpp"
#include "actors/clock_actor.hpp"
#include "actors/dlq_demo_actor.hpp"
#include "actors/health_check_actor.hpp"
#include "actors/log_actor.hpp"
#include "actors/query_actor.hpp"
#include "actors/system_monitor_actor.hpp"
#include "actors/worker_actor.hpp"
#include "messages.hpp"

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/cli/actor/cli_local_actor.hpp>
#include <hpactor/cli/config/cli_config.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace cli_demo = hpactor::apps::cli_demo;

// =============================================================================
// Utility: deliver a start message to an actor from outside the actor system
// =============================================================================

static void
send_to_actor(hpactor::ActorSystem& system, hpactor::ActorId target,
              hpactor::TypeTag tag, hpactor::StreamBuffer payload = {}) {
    system.deliver_local(target, hpactor::TypedMessage(tag, std::move(payload)));
}

// =============================================================================
// Splash screen
// =============================================================================

static void print_splash() {
    std::cout
        << "\n"
        << "╔══════════════════════════════════════════════════════════════╗\n"
        << "║     HPActor App 15 — CLI Interactive Demo                    ║\n"
        << "║     Comprehensive CLI Command Showcase                       ║\n"
        << "╠══════════════════════════════════════════════════════════════╣\n"
        << "║                                                              ║\n"
        << "║  Architecture:                                               ║\n"
        << "║    10 actors across 8 types                                  ║\n"
        << "║    4 scheduler threads with A2WS work-stealing               ║\n"
        << "║                                                              ║\n"
        << "║  Production Features Enabled:                                ║\n"
        << "║    • Rate limiting (Worker-1: 100msg/s, Worker-2: 500msg/s)  ║\n"
        << "║    • Circuit breaker + quarantine (Worker-3, DlqDemoActor)   ║\n"
        << "║    • Delivery failure generation (Worker-4)                  ║\n"
        << "║    • DLQ record generation (DlqDemoActor)                    ║\n"
        << "║    • Fault injection hooks (80 sites across 14 domains)      ║\n"
        << "║    • Bounded mailboxes (256 msg, DeadLetter overflow)        ║\n"
        << "║    • Graceful shutdown (30s drain timeout)                   ║\n"
        << "║                                                              ║\n"
        << "║  Try these CLI commands:                                     ║\n"
        << "║    /help                — see all available commands         ║\n"
        << "║    /actor list          — list all 10 actors                 ║\n"
        << "║    /actor <id> show     — inspect any actor                  ║\n"
        << "║    /actor <id> circuit  — circuit breaker (Worker-3, DLQ)    ║\n"
        << "║    /actor <id> rate     — rate limiter (Worker-1, Worker-2)  ║\n"
        << "║    /actor <id> delivery — delivery counters (Worker-4)       ║\n"
        << "║    /metrics show        — Prometheus metrics + mailbox obs   ║\n"
        << "║    /dlq list            — dead-letter queue records          ║\n"
        << "║    /fault status        — fault injection system status      ║\n"
        << "║    /failure reasons     — canonical failure reasons          ║\n"
        << "║    /system stats        — system-wide statistics             ║\n"
        << "║    /system uptime       — actor system uptime                ║\n"
        << "║    /scheduler workers   — threads & idle model (polling/CV)  ║\n"
        << "║    /quit                — graceful shutdown                  ║\n"
        << "║                                                              ║\n"
        << "╚══════════════════════════════════════════════════════════════╝\n"
        << std::endl;
}

// =============================================================================
// Main
// =============================================================================

int main() {
    print_splash();

    // ── Configure: 4 threads, CLI enabled, bounded mailboxes, DLQ ────────

    hpactor::Config config;
    config.scheduler_threads = 4;
    config.max_queue_depth = 1024;

    // CLI: enabled with pretty formatting, paging every 20 lines
    config.cli = hpactor::cli::CliConfig{.enabled = true,
                                         .listen_path = "",
                                         .tcp_port = 0,
                                         .default_format = "pretty",
                                         .page_size = 20,
                                         .history_path = "",
                                         .history_max = 1000};

    // Bounded mailboxes: 256 msg capacity, overflow → DeadLetter queue
    config.mailbox.default_capacity = 256;
    config.mailbox.default_policy = hpactor::mailbox::OverflowPolicy::DeadLetter;
    config.dead_letters.capacity = 1024;

    // Graceful shutdown: drain in-flight work for up to 30s, then force-stop
    config.shutdown_drain = hpactor::DrainConfig{
        hpactor::DrainPolicy::Drain, std::chrono::milliseconds{30'000}};

    // IMPORTANT: ActorSystem construction starts the CliActor daemon thread
    // which takes over stdin/stdout. All diagnostic output must happen BEFORE
    // this point, or be routed through the CLI formatter.
    hpactor::ActorSystem system(config);

    // ── Spawn all actors (silently — CliActor owns stdout now) ───────────

    auto log_actor = system.spawn<cli_demo::LogActor>();
    auto clock = system.spawn<cli_demo::ClockActor>();
    auto aggregator = system.spawn<cli_demo::AggregatorActor>();
    auto monitor = system.spawn<cli_demo::SystemMonitorActor>();
    auto health_check = system.spawn<cli_demo::HealthCheckActor>();
    auto broadcast = system.spawn<cli_demo::BroadcastActor>();
    auto dlq_demo = system.spawn<cli_demo::DlqDemoActor>();
    auto query_actor = system.spawn<cli_demo::QueryActor>();

    // ── Spawn 4 workers with different configurations ────────────────────

    std::vector<std::shared_ptr<cli_demo::WorkerActor>> workers;

    // Worker-1: Rate limiter 100 msg/s
    {
        cli_demo::WorkerConfig cfg;
        cfg.worker_id = 1;
        cfg.aggregator_id = aggregator.id();
        cfg.log_id = log_actor.id();
        cfg.rate_limit = 100.0;
        cfg.rate_burst = 10;
        auto w = system.spawn<cli_demo::WorkerActor>(cfg);
        workers.push_back(std::static_pointer_cast<cli_demo::WorkerActor>(
            system.get_actor(w.id())));
    }

    // Worker-2: Rate limiter 500 msg/s
    {
        cli_demo::WorkerConfig cfg;
        cfg.worker_id = 2;
        cfg.aggregator_id = aggregator.id();
        cfg.log_id = log_actor.id();
        cfg.rate_limit = 500.0;
        cfg.rate_burst = 50;
        auto w = system.spawn<cli_demo::WorkerActor>(cfg);
        workers.push_back(std::static_pointer_cast<cli_demo::WorkerActor>(
            system.get_actor(w.id())));
    }

    // Worker-3: Circuit breaker + quarantine enabled
    {
        cli_demo::WorkerConfig cfg;
        cfg.worker_id = 3;
        cfg.aggregator_id = aggregator.id();
        cfg.log_id = log_actor.id();
        cfg.rate_limit = 0.0; // no rate limiting
        cfg.quarantine_enabled = true;
        auto w = system.spawn<cli_demo::WorkerActor>(cfg);
        workers.push_back(std::static_pointer_cast<cli_demo::WorkerActor>(
            system.get_actor(w.id())));
    }

    // Worker-4: Delivery failure generation + quarantine enabled
    {
        cli_demo::WorkerConfig cfg;
        cfg.worker_id = 4;
        cfg.aggregator_id = aggregator.id();
        cfg.log_id = log_actor.id();
        cfg.rate_limit = 0.0; // no rate limiting
        cfg.quarantine_enabled = true;
        cfg.generate_delivery_failures = true;
        auto w = system.spawn<cli_demo::WorkerActor>(cfg);
        workers.push_back(std::static_pointer_cast<cli_demo::WorkerActor>(
            system.get_actor(w.id())));
    }

    // ── Wire up addresses (set after spawn so addresses are known) ──────

    for (auto& w : workers) {
        w->set_aggregator_addr(aggregator.address());
        w->set_log_addr(log_actor.address());
    }

    auto* health_raw = std::static_pointer_cast<cli_demo::HealthCheckActor>(
                           system.get_actor(health_check.id()))
                           .get();
    auto* broadcast_raw = std::static_pointer_cast<cli_demo::BroadcastActor>(
                              system.get_actor(broadcast.id()))
                              .get();

    std::vector<hpactor::ActorAddress> worker_addrs;
    for (auto& w : workers) {
        health_raw->add_worker(w->address());
        broadcast_raw->add_worker(w->address());
        worker_addrs.push_back(w->address());
    }

    // Give DlqDemoActor the worker addresses for overflow generation
    auto* dlq_raw = std::static_pointer_cast<cli_demo::DlqDemoActor>(
                        system.get_actor(dlq_demo.id()))
                        .get();
    dlq_raw->set_target_actors(worker_addrs);

    auto* query_raw = std::static_pointer_cast<cli_demo::QueryActor>(
                          system.get_actor(query_actor.id()))
                          .get();
    query_raw->set_clock_addr(clock.address());

    // Rate limiters are configured automatically in WorkerActor::set_mailbox().
    // Workers with rate_limit > 0 (Worker-1 at 100msg/s, Worker-2 at 500msg/s)
    // will have their mailbox rate limiters set during spawn initialization.

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── Kick off periodic work ──────────────────────────────────────────

    for (auto& w : workers) {
        send_to_actor(system, w->id(), cli_demo::StartTag);
    }
    send_to_actor(system, health_check.id(), cli_demo::StartTag);
    send_to_actor(system, broadcast.id(), cli_demo::StartTag);
    send_to_actor(system, monitor.id(), cli_demo::StartTag);
    send_to_actor(system, clock.id(), cli_demo::PeriodicTickTag);
    send_to_actor(system, dlq_demo.id(), cli_demo::StartTag);
    send_to_actor(system, query_actor.id(), cli_demo::StartTag);

    // Let the actors initialize before the user starts typing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── Main loop: wait for CLI exit ────────────────────────────────────

    // Keep the main thread alive. The CliActor's DaemonActor thread handles
    // stdin. When the user types /quit or sends EOF, the CLI loop exits and
    // is_running() returns false. On exit, initiate graceful shutdown.
    while (system.cli_actor() && system.cli_actor()->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nInitiating graceful shutdown..." << std::endl;
    auto shutdown_result = system.shutdown();
    if (shutdown_result.has_value()) {
        std::cout << "Shutdown complete — all actors drained." << std::endl;
    } else {
        std::cout << "Shutdown timed out — forcing exit." << std::endl;
    }

    std::cout << "=== Demo Complete ===" << std::endl;
    return 0;
}
