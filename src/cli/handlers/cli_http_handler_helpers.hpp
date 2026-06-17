// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/adt/json_helpers.hpp>    // for JsonBuilder
#include <hpactor/net/http_connection.hpp> // for HTTPConnection
#include <hpactor/net/http_types.hpp> // for HttpMethod, HttpStatusCode, HttpRequest

#include <cstdint>
#include <optional>
#include <string>
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

// ── Pagination helper ─────────────────────────────────────────────────

/// Emit a "pagination" object with offset, limit, and total.
inline void add_pagination(adt::JsonBuilder& jb, uint32_t offset,
                           uint32_t limit, uint32_t total) {
    jb.object("pagination")
        .field("offset", offset)
        .field("limit", limit)
        .field("total", total)
        .end_object();
}

// ── Actor ID validation helper ──────────────────────────────────────────

/// Parse an actor_id from path params, returning nullopt and sending
/// a 400 error if missing, zero, or invalid.
inline std::optional<uint64_t>
parse_actor_id_or_error(const std::unordered_map<std::string, std::string>& path_params,
                        net::HTTPConnection* conn) {
    auto id_val = parse_path_uint64(path_params, "id");
    if (!id_val || *id_val == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return std::nullopt;
    }
    return *id_val;
}

// ── Content-Type validation ────────────────────────────────────────────

/// Returns true if Content-Type is JSON (or absent — be lenient).
/// Sends 415 error and returns false if Content-Type is set to non-JSON.
inline bool validate_json_content_type(net::HTTPConnection* conn,
                                       const net::HttpRequest& req) {
    auto ct = req.content_type();
    if (ct.has_value() && !ct->starts_with("application/json")) {
        send_error(conn, net::HttpStatusCode::UnsupportedMedia,
                   "UNSUPPORTED_MEDIA_TYPE",
                   "Content-Type must be application/json");
        return false;
    }
    return true;
}

} // namespace cli
} // namespace hpactor
