// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/adt/json_helpers.hpp>    // for JsonBuilder
#include <hpactor/net/http_connection.hpp> // for HTTPConnection
#include <hpactor/net/http_types.hpp> // for HttpMethod, HttpStatusCode, HttpRequest

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hpactor {
namespace cli {

class CliHttpServerActor;

// ── Route entry ──────────────────────────────────────────────────────

using RouteHandler = void (*)(CliHttpServerActor* actor,
                              net::HTTPConnection* conn, net::HttpRequest&& req);

struct RouteEntry {
    net::HttpMethod method;
    std::string pattern;
    RouteHandler handler;
};

// ── Route matching ───────────────────────────────────────────────────

bool match_route_pattern(const std::string& pattern, const std::string& path,
                         std::unordered_map<std::string, std::string>& path_params);

// ── HTTP response helpers ────────────────────────────────────────────

void send_error(net::HTTPConnection* conn, net::HttpStatusCode code,
                const std::string& error_code, const std::string& message);

void send_json_ok(net::HTTPConnection* conn, const std::string& json_body);

void send_json(net::HTTPConnection* conn, net::HttpStatusCode code,
               const std::string& json_body);

void send_accepted(net::HTTPConnection* conn, const std::string& message);

void send_success(net::HTTPConnection* conn);

// ── Query / path param parsers ───────────────────────────────────────

std::optional<uint64_t>
parse_path_uint64(const std::unordered_map<std::string, std::string>& params,
                  const std::string& key);

std::optional<std::string>
parse_query_string(const net::HttpRequest& req, const std::string& key);

std::optional<uint64_t>
parse_query_uint64(const net::HttpRequest& req, const std::string& key);

uint32_t parse_offset(const net::HttpRequest& req);

uint32_t parse_limit(const net::HttpRequest& req);

std::vector<std::string> parse_fields(const net::HttpRequest& req);

} // namespace cli
} // namespace hpactor
