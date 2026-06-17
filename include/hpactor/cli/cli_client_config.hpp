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
namespace cli {

/// \brief Configuration for the remote CLI client (CliClientActor).
struct CliClientConfig {
    /// \brief Transport protocol selection.
    enum class Transport { Protobuf, HttpJson };

    // Transport
    std::string uds_path = "/tmp/hpactor/hpactor.sock";
    std::string host;       // empty = use UDS
    uint16_t port = 0;      // required with host
    uint16_t http_port = 0; // optional HTTP JSON transport
    Transport transport = Transport::Protobuf;

    // Session
    std::string history_path; // "" = $HOME/.hpactor_cli_history
    uint32_t history_max = 1000;
    std::string default_format = "pretty";

    // Timing
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{10000};

    // Reconnection
    std::chrono::milliseconds reconnect_min{1000};
    std::chrono::milliseconds reconnect_max{30000};
};

} // namespace cli
} // namespace hpactor
