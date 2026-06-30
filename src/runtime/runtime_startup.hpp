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

#include "runtime_coordinator.hpp"

namespace hpactor {

class ActorSystem;

/// \brief Registers the real component startup stages on a coordinator.
///
/// Each stage has a start action and a rollback action. The coordinator
/// executes them in order. On failure, completed stages are rolled back
/// in reverse order.
///
/// Stages are:
/// 1. Process preflight (daemonization — before any threads)
/// 2. Telemetry (logging, metrics, tracing)
/// 3. Scheduler (worker threads)
/// 4. Messaging runtime activation
/// 5. System actors (SpawnReceiver, MetricsActor, etc.)
/// 6. Network prepare (bind/listen, do not publish yet)
/// 7. Network activate (discovery, external ingress)
/// 8. Readiness (accept work)
///
/// \param[in,out] coord       Coordinator to register stages on.
/// \param[in]     system      The actor system whose components are started.
/// \param[in]     enable_net  Whether networking is enabled.
void register_runtime_startup_stages(RuntimeCoordinator& coord,
                                     ActorSystem& system, bool enable_net) noexcept;

} // namespace hpactor
