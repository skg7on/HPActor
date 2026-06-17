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

#pragma once

#include <memory>

namespace hpactor {

class ActorSystem;

namespace apps::cli_demo {

// Forward declarations for all actor types
class WorkerActor;
class AggregatorActor;
class HealthCheckActor;
class BroadcastActor;
class ClockActor;
class LogActor;
class SystemMonitorActor;
class DlqDemoActor;
class QueryActor;

/// \brief All spawned cli_demo actors with their shared pointers.
///
/// Owns references to every actor spawned by \c spawn_cli_demo_actors().
/// Callers use this to kick off periodic work and access raw pointers
/// for post-spawn wiring.
struct CliDemoActors {
    std::shared_ptr<WorkerActor> workers[4];
    std::shared_ptr<AggregatorActor> aggregator;
    std::shared_ptr<HealthCheckActor> health_check;
    std::shared_ptr<BroadcastActor> broadcast;
    std::shared_ptr<ClockActor> clock;
    std::shared_ptr<LogActor> log;
    std::shared_ptr<SystemMonitorActor> monitor;
    std::shared_ptr<DlqDemoActor> dlq_demo;
    std::shared_ptr<QueryActor> query;
};

/// \brief Spawn all 10 cli_demo actors and wire them up.
///
/// Creates the same actor topology as \c 15_cli_demo.cpp:
/// - 4 x WorkerActor (Worker-1 at 100msg/s, Worker-2 at 500msg/s,
///   Worker-3 circuit breaker, Worker-4 delivery failures)
/// - 1 x AggregatorActor, HealthCheckActor, BroadcastActor, ClockActor,
///   LogActor, SystemMonitorActor, DlqDemoActor, QueryActor
///
/// Workers are wired to Aggregator and Log. HealthCheck + Broadcast are
/// wired to all workers. DlqDemo is wired to all workers. QueryActor is
/// wired to Clock.
///
/// \param[in] system The actor system to spawn into.
/// \return All spawned actors with their shared pointers.
CliDemoActors spawn_cli_demo_actors(ActorSystem& system);

/// \brief Send StartTag and PeriodicTickTag messages to kick off periodic work.
///
/// Must be called after \c spawn_cli_demo_actors().  \c spawn() is
/// synchronous — mailboxes and behaviors are fully initialized before it
/// returns, so no pre-kickoff delay is needed.  A brief yield after
/// sending messages gives scheduler workers a chance to pick up enqueued
/// work before the CLI starts.
///
/// \param[in] system The actor system for local delivery.
/// \param[in] actors The actors returned by \c spawn_cli_demo_actors().
void kickoff_cli_demo_actors(ActorSystem& system, const CliDemoActors& actors);

} // namespace apps::cli_demo
} // namespace hpactor
