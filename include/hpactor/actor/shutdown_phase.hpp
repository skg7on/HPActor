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

#include <cstdint>

namespace hpactor {

enum class ShutdownPhase : uint8_t {
    Running,           ///< Normal operation.
    DrainingIngress,   ///< Refusing new external connections and messages.
    DrainingActors,    ///< Draining in-flight actor messages per policy.
    LeavingCluster,    ///< Notifying peers and handing off shards.
    FlushingTelemetry, ///< Flushing metrics, logs, and traces.
    Stopped,           ///< Clean shutdown complete.
    ForcedStop,        ///< Force-stopped after timeout.
};

} // namespace hpactor
