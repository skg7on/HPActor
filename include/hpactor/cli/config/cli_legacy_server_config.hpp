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

/// \brief Configuration for the socket-based CLI server (CliLegacyServerActor).
///
/// Controls UDS and TCP listener setup, session limits, timeouts, and
/// default output formatting. Designed for daemon-mode operation where
/// CLI input arrives over Unix domain sockets or TCP connections rather
/// than stdin/stdout.
struct CliLegacyServerConfig {
    /// \brief Unix domain socket path for CLI connections.
    ///        Empty means no UDS listener.
    std::string uds_listen_path;

    /// \brief Permission mode for the UDS socket file.
    uint32_t uds_socket_mode = 0660;

    /// \brief Owner user name for the UDS socket (optional).
    std::string uds_socket_owner;

    /// \brief Owner group name for the UDS socket (optional).
    std::string uds_socket_group;

    /// \brief TCP listen port. 0 means TCP listener is disabled.
    uint16_t tcp_listen_port = 0;

    /// \brief TCP bind address.
    std::string tcp_bind_address = "127.0.0.1";

    /// \brief Maximum concurrent CLI sessions.
    uint32_t max_sessions = 16;

    /// \brief Session idle timeout (default 5 minutes).
    std::chrono::milliseconds session_timeout{300000};

    /// \brief Default output format: "pretty", "json", or "tabular".
    std::string default_format = "pretty";

    /// \brief Number of items per page for paged output.
    uint32_t page_size = 50;
};

} // namespace cli
} // namespace hpactor
