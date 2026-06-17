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

#include <hpactor/cli/cli_http_server_actor.hpp>

#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/cli.pb.h>

#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>

#include <string>

namespace hpactor {
namespace cli {
namespace handlers {

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no protobuf JSON util dependency)
// ---------------------------------------------------------------------------

/// Alias ADT helpers into the anonymous namespace so the protobuf-aware
/// parse/serialize functions below can call them without qualification.
namespace {

using adt::extract_json_array_raw;
using adt::extract_json_object_raw;
using adt::extract_json_string;
using adt::json_escape;
using adt::parse_json_string_array;
using adt::parse_json_string_map;
using adt::skip_json_ws;

/// Parse a JSON object to populate a CliCommand's fields.
/// Returns true on success, false on parse failure.
bool parse_cli_command_json(const std::string& json, hpactor::cli::CliCommand& cmd) {
    size_t pos = 0;
    pos = skip_json_ws(json, pos);
    if (pos >= json.size() || json[pos] != '{')
        return false;
    ++pos;

    std::string current_key;
    while (pos < json.size()) {
        pos = skip_json_ws(json, pos);
        if (pos >= json.size())
            break;
        if (json[pos] == '}') {
            ++pos;
            break;
        }
        if (json[pos] == ',') {
            ++pos;
            current_key.clear();
            continue;
        }
        if (json[pos] == '"' && current_key.empty()) {
            // Reading a key
            current_key = extract_json_string(json, pos);
            pos = skip_json_ws(json, pos);
            if (pos < json.size() && json[pos] == ':')
                ++pos;
            pos = skip_json_ws(json, pos);
            if (pos >= json.size())
                break;

            if (current_key == "path") {
                if (json[pos] == '"') {
                    cmd.set_path(extract_json_string(json, pos));
                }
                current_key.clear();
            } else if (current_key == "format") {
                if (json[pos] == '"') {
                    cmd.set_format(extract_json_string(json, pos));
                }
                current_key.clear();
            } else if (current_key == "params") {
                if (json[pos] == '{') {
                    auto obj_raw = extract_json_object_raw(json, pos);
                    auto pairs = parse_json_string_map(obj_raw);
                    auto* params_map = cmd.mutable_params();
                    for (auto& [k, v] : pairs) {
                        (*params_map)[k] = v;
                    }
                }
                current_key.clear();
            } else if (current_key == "args") {
                if (json[pos] == '[') {
                    auto arr_raw = extract_json_array_raw(json, pos);
                    auto values = parse_json_string_array(arr_raw);
                    for (auto& v : values) {
                        cmd.add_args(std::move(v));
                    }
                }
                current_key.clear();
            } else {
                // Skip unknown field
                if (json[pos] == '"') {
                    skip_json_ws(json, pos); // dummy — extract_json_string
                                             // advances
                    // Reset pos to where we were, then skip the value
                }
                // Skip the value (string, object, array, or literal)
                if (pos < json.size() && json[pos] == '"') {
                    extract_json_string(json, pos);
                } else if (pos < json.size() && json[pos] == '{') {
                    extract_json_object_raw(json, pos);
                } else if (pos < json.size() && json[pos] == '[') {
                    extract_json_array_raw(json, pos);
                } else {
                    // Skip literal (number, true, false, null)
                    while (pos < json.size() && json[pos] != ',' &&
                           json[pos] != '}') {
                        ++pos;
                    }
                }
                current_key.clear();
            }
        } else {
            // Unexpected; skip one character
            ++pos;
        }
    }
    return true;
}

/// Serialize a CliResponse to a JSON string.
std::string serialize_cli_response_json(const hpactor::cli::CliResponse& resp) {
    std::string json;
    json = "{";
    json += "\"content_type\":\"" + json_escape(resp.content_type()) + "\",";
    json += "\"payload\":\"" + json_escape(resp.payload()) + "\",";
    json += "\"is_error\":" + std::string(resp.is_error() ? "true" : "false") + ",";
    json += "\"error_code\":" + std::to_string(resp.error_code()) + ",";
    json += "\"is_structured\":" +
            std::string(resp.is_structured() ? "true" : "false");
    json += "}";
    return json;
}

/// Send a JSON CliResponse as an HTTP response.
void send_json_response(net::HTTPConnection* conn, net::HttpStatusCode http_code,
                        const hpactor::cli::CliResponse& resp) {
    std::string json_out = serialize_cli_response_json(resp);
    StreamBuffer body_buf(
        reinterpret_cast<const uint8_t*>(json_out.data()),
        reinterpret_cast<const uint8_t*>(json_out.data() + json_out.size()));
    conn->send_response(http_code, {{"Content-Type", "application/json"}}, body_buf);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Legacy POST /cli handler
// ---------------------------------------------------------------------------

void handle_legacy_post_cli(CliHttpServerActor* actor,
                            net::HTTPConnection* conn, net::HttpRequest&& req) {
    // Extract body text
    std::string body_str(reinterpret_cast<const char*>(req.body.data()),
                         req.body.size());

    // Validate JSON Content-Type
    {
        auto ct = req.content_type();
        if (ct.has_value() && ct->find("application/json") == std::string::npos) {
            hpactor::cli::CliResponse err_resp;
            err_resp.set_content_type("text/plain");
            err_resp.set_payload("Content-Type must be application/json");
            err_resp.set_is_error(true);
            err_resp.set_error_code(415);
            err_resp.set_is_structured(false);
            send_json_response(conn, net::HttpStatusCode::UnsupportedMedia,
                               err_resp);
            return;
        }
    }

    // Parse JSON body -> CliCommand
    hpactor::cli::CliCommand cmd;
    if (!parse_cli_command_json(body_str, cmd)) {
        hpactor::cli::CliResponse err_resp;
        err_resp.set_content_type("text/plain");
        err_resp.set_payload("Invalid JSON in request body");
        err_resp.set_is_error(true);
        err_resp.set_error_code(400);
        err_resp.set_is_structured(false);
        send_json_response(conn, net::HttpStatusCode::BadRequest, err_resp);
        return;
    }

    // Reconstruct command line from CliCommand fields.
    // Path uses "/" as separator: "actor/42/show" -> tokens "actor 42 show".
    std::string cmd_line = "/";
    if (!cmd.path().empty()) {
        for (char c : cmd.path()) {
            if (c == '/')
                cmd_line += ' ';
            else
                cmd_line += c;
        }
    }

    // Params: map of captured <param> values and --flags.
    for (const auto& [key, value] : cmd.params()) {
        if (value == "true") {
            cmd_line += " --" + key;
        } else {
            cmd_line += " --" + key + " " + value;
        }
    }

    // Format: explicit --format flag.
    std::string format = cmd.format();
    if (!format.empty()) {
        cmd_line += " --format " + format;
    }

    // Positional args.
    for (const auto& arg : cmd.args()) {
        cmd_line += " " + arg;
    }

    // Determine content_type for the CliResponse payload.
    std::string content_type = "text/plain";
    if (format == "json") {
        content_type = "application/json";
    }

    // Execute the command via CliSession.
    const auto& config = actor->config();
    std::string output;
    {
        auto session = std::make_unique<CliSession>(
            &actor->system(), actor->command_tree(),
            OutputFormatter::create(config.default_format),
            [&output](const std::string& text) { output = text; },
            config.page_size);
        session->set_system_host(actor);
        session->set_lifecycle_host(actor);
        session->set_command_host(actor);

        session->process_line(cmd_line);
    }

    // Strip trailing newline appended by process_line.
    if (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }

    // Build JSON CliResponse.
    hpactor::cli::CliResponse resp;
    resp.set_content_type(content_type);
    resp.set_payload(output);
    resp.set_is_error(false);
    resp.set_error_code(0);
    resp.set_is_structured(false);

    send_json_response(conn, net::HttpStatusCode::OK, resp);
}

} // namespace handlers
} // namespace cli
} // namespace hpactor
