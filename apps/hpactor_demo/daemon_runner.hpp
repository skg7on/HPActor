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

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor {

class ActorSystem;

namespace apps::hpactor_demo {

struct DaemonConfig {
    std::string uds_path;
    uint16_t tcp_port = 0;
    uint16_t health_port = 8089;
    std::chrono::milliseconds watchdog_interval{0};
};

/// Run the actor system in daemon or systemd mode (Linux only).
/// On macOS, prints an error and returns 1.
int run_daemon(ActorSystem& system, const DaemonConfig& cfg);

} // namespace apps::hpactor_demo
} // namespace hpactor
