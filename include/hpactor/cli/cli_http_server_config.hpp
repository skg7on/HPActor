// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace hpactor {
namespace cli {

/// \brief Configuration for the HTTP JSON CLI server actor
/// (CliHttpServerActor).
///
/// Controls the HTTP listener for JSON-formatted CLI access.  Uses standard
/// HTTP request-response semantics: CLI commands are sent as JSON payloads
/// and responses are returned as JSON documents suitable for web-based tools,
/// dashboards, and scripting clients.
struct CliHttpServerConfig {
    /// \brief HTTP listen port for JSON CLI connections.
    uint16_t http_port = 9090;

    /// \brief HTTP bind address for the JSON CLI listener.
    std::string http_bind_address = "127.0.0.1";

    /// \brief Maximum concurrent HTTP connections.
    uint32_t max_connections = 100;

    /// \brief Default output format: "pretty", "json", or "tabular".
    std::string default_format = "pretty";

    /// \brief Number of items per page for paged output.
    uint32_t page_size = 50;

    /// \brief Enable the legacy POST /cli endpoint for backward compatibility.
    /// Set to false once all clients have migrated to the REST API.
    bool legacy_cli_endpoint = true;
};

} // namespace cli
} // namespace hpactor
