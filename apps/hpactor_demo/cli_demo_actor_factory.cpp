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

#include "cli_demo_actor_factory.hpp"

#include "apps/cli_demo/actors/aggregator_actor.hpp"
#include "apps/cli_demo/actors/broadcast_actor.hpp"
#include "apps/cli_demo/actors/clock_actor.hpp"
#include "apps/cli_demo/actors/dlq_demo_actor.hpp"
#include "apps/cli_demo/actors/health_check_actor.hpp"
#include "apps/cli_demo/actors/log_actor.hpp"
#include "apps/cli_demo/actors/query_actor.hpp"
#include "apps/cli_demo/actors/system_monitor_actor.hpp"
#include "apps/cli_demo/actors/worker_actor.hpp"
#include "apps/cli_demo/messages.hpp"

#include <hpactor/core/actor_system.hpp>

#include <thread>
#include <vector>

namespace hpactor::apps::cli_demo {

namespace {

void deliver_local(ActorSystem& system, ActorId target, TypeTag tag,
                   StreamBuffer payload = {}) {
    system.deliver_local(target, TypedMessage(tag, std::move(payload)));
}

} // namespace

CliDemoActors spawn_cli_demo_actors(ActorSystem& system) {
    CliDemoActors a;

    // System actors
    a.log = std::static_pointer_cast<LogActor>(
        system.get_actor(system.spawn<LogActor>().id()));
    a.clock = std::static_pointer_cast<ClockActor>(
        system.get_actor(system.spawn<ClockActor>().id()));
    a.aggregator = std::static_pointer_cast<AggregatorActor>(
        system.get_actor(system.spawn<AggregatorActor>().id()));
    a.monitor = std::static_pointer_cast<SystemMonitorActor>(
        system.get_actor(system.spawn<SystemMonitorActor>().id()));
    a.health_check = std::static_pointer_cast<HealthCheckActor>(
        system.get_actor(system.spawn<HealthCheckActor>().id()));
    a.broadcast = std::static_pointer_cast<BroadcastActor>(
        system.get_actor(system.spawn<BroadcastActor>().id()));
    a.dlq_demo = std::static_pointer_cast<DlqDemoActor>(
        system.get_actor(system.spawn<DlqDemoActor>().id()));
    a.query = std::static_pointer_cast<QueryActor>(
        system.get_actor(system.spawn<QueryActor>().id()));

    // Worker-1: Rate limiter 100 msg/s
    {
        WorkerConfig cfg;
        cfg.worker_id = 1;
        cfg.aggregator_id = a.aggregator->id();
        cfg.log_id = a.log->id();
        cfg.rate_limit = 100.0;
        cfg.rate_burst = 10;
        a.workers[0] = std::static_pointer_cast<WorkerActor>(
            system.get_actor(system.spawn<WorkerActor>(cfg).id()));
    }

    // Worker-2: Rate limiter 500 msg/s
    {
        WorkerConfig cfg;
        cfg.worker_id = 2;
        cfg.aggregator_id = a.aggregator->id();
        cfg.log_id = a.log->id();
        cfg.rate_limit = 500.0;
        cfg.rate_burst = 50;
        a.workers[1] = std::static_pointer_cast<WorkerActor>(
            system.get_actor(system.spawn<WorkerActor>(cfg).id()));
    }

    // Worker-3: Circuit breaker + quarantine enabled
    {
        WorkerConfig cfg;
        cfg.worker_id = 3;
        cfg.aggregator_id = a.aggregator->id();
        cfg.log_id = a.log->id();
        cfg.rate_limit = 0.0;
        cfg.quarantine_enabled = true;
        a.workers[2] = std::static_pointer_cast<WorkerActor>(
            system.get_actor(system.spawn<WorkerActor>(cfg).id()));
    }

    // Worker-4: Delivery failure generation + quarantine enabled
    {
        WorkerConfig cfg;
        cfg.worker_id = 4;
        cfg.aggregator_id = a.aggregator->id();
        cfg.log_id = a.log->id();
        cfg.rate_limit = 0.0;
        cfg.quarantine_enabled = true;
        cfg.generate_delivery_failures = true;
        a.workers[3] = std::static_pointer_cast<WorkerActor>(
            system.get_actor(system.spawn<WorkerActor>(cfg).id()));
    }

    // Wire addresses
    for (auto& w : a.workers) {
        w->set_aggregator_addr(a.aggregator->address());
        w->set_log_addr(a.log->address());
    }

    auto* health_raw = a.health_check.get();
    auto* broadcast_raw = a.broadcast.get();

    std::vector<ActorAddress> worker_addrs;
    for (auto& w : a.workers) {
        health_raw->add_worker(w->address());
        broadcast_raw->add_worker(w->address());
        worker_addrs.push_back(w->address());
    }

    auto* dlq_raw = a.dlq_demo.get();
    dlq_raw->set_target_actors(worker_addrs);

    auto* query_raw = a.query.get();
    query_raw->set_clock_addr(a.clock->address());

    return a;
}

void kickoff_cli_demo_actors(ActorSystem& system, const CliDemoActors& actors) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (auto& w : actors.workers) {
        deliver_local(system, w->id(), StartTag);
    }
    deliver_local(system, actors.health_check->id(), StartTag);
    deliver_local(system, actors.broadcast->id(), StartTag);
    deliver_local(system, actors.monitor->id(), StartTag);
    deliver_local(system, actors.clock->id(), PeriodicTickTag);
    deliver_local(system, actors.dlq_demo->id(), StartTag);
    deliver_local(system, actors.query->id(), StartTag);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

} // namespace hpactor::apps::cli_demo
