// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include "cli_http_handler_helpers.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace hpactor {
namespace cli {

// ── Route matching ───────────────────────────────────────────────────

bool match_route_pattern(const std::string& pattern, const std::string& path,
                         std::unordered_map<std::string, std::string>& path_params) {
    // Split a path into non-empty segments.
    auto split = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> result;
        if (s.empty())
            return result;

        size_t start = (s[0] == '/') ? 1 : 0;
        if (start >= s.size())
            return result; // path was just "/"

        size_t end = start;
        while (end <= s.size()) {
            if (end == s.size() || s[end] == '/') {
                if (end > start) {
                    result.emplace_back(s.substr(start, end - start));
                }
                start = end + 1;
            }
            ++end;
        }
        return result;
    };

    auto pattern_segs = split(pattern);
    auto path_segs = split(path);

    if (pattern_segs.size() != path_segs.size())
        return false;

    for (size_t i = 0; i < pattern_segs.size(); ++i) {
        const auto& ps = pattern_segs[i];
        const auto& xs = path_segs[i];

        if (!ps.empty() && ps[0] == ':') {
            // Named parameter — capture the path segment value.
            path_params[ps.substr(1)] = xs;
        } else if (ps != xs) {
            return false;
        }
    }

    return true;
}

// ── HTTP response helpers ────────────────────────────────────────────

void send_error(net::HTTPConnection* conn, net::HttpStatusCode code,
                const std::string& error_code, const std::string& message) {
    std::string json = adt::JsonBuilder::root_object()
                           .object("error")
                           .field("code", error_code)
                           .field("message", message)
                           .end_object()
                           .end_object()
                           .build();
    send_json(conn, code, json);
}

void send_json_ok(net::HTTPConnection* conn, const std::string& json_body) {
    send_json(conn, net::HttpStatusCode::OK, json_body);
}

void send_json(net::HTTPConnection* conn, net::HttpStatusCode code,
               const std::string& json_body) {
    StreamBuffer body;
    body.append(reinterpret_cast<const uint8_t*>(json_body.data()),
                json_body.size());
    conn->send_response(
        code, {net::HttpHeader{"Content-Type", "application/json"}}, body);
}

void send_accepted(net::HTTPConnection* conn, const std::string& message) {
    std::string json = adt::JsonBuilder::root_object()
                           .object("data")
                           .field("success", true)
                           .field("message", message)
                           .end_object()
                           .end_object()
                           .build();
    send_json(conn, net::HttpStatusCode::Accepted, json);
}

void send_success(net::HTTPConnection* conn) {
    std::string json = adt::JsonBuilder::root_object()
                           .object("data")
                           .field("success", true)
                           .end_object()
                           .end_object()
                           .build();
    send_json(conn, net::HttpStatusCode::OK, json);
}

// ── Query / path param parsers ───────────────────────────────────────

std::optional<uint64_t>
parse_path_uint64(const std::unordered_map<std::string, std::string>& params,
                  const std::string& key) {
    auto it = params.find(key);
    if (it == params.end())
        return std::nullopt;
    if (it->second.empty())
        return std::nullopt;

    char* end = nullptr;
    uint64_t val = std::strtoull(it->second.c_str(), &end, 10);
    if (*end != '\0')
        return std::nullopt;
    return val;
}

std::optional<std::string>
parse_query_string(const net::HttpRequest& req, const std::string& key) {
    auto it = req.query_params.find(key);
    if (it == req.query_params.end())
        return std::nullopt;
    return it->second;
}

std::optional<uint64_t>
parse_query_uint64(const net::HttpRequest& req, const std::string& key) {
    auto str = parse_query_string(req, key);
    if (!str || str->empty())
        return std::nullopt;

    char* end = nullptr;
    uint64_t val = std::strtoull(str->c_str(), &end, 10);
    if (*end != '\0')
        return std::nullopt;
    return val;
}

uint32_t parse_offset(const net::HttpRequest& req) {
    auto val = parse_query_uint64(req, "offset");
    if (!val)
        return 0;
    if (*val > UINT32_MAX)
        return UINT32_MAX;
    return static_cast<uint32_t>(*val);
}

uint32_t parse_limit(const net::HttpRequest& req) {
    auto val = parse_query_uint64(req, "limit");
    if (!val)
        return 50;
    if (*val < 1)
        return 1;
    if (*val > 200)
        return 200;
    return static_cast<uint32_t>(*val);
}

std::vector<std::string> parse_fields(const net::HttpRequest& req) {
    auto str = parse_query_string(req, "fields");
    if (!str || str->empty())
        return {};

    std::vector<std::string> result;
    std::istringstream ss(*str);
    std::string field;
    while (std::getline(ss, field, ',')) {
        // Trim leading whitespace.
        size_t start = 0;
        while (start < field.size() &&
               std::isspace(static_cast<unsigned char>(field[start]))) {
            ++start;
        }
        // Trim trailing whitespace.
        size_t end = field.size();
        while (end > start &&
               std::isspace(static_cast<unsigned char>(field[end - 1]))) {
            --end;
        }
        if (start < end) {
            result.emplace_back(field.substr(start, end - start));
        }
    }
    return result;
}

} // namespace cli
} // namespace hpactor
