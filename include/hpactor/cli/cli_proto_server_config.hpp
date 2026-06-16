// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace hpactor {
namespace cli {

/// \brief Configuration for the protobuf CLI server actor
/// (CliProtoServerActor).
///
/// Controls the Unix domain socket and TCP listeners for the binary protobuf
/// CLI protocol.  Supports concurrent sessions with configurable limits and
/// idle timeouts.  Designed for programmatic and administrative access where
/// structured binary CLI commands are dispatched over persistent connections.
struct CliProtoServerConfig {
    /// \brief Unix domain socket path for protobuf CLI connections.
    ///        Empty means no UDS listener.
    std::string uds_listen_path;

    /// \brief TCP listen port. 0 means TCP listener is disabled.
    uint16_t tcp_listen_port = 0;

    /// \brief TCP bind address for the protobuf CLI listener.
    std::string tcp_bind_address = "127.0.0.1";

    /// \brief Maximum concurrent protobuf CLI sessions.
    uint32_t max_sessions = 16;

    /// \brief Session idle timeout (default 5 minutes).
    std::chrono::milliseconds session_timeout{300000};

    /// \brief Default output format: "pretty", "json", or "tabular".
    std::string default_format = "pretty";

    /// \brief Number of items per page for paged output.
    uint32_t page_size = 50;

    /// \brief Permission mode for the UDS socket file.
    uint32_t uds_socket_mode = 0660;

    /// \brief Owner user name for the UDS socket (optional).
    std::string uds_socket_owner;

    /// \brief Owner group name for the UDS socket (optional).
    std::string uds_socket_group;
};

} // namespace cli
} // namespace hpactor
